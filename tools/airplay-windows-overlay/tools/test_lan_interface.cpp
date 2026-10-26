#include "net/socket.h"

#include <iostream>

int main() {
    if (!ap::net::global_init()) return 1;
    const auto lan = ap::net::primary_lan_interface();
    std::cout << "ipv4=" << lan.ipv4
              << " ipv6=" << lan.ipv6
              << " mac=" << lan.mac
              << " index=" << lan.index << '\n';
    ap::net::global_shutdown();
    if (lan.ipv4.empty() || lan.mac.empty() || lan.index == 0) {
        std::cerr << "FAIL: no physical LAN interface selected\n";
        return 1;
    }
    if (lan.ipv4.rfind("198.18.", 0) == 0 ||
        lan.ipv4.rfind("10.147.", 0) == 0) {
        std::cerr << "FAIL: virtual proxy interface selected\n";
        return 1;
    }
    std::cout << "PASS: physical LAN interface selected\n";
    return 0;
}
