#include "airplay/ptp_listener.h"

#include "log.h"
#include "net/socket.h"

#include <chrono>
#include <cstring>

#if defined(_WIN32)
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/select.h>
#endif

namespace ap::airplay {
namespace {

constexpr uint16_t kPtPEventPort = 319;
constexpr uint16_t kPtpGeneralPort = 320;
constexpr int64_t kPtpSampleMaxAgeNs = 2'000'000'000LL;

uint64_t read_be64(const unsigned char* data) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | data[i];
    }
    return value;
}

int64_t local_steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int64_t ptp_timestamp_ns(const unsigned char* timestamp) {
    uint64_t seconds = 0;
    for (int i = 0; i < 6; ++i) {
        seconds = (seconds << 8) | timestamp[i];
    }
    const uint32_t nanoseconds =
        (static_cast<uint32_t>(timestamp[6]) << 24) |
        (static_cast<uint32_t>(timestamp[7]) << 16) |
        (static_cast<uint32_t>(timestamp[8]) << 8) |
        static_cast<uint32_t>(timestamp[9]);
    return static_cast<int64_t>(seconds * UINT64_C(1'000'000'000) +
                                nanoseconds);
}

socket_t open_ptp_socket(uint16_t port, const std::string& local_ip) {
    socket_t socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_SOCK) return INVALID_SOCK;
    const int reuse = 1;
    ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(socket, reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) != 0) {
        ap::net::close_socket(socket);
        return INVALID_SOCK;
    }

    in_addr local{};
    InetPtonA(AF_INET, local_ip.c_str(), &local);
    ip_mreq membership{};
    InetPtonA(AF_INET, "224.0.1.129", &membership.imr_multiaddr);
    membership.imr_interface = local;
    ::setsockopt(socket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                 reinterpret_cast<const char*>(&membership),
                 sizeof(membership));
    ::setsockopt(socket, IPPROTO_IP, IP_MULTICAST_IF,
                 reinterpret_cast<const char*>(&local), sizeof(local));
    return socket;
}

} // namespace

PtpListener::PtpListener() = default;

PtpListener::~PtpListener() {
    stop();
}

bool PtpListener::start() {
    if (running_.load()) return true;
    const auto lan = ap::net::primary_lan_interface();
    if (lan.index == 0 || lan.ipv4.empty()) {
        LOG_WARN << "PTP timing unavailable: no physical LAN interface";
        return false;
    }

    socket_t event = open_ptp_socket(kPtPEventPort, lan.ipv4);
    socket_t general = open_ptp_socket(kPtpGeneralPort, lan.ipv4);
    if (event == INVALID_SOCK && general == INVALID_SOCK) {
        LOG_WARN << "PTP timing unavailable: UDP 319/320 cannot be bound";
        return false;
    }
    if (event != INVALID_SOCK) sockets_.push_back(event);
    if (general != INVALID_SOCK) sockets_.push_back(general);
    running_.store(true);
    thread_ = std::thread(&PtpListener::run, this);
    LOG_INFO << "AP2 PTP listener active on " << lan.ipv4
             << " (UDP 319/320)";
    return true;
}

void PtpListener::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    for (socket_t socket : sockets_) ap::net::close_socket(socket);
    sockets_.clear();
    ready_.store(false);
}

bool PtpListener::ready() const {
    if (!ready_.load()) return false;
    std::lock_guard<std::mutex> lock(sample_mutex_);
    const auto it = clock_samples_.find(latest_clock_id_);
    return it != clock_samples_.end() &&
           local_steady_ns() - static_cast<int64_t>(it->second.local_ns) <=
               kPtpSampleMaxAgeNs;
}

bool PtpListener::master_to_local(
    uint64_t master_ns,
    uint64_t& local_ns,
    uint64_t expected_clock_id) const {
    std::lock_guard<std::mutex> lock(sample_mutex_);
    const uint64_t selected =
        expected_clock_id != 0 ? expected_clock_id : latest_clock_id_;
    const auto it = clock_samples_.find(selected);
    if (it == clock_samples_.end() ||
        local_steady_ns() - static_cast<int64_t>(it->second.local_ns) >
            kPtpSampleMaxAgeNs) {
        return false;
    }
    const int64_t converted =
        static_cast<int64_t>(master_ns) -
        it->second.master_minus_local_ns;
    if (converted < 0) return false;
    local_ns = static_cast<uint64_t>(converted);
    return true;
}

uint64_t PtpListener::clock_id() const {
    std::lock_guard<std::mutex> lock(sample_mutex_);
    return latest_clock_id_;
}

void PtpListener::process_packet(
    const unsigned char* data, std::size_t size) {
    if (size < 44) return;
    const unsigned char message_type = data[0] & 0x0F;
    if (message_type != 0 && message_type != 8 &&
        message_type != 11) {
        return;
    }

    const uint64_t clock_id = read_be64(data + 20);
    if (message_type == 8) {
        // 当前实现只消费两-step PTP 的 Follow_Up preciseOriginTimestamp。
        // correctionField 采用 48.16 定点纳秒，可吸收交换机驻留时间；
        // 单机场景暂不发送 Delay_Req，因此仍有 LAN 单向传播误差。
        const int64_t correction =
            static_cast<int64_t>(read_be64(data + 8)) / 65536;
        const int64_t master_ns =
            ptp_timestamp_ns(data + 34) + correction;
        const int64_t local_ns = local_steady_ns();
        {
            std::lock_guard<std::mutex> lock(sample_mutex_);
            latest_clock_id_ = clock_id;
            clock_samples_[clock_id] = ClockSample{
                master_ns - local_ns,
                static_cast<uint64_t>(local_ns)};
            for (auto it = clock_samples_.begin();
                 it != clock_samples_.end();) {
                if (local_ns - static_cast<int64_t>(it->second.local_ns) >
                    5 * kPtpSampleMaxAgeNs) {
                    it = clock_samples_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        if (!ready_.exchange(true)) {
            LOG_INFO << "AP2 PTP clock synchronization acquired";
        }
    }
}

void PtpListener::run() {
    while (running_.load()) {
        fd_set readable;
        FD_ZERO(&readable);
        SOCKET highest = 0;
        for (socket_t socket : sockets_) {
            FD_SET(socket, &readable);
            if (socket > highest) highest = socket;
        }
        timeval timeout{0, 250000};
        const int result = ::select(
            static_cast<int>(highest) + 1,
            &readable, nullptr, nullptr, &timeout);
        if (!running_.load()) break;
        if (result <= 0) continue;

        for (socket_t socket : sockets_) {
            if (!FD_ISSET(socket, &readable)) continue;
            unsigned char packet[1500];
            const int received = ::recv(
                socket, reinterpret_cast<char*>(packet),
                sizeof(packet), 0);
            if (received > 0) {
                process_packet(
                    packet, static_cast<std::size_t>(received));
            }
        }
    }
}

} // namespace ap::airplay
