#include "airplay/ptp_listener.h"
#include "net/socket.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

#include <winsock2.h>
#include <ws2tcpip.h>

int main() {
    if (!ap::net::global_init()) return 1;

    for (uint16_t port : {static_cast<uint16_t>(319),
                          static_cast<uint16_t>(320)}) {
        socket_t probe = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (probe == INVALID_SOCK) continue;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        const bool available =
            ::bind(probe, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)) == 0;
        ap::net::close_socket(probe);
        if (!available) {
            std::cout << "SKIP: UDP " << port
                      << " is owned by a running receiver\n";
            ap::net::global_shutdown();
            return 0;
        }
    }

    ap::airplay::PtpListener listener;
    if (!listener.start()) {
        std::cout << "SKIP: UDP 319/320 are already owned by a running "
                     "receiver\n";
        ap::net::global_shutdown();
        return 0;
    }

    socket_t sender = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sender == INVALID_SOCK) {
        listener.stop();
        ap::net::global_shutdown();
        return 1;
    }

    unsigned char follow_up[44]{};
    follow_up[0] = 0x08;
    for (int i = 0; i < 8; ++i) follow_up[20 + i] = static_cast<unsigned char>(i + 1);
    // PTP epoch seconds 1, nanoseconds 500.
    follow_up[39] = 1;
    follow_up[42] = 0x01;
    follow_up[43] = 0xF4;

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(320);
    InetPtonA(AF_INET, "127.0.0.1", &target.sin_addr);
    ::sendto(sender, reinterpret_cast<const char*>(follow_up), sizeof(follow_up),
             0, reinterpret_cast<sockaddr*>(&target), sizeof(target));
    ap::net::close_socket(sender);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!listener.ready() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const bool ready = listener.ready();
    uint64_t mapped_local = 0;
    const bool mapped =
        listener.master_to_local(
            UINT64_C(1'000'000'500), mapped_local,
            UINT64_C(0x0102030405060708)) &&
        listener.clock_id() == UINT64_C(0x0102030405060708);
    const bool rejected_other_clock =
        !listener.master_to_local(
            UINT64_C(1'000'000'500), mapped_local,
            UINT64_C(0x9999999999999999));
    listener.stop();
    ap::net::global_shutdown();
    if (!ready || !mapped || !rejected_other_clock) {
        std::cerr << "FAIL: PTP Follow_Up was not processed\n";
        return 1;
    }
    std::cout << "PASS: PTP timing listener and master-to-local mapping\n";
    return 0;
}
