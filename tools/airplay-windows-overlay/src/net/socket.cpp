#include "net/socket.h"
#include "log.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <vector>

#if defined(_WIN32)
    #include <iphlpapi.h>
    #include <mstcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "iphlpapi.lib")
#else
    #include <ifaddrs.h>
    #include <net/if.h>
    #include <netdb.h>
    #include <netinet/tcp.h>
    #include <errno.h>
#endif

namespace ap::net {

bool global_init() {
#if defined(_WIN32)
    WSADATA wsa;
    int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0) {
        LOG_ERROR << "WSAStartup failed: " << rc;
        return false;
    }
#endif
    return true;
}

void global_shutdown() {
#if defined(_WIN32)
    WSACleanup();
#endif
}

void close_socket(socket_t s) {
    if (s == INVALID_SOCK) return;
#if defined(_WIN32)
    closesocket(s);
#else
    ::close(s);
#endif
}

bool enable_tcp_keepalive(socket_t s, int idle_seconds, int interval_seconds, int probes) {
    const int enabled = 1;
    const auto set = [s](int level, int option, const int& value) {
        return ::setsockopt(s, level, option,
            reinterpret_cast<const char*>(&value), sizeof(value)) == 0;
    };
    return set(SOL_SOCKET, SO_KEEPALIVE, enabled)
        && set(IPPROTO_TCP, TCP_KEEPIDLE, idle_seconds)
        && set(IPPROTO_TCP, TCP_KEEPINTVL, interval_seconds)
        && set(IPPROTO_TCP, TCP_KEEPCNT, probes);
}

int recv_interruptible(socket_t s, void* buf, int len, const std::atomic<bool>* cancelled) {
    // shutdown() alone need not cancel a pending synchronous Winsock recv.
    // Poll readiness so cancellation is observed without closing/reusing a
    // descriptor while its worker may still be reading it.
    while (!cancelled || !cancelled->load()) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(s, &readable);
        timeval timeout{0, 100000};
        const int ready = ::select(static_cast<int>(s) + 1, &readable, nullptr, nullptr, &timeout);
        if (cancelled && cancelled->load()) return -1;
        if (ready < 0) return -1;
        if (ready > 0) return ::recv(s, static_cast<char*>(buf), len, 0);
    }
    return -1;
}

int last_error() {
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

std::string last_error_string() {
    int err = last_error();
#if defined(_WIN32)
    char* buf = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, reinterpret_cast<char*>(&buf), 0, nullptr);
    std::string msg = buf ? buf : "";
    if (buf) LocalFree(buf);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
        msg.pop_back();
    return msg + " (" + std::to_string(err) + ")";
#else
    return std::string(strerror(err)) + " (" + std::to_string(err) + ")";
#endif
}

int recv_all(socket_t s, void* buf, int len) {
    auto* p = static_cast<char*>(buf);
    int got = 0;
    while (got < len) {
        int n = ::recv(s, p + got, len - got, 0);
        if (n == 0) return got;   // peer closed
        if (n < 0)  return -1;
        got += n;
    }
    return got;
}

int send_all(socket_t s, const void* buf, int len) {
    const auto* p = static_cast<const char*>(buf);
    int sent = 0;
    while (sent < len) {
        int n = ::send(s, p + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return sent;
}

// "Connect" a UDP socket to a public address and read back the local address
// the OS routing table assigned. No packet is actually sent — `connect` on a
// UDP socket only selects the outbound interface. This bypasses virtual
// adapters (Hyper-V, VMware, WSL) that would otherwise be picked by a naive
// "first non-loopback" enumeration.
namespace {
std::string primary_ipv4_via_route() {
    // Intentional: this helper is v4-specific by design. The IPv6
    // equivalent lives in primary_ipv6_via_route() below.
    socket_t s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCK) return "0.0.0.0";

    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port   = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &peer.sin_addr);   // v4-only probe

    std::string result = "0.0.0.0";
    if (::connect(s, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) == 0) {
        sockaddr_in local{};
#if defined(_WIN32)
        int len = sizeof(local);
#else
        socklen_t len = sizeof(local);
#endif
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
            char buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf));
            result = buf;
        }
    }
    close_socket(s);
    return result;
}

// Same trick over IPv6: UDP-connect to Google Public DNS v6 and read
// back the local address the OS route table chose. Skip link-local
// (fe80::) — iOS needs a globally-routable address to connect back to.
std::string primary_ipv6_via_route() {
    socket_t s = ::socket(AF_INET6, SOCK_DGRAM, 0);
    if (s == INVALID_SOCK) return {};

    sockaddr_in6 peer{};
    peer.sin6_family = AF_INET6;
    peer.sin6_port   = htons(53);
    inet_pton(AF_INET6, "2001:4860:4860::8888", &peer.sin6_addr);

    std::string result;
    if (::connect(s, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) == 0) {
        sockaddr_in6 local{};
#if defined(_WIN32)
        int len = sizeof(local);
#else
        socklen_t len = sizeof(local);
#endif
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
            char buf[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &local.sin6_addr, buf, sizeof(buf));
            // Skip link-local; unusable from outside our own subnet.
            if (std::strncmp(buf, "fe80", 4) != 0 &&
                std::strncmp(buf, "::",   2) != 0) {
                result = buf;
            }
        }
    }
    close_socket(s);
    return result;
}
} // namespace

std::string primary_ipv6() {
    return primary_ipv6_via_route();
}

#if defined(_WIN32)

namespace {

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool is_private_ipv4(const sockaddr_in& address) {
    const uint32_t value = ntohl(address.sin_addr.s_addr);
    return (value & 0xFF000000U) == 0x0A000000U ||
           (value & 0xFFF00000U) == 0xAC100000U ||
           (value & 0xFFFF0000U) == 0xC0A80000U;
}

bool looks_virtual_adapter(const std::string& description,
                           const std::string& friendly_name) {
    const std::string text =
        lower_ascii(description + " " + friendly_name);
    static const char* blocked[] = {
        "virtual", "vpn", "vethernet", "hyper-v", "tap-", "wireguard",
        "zerotier", "clash", "todesk", "wsl", "loopback"
    };
    return std::any_of(std::begin(blocked), std::end(blocked),
        [&](const char* word) { return text.find(word) != std::string::npos; });
}

std::string narrow_wide(const wchar_t* value) {
    if (!value || !*value) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string out(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value, -1, out.data(), size, nullptr, nullptr);
    return out;
}

std::string adapter_mac(const IP_ADAPTER_ADDRESSES& adapter) {
    if (adapter.PhysicalAddressLength != 6) return {};
    char value[18]{};
    std::snprintf(value, sizeof(value), "%02X:%02X:%02X:%02X:%02X:%02X",
        adapter.PhysicalAddress[0], adapter.PhysicalAddress[1],
        adapter.PhysicalAddress[2], adapter.PhysicalAddress[3],
        adapter.PhysicalAddress[4], adapter.PhysicalAddress[5]);
    return value;
}

} // namespace

LanInterface primary_lan_interface() {
    LanInterface best;
    int best_score = -1;

    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    DWORD rc = GetAdaptersAddresses(
        AF_UNSPEC,
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
            GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, adapters, &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        rc = GetAdaptersAddresses(
            AF_UNSPEC,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, adapters, &size);
    }
    if (rc != NO_ERROR) return best;

    for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp ||
            adapter->IfType != IF_TYPE_IEEE80211 &&
            adapter->IfType != IF_TYPE_ETHERNET_CSMACD) {
            continue;
        }

        const std::string description =
            narrow_wide(adapter->Description);
        const std::string friendly =
            narrow_wide(adapter->FriendlyName);
        if (looks_virtual_adapter(description, friendly)) continue;

        LanInterface candidate;
        candidate.index = adapter->IfIndex;
        candidate.mac = adapter_mac(*adapter);
        bool has_private_ipv4 = false;
        for (auto* unicast = adapter->FirstUnicastAddress;
             unicast; unicast = unicast->Next) {
            const sockaddr* address = unicast->Address.lpSockaddr;
            if (!address) continue;
            char text[INET6_ADDRSTRLEN]{};
            if (address->sa_family == AF_INET) {
                const auto* v4 =
                    reinterpret_cast<const sockaddr_in*>(address);
                if (!is_private_ipv4(*v4)) continue;
                inet_ntop(AF_INET, &v4->sin_addr, text, sizeof(text));
                candidate.ipv4 = text;
                has_private_ipv4 = true;
            } else if (address->sa_family == AF_INET6 &&
                       candidate.ipv6.empty()) {
                const auto* v6 =
                    reinterpret_cast<const sockaddr_in6*>(address);
                if (IN6_IS_ADDR_LINKLOCAL(&v6->sin6_addr) ||
                    IN6_IS_ADDR_LOOPBACK(&v6->sin6_addr)) {
                    continue;
                }
                inet_ntop(AF_INET6, &v6->sin6_addr, text, sizeof(text));
                candidate.ipv6 = text;
            }
        }

        if (candidate.ipv4.empty()) continue;
        int score = adapter->IfType == IF_TYPE_IEEE80211 ? 100 : 80;
        if (has_private_ipv4) score += 20;
        if (adapter->FirstGatewayAddress) score += 30;
        if (!candidate.mac.empty()) score += 10;
        if (score > best_score) {
            best_score = score;
            best = std::move(candidate);
        }
    }
    return best;
}

std::string primary_ipv4() {
    return primary_ipv4_via_route();
}

std::string primary_mac() {
    const LanInterface lan = primary_lan_interface();
    if (!lan.mac.empty()) return lan.mac;

    // Find the adapter whose IP (v4 or v6) matches the one we advertise
    // and return its MAC. iOS correlates `deviceid` (MAC) with the RTSP
    // socket's local IP, so the two must come from the same interface.
    // On an IPv6-only network primary_ipv4() is "0.0.0.0" — we'd never
    // match anything and fall back to a bogus 02:00:... MAC that iOS
    // refuses to re-pair with. Enumerate both families and match either.
    const std::string target_v4 = primary_ipv4();
    const std::string target_v6 = primary_ipv6();

    ULONG size = 15 * 1024;
    std::vector<unsigned char> buf(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    DWORD rc = GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, adapters, &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        rc = GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, adapters, &size);
    }
    if (rc != NO_ERROR) return "02:00:00:00:00:00";

    for (auto* a = adapters; a; a = a->Next) {
        if (a->PhysicalAddressLength != 6) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            const auto* sa = u->Address.lpSockaddr;
            if (!sa) continue;
            char ip[INET6_ADDRSTRLEN] = {0};
            if (sa->sa_family == AF_INET) {
                const auto* in4 = reinterpret_cast<const sockaddr_in*>(sa);
                inet_ntop(AF_INET, &in4->sin_addr, ip, sizeof(ip));
            } else if (sa->sa_family == AF_INET6) {
                const auto* in6 = reinterpret_cast<const sockaddr_in6*>(sa);
                inet_ntop(AF_INET6, &in6->sin6_addr, ip, sizeof(ip));
            } else {
                continue;
            }
            const bool match =
                (!target_v4.empty() && target_v4 != "0.0.0.0" && target_v4 == ip) ||
                (!target_v6.empty() && target_v6 == ip);
            if (match) {
                char out[18];
                std::snprintf(out, sizeof(out),
                              "%02X:%02X:%02X:%02X:%02X:%02X",
                              a->PhysicalAddress[0], a->PhysicalAddress[1],
                              a->PhysicalAddress[2], a->PhysicalAddress[3],
                              a->PhysicalAddress[4], a->PhysicalAddress[5]);
                return out;
            }
        }
    }
    return "02:00:00:00:00:00";
}

#else

std::string primary_ipv4() {
    return primary_ipv4_via_route();
}

std::string primary_mac() {
    // Minimal fallback for non-Windows development builds.
    return "02:00:00:00:00:00";
}

#endif

} // namespace ap::net
