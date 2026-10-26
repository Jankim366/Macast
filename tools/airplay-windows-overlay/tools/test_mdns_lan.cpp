#include "airplay/routes.h"
#include "log.h"
#include "mdns/mdns_service.h"
#include "net/socket.h"

#include <windows.h>
#include <windns.h>

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>

namespace {

struct BrowseState {
    std::mutex mutex;
    std::condition_variable cv;
    std::wstring expected;
    bool found{false};
    DNS_SERVICE_BROWSE_REQUEST request{};
    DNS_SERVICE_CANCEL cancel{};
};

void WINAPI browse_callback(
    DWORD status, PVOID context, PDNS_RECORD records) {
    auto* state = static_cast<BrowseState*>(context);
    if (status != ERROR_SUCCESS || !records) return;
    for (PDNS_RECORD record = records; record; record = record->pNext) {
        if (record->wType != DNS_TYPE_PTR ||
            !record->Data.PTR.pNameHost) {
            continue;
        }
        const std::wstring name(
            reinterpret_cast<const wchar_t*>(
                record->Data.PTR.pNameHost));
        if (_wcsicmp(name.c_str(), state->expected.c_str()) != 0) {
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->found = true;
        }
        state->cv.notify_all();
        return;
    }
}

} // namespace

int main() {
    if (!ap::net::global_init()) return 1;
    ap::set_log_enabled(true);

    const auto lan = ap::net::primary_lan_interface();
    if (lan.ipv4.empty() || lan.index == 0) {
        std::cerr << "FAIL: no LAN interface\n";
        return 1;
    }

    ap::airplay::DeviceContext context;
    context.name = "Macast mDNS Test";
    context.deviceid = ap::net::primary_mac();
    context.features = "0x5A7FFFF7,0x1E";
    context.model = "AppleTV3,2";
    context.srcvers = "220.68";

    ap::mdns::MdnsService service;
    if (!service.start(context, 65010)) {
        std::cerr << "FAIL: mDNS registration failed\n";
        return 1;
    }

    BrowseState state;
    state.expected = L"Macast mDNS Test._airplay._tcp.local";
    state.request.Version = DNS_QUERY_REQUEST_VERSION1;
    state.request.InterfaceIndex = lan.index;
    state.request.QueryName = L"_airplay._tcp.local";
    state.request.pBrowseCallback = &browse_callback;
    state.request.pQueryContext = &state;
    if (DnsServiceBrowse(&state.request, &state.cancel) !=
        DNS_REQUEST_PENDING) {
        std::cerr << "FAIL: mDNS browse could not start\n";
        service.stop();
        return 1;
    }

    bool found = false;
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        found = state.cv.wait_for(
            lock, std::chrono::seconds(8),
            [&] { return state.found; });
    }
    DnsServiceBrowseCancel(&state.cancel);
    service.stop();
    ap::net::global_shutdown();

    if (!found) {
        std::cerr << "FAIL: LAN mDNS browse did not return the receiver\n";
        return 1;
    }
    std::cout << "PASS: LAN mDNS registration is discoverable on interface "
              << lan.index << '\n';
    return 0;
}
