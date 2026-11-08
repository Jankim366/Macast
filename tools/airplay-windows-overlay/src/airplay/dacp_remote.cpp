#include "airplay/dacp_remote.h"
#include "net/socket.h"
#include "log.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <sstream>
#include <utility>
#if defined(_WIN32)
#include <windows.h>
#include <windns.h>
#endif

namespace ap::airplay {
namespace {
bool hex_id(const std::string& s) {
    return !s.empty() && s.size() <= 16 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}
bool remote_token(const std::string& s) {
    return !s.empty() && s.size() <= 64 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_';
    });
}
#if defined(_WIN32)
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

bool send_request(const std::string& ip, uint16_t port,
                  const std::string& token, const std::string& host_name,
                  const std::string& path, bool post) {
    addrinfo hints{}, *addresses = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    const std::string port_string = std::to_string(port);
    if (getaddrinfo(ip.c_str(), port_string.c_str(), &hints, &addresses) != 0) return false;
    socket_t fd = INVALID_SOCK;
    for (auto* address = addresses; address; address = address->ai_next) {
        fd = ::socket(address->ai_family, SOCK_STREAM, IPPROTO_TCP);
        if (fd == INVALID_SOCK) continue;
        u_long nonblocking = 1;
        if (ioctlsocket(fd, FIONBIO, &nonblocking) != 0) {
            ap::net::close_socket(fd); fd = INVALID_SOCK; continue;
        }
        int rc = ::connect(fd, address->ai_addr, static_cast<int>(address->ai_addrlen));
        if (rc != 0) {
            const int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK || error == WSAEINPROGRESS) {
                fd_set writable, errors; FD_ZERO(&writable); FD_ZERO(&errors);
                FD_SET(fd, &writable); FD_SET(fd, &errors);
                timeval timeout{0, 500000};
                rc = ::select(0, nullptr, &writable, &errors, &timeout);
                int socket_error = 0, size = sizeof(socket_error);
                rc = rc > 0 && FD_ISSET(fd, &writable) &&
                     getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socket_error), &size) == 0 &&
                     socket_error == 0 ? 0 : -1;
            }
        }
        if (rc == 0) {
            nonblocking = 0;
            if (ioctlsocket(fd, FIONBIO, &nonblocking) == 0) break;
        }
        ap::net::close_socket(fd); fd = INVALID_SOCK;
    }
    freeaddrinfo(addresses);
    if (fd == INVALID_SOCK) { LOG_WARN << "DACP sender connection failed"; return false; }
    DWORD timeout = 500;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    std::string address_only = ip;
    const auto scope = address_only.find('%');
    if (scope != std::string::npos) address_only.resize(scope);
    const std::string address_host =
        address_only.find(':') == std::string::npos
            ? address_only
            : "[" + address_only + "]";
    const std::string host = !host_name.empty()
        ? host_name
        : address_host + ":" + port_string;
    const std::string wire = std::string(post ? "POST" : "GET") +
        " /ctrl-int/1/" + path + " HTTP/1.1\r\nHost: " + host +
        "\r\nActive-Remote: " + token +
        "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    bool success = false;
    if (ap::net::send_all(fd, wire.data(), static_cast<int>(wire.size())) == static_cast<int>(wire.size())) {
        std::string line;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (line.find("\r\n") == std::string::npos && line.size() < 256) {
            const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) break;
            fd_set readable; FD_ZERO(&readable); FD_SET(fd, &readable);
            timeval remaining_timeout{0, static_cast<long>(remaining)};
            if (::select(0, &readable, nullptr, nullptr, &remaining_timeout) <= 0) break;
            char reply[256];
            const int size = ::recv(fd, reply, sizeof(reply), 0);
            if (size <= 0) break;
            line.append(reply, size);
        }
        if (line.find("\r\n") != std::string::npos) {
            std::istringstream status(line);
            std::string version; int code = 0;
            status >> version >> code;
            success = version.rfind("HTTP/", 0) == 0 && code >= 200 && code < 300;
            LOG_INFO << "DACP media response status=" << code;
        } else {
            LOG_WARN << "DACP media response absent/incomplete";
        }
    }
    ap::net::close_socket(fd);
    return success;
}
#endif
}

struct DacpRemote::ResolveState {
    std::mutex mutex;
    std::condition_variable cv;
    bool pending{true};
    bool resolving{false};
    uint16_t port{0};
    std::string host_name;
#if defined(_WIN32)
    std::wstring target_service;
    std::wstring resolve_service;
    DNS_SERVICE_RESOLVE_REQUEST resolve_request{};
    DNS_SERVICE_CANCEL resolve_cancel{};
    DNS_SERVICE_BROWSE_REQUEST browse_request{};
    DNS_SERVICE_CANCEL browse_cancel{};
    bool browse_started{false};

    static void WINAPI completed(
        DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance) {
        std::unique_ptr<std::shared_ptr<ResolveState>> owner(
            static_cast<std::shared_ptr<ResolveState>*>(context));
        auto state = *owner;
        const uint16_t port =
            status == ERROR_SUCCESS && instance ? instance->wPort : 0;
        std::string host_name;
        if (status == ERROR_SUCCESS && instance && instance->pszHostName) {
            host_name = wide_to_utf8(instance->pszHostName);
        }
        if (instance) DnsServiceFreeInstance(instance);

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->resolving = false;
            if (port != 0) {
                state->port = port;
                state->host_name = std::move(host_name);
                state->pending = false;
            }
        }
        if (port != 0) state->cv.notify_all();
        LOG_INFO << "DACP service resolve status=" << status
                 << " port=" << port;
    }

    static void start_resolve(
        const std::shared_ptr<ResolveState>& state,
        const std::wstring& full_name,
        DWORD interface_index) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->port != 0 || state->resolving) return;
            state->resolving = true;
            state->resolve_service = full_name;
            state->resolve_request = {};
            state->resolve_request.Version = DNS_QUERY_REQUEST_VERSION1;
            state->resolve_request.InterfaceIndex = interface_index;
            state->resolve_request.QueryName =
                const_cast<PWSTR>(state->resolve_service.c_str());
            state->resolve_request.pResolveCompletionCallback = &completed;
            state->resolve_request.pQueryContext =
                new std::shared_ptr<ResolveState>(state);
        }
        const DWORD status = DnsServiceResolve(
            &state->resolve_request, &state->resolve_cancel);
        if (status != DNS_REQUEST_PENDING) {
            delete static_cast<std::shared_ptr<ResolveState>*>(
                state->resolve_request.pQueryContext);
            state->resolve_request.pQueryContext = nullptr;
            std::lock_guard<std::mutex> lock(state->mutex);
            state->resolving = false;
            LOG_WARN << "DACP service resolve could not start: " << status;
        }
    }

    static void WINAPI browse_completed(
        DWORD status, PVOID context, PDNS_RECORD records) {
        auto* owner =
            static_cast<std::shared_ptr<ResolveState>*>(context);
        if (!owner) return;
        auto state = *owner;

        if (status == ERROR_CANCELLED) {
            delete owner;
            return;
        }
        if (status != ERROR_SUCCESS || !records) return;

        for (PDNS_RECORD record = records; record; record = record->pNext) {
            if (record->wType != DNS_TYPE_PTR ||
                !record->Data.PTR.pNameHost) {
                continue;
            }
            const std::wstring service_name(
                reinterpret_cast<const wchar_t*>(
                    record->Data.PTR.pNameHost));
            if (_wcsicmp(service_name.c_str(),
                         state->target_service.c_str()) != 0) {
                continue;
            }
            LOG_INFO << "DACP browser found "
                     << wide_to_utf8(service_name.c_str());
            start_resolve(
                state, service_name,
                state->browse_request.InterfaceIndex);
            break;
        }
    }
#endif

    static std::shared_ptr<ResolveState> start(
        const std::string& id, DWORD interface_index) {
        auto state = std::make_shared<ResolveState>();
#if defined(_WIN32)
        const std::string name =
            "iTunes_Ctrl_" + id + "._dacp._tcp.local";
        state->target_service.assign(name.begin(), name.end());
        state->browse_request.Version = DNS_QUERY_REQUEST_VERSION1;
        state->browse_request.InterfaceIndex = interface_index;
        state->browse_request.QueryName = L"_dacp._tcp.local";
        state->browse_request.pBrowseCallback = &browse_completed;
        state->browse_request.pQueryContext =
            new std::shared_ptr<ResolveState>(state);
        const DWORD status = DnsServiceBrowse(
            &state->browse_request, &state->browse_cancel);
        if (status == DNS_REQUEST_PENDING) {
            state->browse_started = true;
            LOG_INFO << "DACP browser started for " << id;
        } else {
            delete static_cast<std::shared_ptr<ResolveState>*>(
                state->browse_request.pQueryContext);
            state->browse_request.pQueryContext = nullptr;
            LOG_WARN << "DACP browser could not start: " << status
                     << " (using direct resolve)";
            start_resolve(
                state, state->target_service, interface_index);
        }
#else
        (void)id;
        state->pending = false;
#endif
        return state;
    }

    void cancel_query() {
#if defined(_WIN32)
        bool cancel_browse = false;
        bool cancel_resolve = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            cancel_browse = browse_started;
            browse_started = false;
            cancel_resolve = resolving;
        }
        if (cancel_browse) {
            const DWORD status = DnsServiceBrowseCancel(&browse_cancel);
            LOG_INFO << "DACP browser cancel status=" << status;
        }
        if (cancel_resolve) DnsServiceResolveCancel(&resolve_cancel);
#endif
    }
};

DacpRemote::DacpRemote(std::string peer_ip) : peer_ip_(std::move(peer_ip)) {
    interface_index_ = ap::net::primary_lan_interface().index;
    if (interface_index_ && peer_ip_.find(':') != std::string::npos &&
        peer_ip_.find('%') == std::string::npos &&
        peer_ip_.rfind("fe80:", 0) == 0) {
        peer_ip_ += "%" + std::to_string(interface_index_);
    }
}
DacpRemote::~DacpRemote() { if (resolve_) resolve_->cancel_query(); }
void DacpRemote::update(const std::string& id, const std::string& token, bool audio_only, const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    audio_only_ = audio_only;
    session_id_ = session_id;
    if (!token.empty()) active_remote_ = remote_token(token) ? token : std::string{};
    if (!id.empty() && id != dacp_id_) {
        if (resolve_) resolve_->cancel_query();
        resolve_.reset();
        dacp_id_ = hex_id(id) ? id : std::string{};
        if (!dacp_id_.empty()) {
            resolve_ = ResolveState::start(
                dacp_id_, interface_index_);
        }
    }
}
bool DacpRemote::available(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!audio_only_ || active_remote_.empty() || !resolve_ ||
        (!session_id.empty() && session_id != session_id_)) return false;
    std::lock_guard<std::mutex> resolve_lock(resolve_->mutex);
    return resolve_->port != 0;
}
bool DacpRemote::send(MediaCommand command, uint32_t position_ms, const std::string& session_id) {
    std::shared_ptr<ResolveState> state;
    std::string token;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!audio_only_ || (!session_id.empty() && session_id != session_id_)) return false;
        state = resolve_;
        token = active_remote_;
    }
    if (!state || token.empty()) {
        LOG_WARN << "Audio control: no usable DACP credentials";
        return false;
    }
    std::unique_lock<std::mutex> lock(state->mutex);
    state->cv.wait_for(lock, std::chrono::milliseconds(750), [&] { return !state->pending; });
    const uint16_t port = state->port;
    const std::string host_name = state->host_name;
    lock.unlock();
    if (!port) { LOG_WARN << "Audio control: DACP sender service unavailable"; return false; }
#if defined(_WIN32)
    std::string path;
    switch (command) {
        case MediaCommand::Play: path = "play"; break;
        case MediaCommand::Pause: path = "pause"; break;
        case MediaCommand::PlayPause: path = "playpause"; break;
        case MediaCommand::Previous: path = "previtem"; break;
        case MediaCommand::Next: path = "nextitem"; break;
        case MediaCommand::VolumeUp: path = "volumeup"; break;
        case MediaCommand::VolumeDown: path = "volumedown"; break;
        case MediaCommand::MuteToggle: path = "mutetoggle"; break;
        case MediaCommand::Seek: path = "setproperty?dacp.playingtime=" + std::to_string(position_ms); break;
        default: return false;
    }
    LOG_INFO << "Audio control: " << path;
    return send_request(peer_ip_, port, token, host_name, path,
                        command == MediaCommand::Seek);
#else
    (void)command; (void)position_ms;
    return false;
#endif
}
}
