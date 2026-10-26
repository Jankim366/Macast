#include <windows.h>
#include <windns.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

struct BrowseState {
    std::wstring query_name;
    DNS_SERVICE_BROWSE_REQUEST request{};
    DNS_SERVICE_CANCEL cancel{};
};

std::string wide_to_utf8(const wchar_t* value) {
    if (!value || !*value) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string out(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value, -1, out.data(), size, nullptr, nullptr);
    return out;
}

void WINAPI browse_callback(
    DWORD status, PVOID context, PDNS_RECORD records) {
    auto* state = static_cast<BrowseState*>(context);
    if (status == ERROR_CANCELLED) return;
    std::cout << "CALLBACK query=" << wide_to_utf8(state->query_name.c_str())
              << " status=" << status << '\n';
    if (status != ERROR_SUCCESS || !records) return;
    for (PDNS_RECORD record = records; record; record = record->pNext) {
        if (record->wType != DNS_TYPE_PTR ||
            !record->Data.PTR.pNameHost) {
            continue;
        }
        std::cout << "  PTR="
                  << wide_to_utf8(reinterpret_cast<const wchar_t*>(
                         record->Data.PTR.pNameHost))
                  << " ttl=" << record->dwTtl << '\n';
    }
}

bool start_browse(BrowseState& state, const wchar_t* query_name,
                  ULONG interface_index) {
    state.query_name = query_name;
    state.request.Version = DNS_QUERY_REQUEST_VERSION1;
    state.request.InterfaceIndex = interface_index;
    state.request.QueryName = state.query_name.c_str();
    state.request.pBrowseCallback = &browse_callback;
    state.request.pQueryContext = &state;
    const DWORD status =
        DnsServiceBrowse(&state.request, &state.cancel);
    std::cout << "START query=" << wide_to_utf8(query_name)
              << " interface=" << interface_index
              << " status=" << status << '\n';
    return status == DNS_REQUEST_PENDING;
}

} // namespace

int main(int argc, char** argv) {
    const ULONG interface_index =
        argc > 1 ? static_cast<ULONG>(std::strtoul(argv[1], nullptr, 10)) : 0;
    BrowseState dacp;
    BrowseState airplay;
    const bool dacp_started =
        start_browse(dacp, L"_dacp._tcp.local", interface_index);
    const bool airplay_started =
        start_browse(airplay, L"_airplay._tcp.local", interface_index);

    std::this_thread::sleep_for(std::chrono::seconds(15));
    if (dacp_started) DnsServiceBrowseCancel(&dacp.cancel);
    if (airplay_started) DnsServiceBrowseCancel(&airplay.cancel);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    return dacp_started && airplay_started ? 0 : 1;
}
