// Headless Windows A2DP sink bridge used by Macast.
//
// The AudioPlaybackConnection lifecycle follows the approach used by
// summerwind6/AudioPlaybackConnector-Win11 (MIT, Copyright (c) 2020 Richard
// Yu). This implementation intentionally has no tray icon, XAML picker, or
// standalone management window.

#include <windows.h>

#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Media.Audio.h>
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using winrt::Windows::Devices::Enumeration::DeviceInformation;
using winrt::Windows::Devices::Enumeration::DeviceInformationUpdate;
using winrt::Windows::Devices::Enumeration::DeviceWatcher;
using winrt::Windows::Foundation::Metadata::ApiInformation;
using winrt::Windows::Media::Audio::AudioPlaybackConnection;
using winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus;
using winrt::Windows::Media::Audio::AudioPlaybackConnectionState;

struct DeviceRecord {
    std::wstring id;
    std::wstring name;
    bool connected{false};
};

constexpr wchar_t kDefaultPipeName[] =
    L"\\\\.\\pipe\\Macast-BluetoothAudio-Control-v1";

std::atomic<bool> g_stop{false};
std::atomic<bool> g_initial_activation_pending{true};
std::atomic<bool> g_auto_mode{false};
std::mutex g_state_mutex;
std::mutex g_log_mutex;
std::mutex g_connect_mutex;
std::mutex g_devices_mutex;
std::mutex g_armed_mutex;
std::wstring g_pipe_name = kDefaultPipeName;
std::wstring g_state_path;
std::wstring g_log_path;
std::wstring g_device_id;
std::wstring g_device_name;
std::wstring g_error;
std::string g_state = "disconnected";
AudioPlaybackConnection g_connection{nullptr};
winrt::event_token g_state_token{};
uint64_t g_generation = 0;
std::wstring g_auto_preferred_device;
std::thread g_auto_thread;
DeviceWatcher g_device_watcher{nullptr};
std::map<std::wstring, DeviceRecord> g_known_devices;
struct ArmedConnection {
    AudioPlaybackConnection connection{nullptr};
    winrt::event_token token{};
    std::wstring id;
    std::wstring name;
};
std::map<std::wstring, std::shared_ptr<ArmedConnection>>
    g_armed_connections;

std::string utf8(const winrt::hstring& value) {
    return winrt::to_string(value);
}

std::string utf8(const std::wstring& value) {
    return winrt::to_string(winrt::hstring(value));
}

std::wstring wide(const winrt::hstring& value) {
    return std::wstring(value.c_str(), value.size());
}

void log_line(const std::string& message) {
    if (g_log_path.empty()) return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::lock_guard lock(g_log_mutex);
    std::ofstream out(g_log_path, std::ios::binary | std::ios::app);
    if (!out) return;
    out << '[' << std::setfill('0')
        << std::setw(2) << now.wHour << ':'
        << std::setw(2) << now.wMinute << ':'
        << std::setw(2) << now.wSecond << '.'
        << std::setw(3) << now.wMilliseconds << "] "
        << message << '\n';
}

std::string hex_encode(const std::string& value) {
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setfill('0');
    for (const unsigned char byte : value) {
        out << std::setw(2) << static_cast<unsigned>(byte);
    }
    return out.str();
}

bool hex_decode(const std::string& value, std::string& decoded) {
    if (value.size() % 2 != 0) return false;
    decoded.clear();
    decoded.reserve(value.size() / 2);
    for (std::size_t i = 0; i < value.size(); i += 2) {
        unsigned byte = 0;
        std::istringstream input(value.substr(i, 2));
        input >> std::hex >> byte;
        if (!input || !input.eof()) return false;
        decoded.push_back(static_cast<char>(byte));
    }
    return true;
}

std::string encode_hstring(const std::wstring& value) {
    return hex_encode(utf8(winrt::hstring(value)));
}

void publish_state_locked() {
    if (g_state_path.empty()) return;
    const std::filesystem::path state_path(g_state_path);
    std::error_code error;
    if (!state_path.parent_path().empty()) {
        std::filesystem::create_directories(
            state_path.parent_path(), error);
    }
    const std::filesystem::path temporary =
        state_path.wstring() + L".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << "version=1\n"
            << "state=" << g_state << '\n'
            << "device_id_hex=" << encode_hstring(g_device_id) << '\n'
            << "device_name_hex=" << encode_hstring(g_device_name) << '\n'
            << "error_hex=" << encode_hstring(g_error) << '\n';
    }
    MoveFileExW(
        temporary.c_str(), state_path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

void set_state(
        const std::string& state,
        std::wstring device_id = {},
        std::wstring device_name = {},
        std::wstring error = {}) {
    std::lock_guard lock(g_state_mutex);
    g_state = state;
    g_device_id = std::move(device_id);
    g_device_name = std::move(device_name);
    g_error = std::move(error);
    publish_state_locked();
}

std::string status_json() {
    std::lock_guard lock(g_state_mutex);
    std::ostringstream out;
    out << "{\"state\":\"" << g_state
        << "\",\"device_id_hex\":\"" << encode_hstring(g_device_id)
        << "\",\"device_name_hex\":\"" << encode_hstring(g_device_name)
        << "\",\"error_hex\":\"" << encode_hstring(g_error)
        << "\"}";
    return out.str();
}

void close_connection() {
    AudioPlaybackConnection connection{nullptr};
    {
        std::lock_guard lock(g_state_mutex);
        connection = std::exchange(g_connection, nullptr);
        ++g_generation;
    }
    if (!connection) return;
    try {
        connection.StateChanged(g_state_token);
    } catch (...) {
    }
    try {
        connection.Close();
    } catch (...) {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
}

std::vector<DeviceRecord> list_devices() {
    std::vector<DeviceRecord> result;
    std::lock_guard lock(g_devices_mutex);
    for (const auto& entry : g_known_devices) {
        result.push_back(entry.second);
    }
    return result;
}

void start_device_watcher() {
    try {
        auto properties =
            winrt::single_threaded_vector<winrt::hstring>();
        properties.Append(L"System.Devices.Aep.IsConnected");
        properties.Append(L"System.Devices.Aep.IsPresent");
        g_device_watcher = DeviceInformation::CreateWatcher(
            AudioPlaybackConnection::GetDeviceSelector(),
            properties);
        g_device_watcher.Added(
            [](const DeviceWatcher&,
               const DeviceInformation& device) {
                if (g_stop.load()) return;
                bool connected = false;
                try {
                    const auto values = device.Properties();
                    if (values.HasKey(
                            L"System.Devices.Aep.IsConnected")) {
                        connected = winrt::unbox_value<bool>(
                            values.Lookup(
                                L"System.Devices.Aep.IsConnected"));
                    }
                } catch (...) {
                }
                std::lock_guard lock(g_devices_mutex);
                g_known_devices[std::wstring(device.Id())] = DeviceRecord{
                    std::wstring(device.Id()),
                    std::wstring(device.Name()),
                    connected,
                };
                log_line("Bluetooth audio device available: " +
                         utf8(device.Name()) +
                         (connected ? " (connected)" : " (paired)"));
            });
        g_device_watcher.Updated(
            [](const DeviceWatcher&,
               const DeviceInformationUpdate& update) {
                if (g_stop.load()) return;
                std::lock_guard lock(g_devices_mutex);
                const auto found = g_known_devices.find(
                    std::wstring(update.Id()));
                if (found != g_known_devices.end()) {
                    try {
                        const auto values = update.Properties();
                        if (values.HasKey(
                                L"System.Devices.Aep.IsConnected")) {
                            found->second.connected =
                                winrt::unbox_value<bool>(
                                    values.Lookup(
                                        L"System.Devices.Aep.IsConnected"));
                        }
                    } catch (...) {
                    }
                }
            });
        g_device_watcher.Removed(
            [](const DeviceWatcher&,
               const DeviceInformationUpdate& update) {
                std::lock_guard lock(g_devices_mutex);
                g_known_devices.erase(std::wstring(update.Id()));
            });
        g_device_watcher.Start();
    } catch (const winrt::hresult_error& exception) {
        log_line("Bluetooth device watcher failed: " +
                 utf8(exception.message()));
    }
}

void stop_device_watcher() {
    if (!g_device_watcher) return;
    try {
        g_device_watcher.Stop();
    } catch (...) {
    }
    g_device_watcher = nullptr;
}

std::string devices_json() {
    std::ostringstream out;
    out << "{\"devices\":[";
    bool first = true;
    for (const auto& device : list_devices()) {
        if (!first) out << ',';
        first = false;
        out << "{\"id_hex\":\"" << encode_hstring(device.id)
            << "\",\"name_hex\":\"" << encode_hstring(device.name)
            << "\",\"connected\":"
            << (device.connected ? "true" : "false") << '}';
    }
    out << "]}";
    return out.str();
}

template <typename Operation>
bool wait_for_operation(
        Operation& operation, std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (operation.Status() ==
               winrt::Windows::Foundation::AsyncStatus::Started &&
           std::chrono::steady_clock::now() < deadline &&
           !g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (operation.Status() ==
            winrt::Windows::Foundation::AsyncStatus::Started) {
        operation.Cancel();
        return false;
    }
    return true;
}

void disarm_armed_connections() {
    std::vector<std::shared_ptr<ArmedConnection>> connections;
    {
        std::lock_guard lock(g_armed_mutex);
        for (const auto& entry : g_armed_connections) {
            connections.push_back(entry.second);
        }
        g_armed_connections.clear();
    }
    for (const auto& item : connections) {
        try {
            item->connection.StateChanged(item->token);
            item->connection.Close();
        } catch (...) {
        }
    }
    set_state("disconnected");
}

bool arm_device(const std::wstring& device_id,
                const std::wstring& device_name) {
    {
        std::lock_guard lock(g_armed_mutex);
        if (g_armed_connections.count(device_id)) return true;
    }

    auto connection =
        AudioPlaybackConnection::TryCreateFromId(
            winrt::hstring(device_id));
    if (!connection) return false;

    auto armed = std::make_shared<ArmedConnection>();
    armed->connection = connection;
    armed->id = device_id;
    armed->name = device_name;
    armed->token = connection.StateChanged(
        [device_id, device_name](
                const AudioPlaybackConnection& sender,
                const winrt::Windows::Foundation::IInspectable&) {
            try {
                if (sender.State() ==
                        AudioPlaybackConnectionState::Opened) {
                    std::vector<AudioPlaybackConnection> close_others;
                    {
                        std::lock_guard lock(g_armed_mutex);
                        for (const auto& entry : g_armed_connections) {
                            if (entry.first == device_id) continue;
                            try {
                                if (entry.second->connection.State() ==
                                        AudioPlaybackConnectionState::Opened) {
                                    close_others.push_back(
                                        entry.second->connection);
                                }
                            } catch (...) {
                            }
                        }
                    }
                    for (const auto& other : close_others) {
                        try {
                            other.Close();
                        } catch (...) {
                        }
                    }
                    set_state("connected", device_id, device_name);
                    log_line("Bluetooth source opened: " +
                             utf8(winrt::hstring(device_name)));
                } else if (sender.State() ==
                               AudioPlaybackConnectionState::Closed) {
                    bool active = false;
                    {
                        std::lock_guard lock(g_state_mutex);
                        active = g_device_id == device_id;
                    }
                    if (active) {
                        set_state("disconnected", device_id, device_name);
                        log_line("Bluetooth source closed: " +
                                 utf8(winrt::hstring(device_name)));
                    }
                }
            } catch (...) {
            }
        });

    {
        std::lock_guard lock(g_armed_mutex);
        g_armed_connections[device_id] = armed;
    }

    try {
        auto start_operation = connection.StartAsync();
        if (!wait_for_operation(
                start_operation, std::chrono::seconds(10))) {
            throw winrt::hresult_error(
                HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        }
        start_operation.GetResults();
        log_line("Bluetooth source armed: " +
                 utf8(winrt::hstring(device_name)));
        return true;
    } catch (const winrt::hresult_error& exception) {
        log_line("Bluetooth source arm failed: " +
                 utf8(exception.message()));
        std::lock_guard lock(g_armed_mutex);
        g_armed_connections.erase(device_id);
        return false;
    }
}

bool connect_device(
        const std::string& device_id_utf8,
        std::string& error) {
    std::lock_guard connect_lock(g_connect_mutex);
    const winrt::hstring device_id =
        winrt::to_hstring(device_id_utf8);
    const std::wstring device_id_wide = wide(device_id);
    if (device_id.empty()) {
        error = "empty device id";
        return false;
    }

    std::wstring device_name;
    try {
        const auto info =
            DeviceInformation::CreateFromIdAsync(device_id).get();
        device_name = wide(info.Name());
    } catch (const winrt::hresult_error& exception) {
        error = utf8(exception.message());
    } catch (const std::exception& exception) {
        error = exception.what();
    }

    close_connection();
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt > 0) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        log_line("Bluetooth activation attempt " +
                 std::to_string(attempt + 1));
        try {
            auto connection =
                AudioPlaybackConnection::TryCreateFromId(device_id);
            if (!connection) {
                error = "AudioPlaybackConnection unavailable";
                continue;
            }

            uint64_t generation = 0;
            bool warmup = g_initial_activation_pending.load();
            {
                std::lock_guard lock(g_state_mutex);
                g_connection = connection;
                generation = ++g_generation;
                g_state = "connecting";
                g_device_id = device_id_wide;
                g_device_name = device_name;
                g_error.clear();
                publish_state_locked();
            }

            if (!warmup) {
                g_state_token = connection.StateChanged(
                    [generation, device_id_wide, device_name](
                            const AudioPlaybackConnection& sender,
                            const winrt::Windows::Foundation::IInspectable&) {
                        try {
                            {
                                std::lock_guard lock(g_state_mutex);
                                if (generation != g_generation) return;
                            }
                            if (sender.State() ==
                                    AudioPlaybackConnectionState::Opened) {
                                set_state(
                                    "connected",
                                    device_id_wide, device_name);
                            } else if (sender.State() ==
                                           AudioPlaybackConnectionState::Closed) {
                                set_state(
                                    "disconnected",
                                    device_id_wide, device_name);
                            }
                        } catch (...) {
                        }
                    });
            }

            auto start_operation = connection.StartAsync();
            if (!wait_for_operation(
                    start_operation, std::chrono::seconds(10))) {
                error = "The Bluetooth start request timed out";
                close_connection();
                continue;
            }
            start_operation.GetResults();
            log_line("Bluetooth StartAsync completed");

            auto open_operation = connection.OpenAsync();
            if (!wait_for_operation(
                    open_operation, std::chrono::seconds(20))) {
                error = "The Bluetooth open request timed out";
                close_connection();
                continue;
            }
            const auto result = open_operation.GetResults();
            log_line("Bluetooth OpenAsync completed");
            if (result.Status() !=
                    AudioPlaybackConnectionOpenResultStatus::Success) {
                error = result.Status() ==
                        AudioPlaybackConnectionOpenResultStatus::
                            RequestTimedOut
                    ? "The Bluetooth request timed out"
                    : result.Status() ==
                        AudioPlaybackConnectionOpenResultStatus::
                            DeniedBySystem
                    ? "The Bluetooth request was denied by Windows"
                    : "Unknown Bluetooth connection failure";
                close_connection();
                continue;
            }

            if (warmup) {
                bool expected = true;
                if (g_initial_activation_pending.compare_exchange_strong(
                        expected, false)) {
                    log_line(
                        "First A2DP route warm-up succeeded; reconnecting "
                        "to activate the output endpoint");
                    close_connection();
                    continue;
                }
            }

            set_state("connected", device_id_wide, device_name);
            log_line("Bluetooth audio connected");
            return true;
        } catch (const winrt::hresult_error& exception) {
            error = utf8(exception.message());
            close_connection();
        } catch (const std::exception& exception) {
            error = exception.what();
            close_connection();
        }
    }

    set_state(
        "error", device_id_wide, device_name,
        wide(winrt::to_hstring(error.empty()
            ? "Unable to activate Bluetooth audio route" : error)));
    log_line("Bluetooth audio connection failed: " + error);
    return false;
}

void auto_connection_loop() {
    while (g_auto_mode.load() && !g_stop.load()) {
        std::vector<DeviceRecord> devices;
        try {
            devices = list_devices();
        } catch (const winrt::hresult_error& exception) {
            log_line("Bluetooth auto discovery failed: " +
                     utf8(exception.message()));
        } catch (...) {
            log_line("Bluetooth auto discovery failed");
        }

        if (!devices.empty()) {
            if (!g_auto_preferred_device.empty()) {
                const auto preferred = std::find_if(
                    devices.begin(), devices.end(),
                    [](const DeviceRecord& device) {
                        return device.id == g_auto_preferred_device;
                    });
                if (preferred != devices.end()) {
                    std::rotate(
                        devices.begin(), preferred, preferred + 1);
                }
            }
            const std::size_t count = devices.size();
            for (std::size_t index = 0;
                 index < count && g_auto_mode.load() && !g_stop.load();
                 ++index) {
                const auto& device = devices[index];
                if (!device.connected) continue;
                arm_device(device.id, device.name);
            }
        }

        for (int i = 0; i < 20 && g_auto_mode.load() && !g_stop.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void start_auto_mode() {
    if (g_auto_mode.exchange(true)) return;
    if (g_auto_thread.joinable()) g_auto_thread.join();
    g_auto_thread = std::thread(auto_connection_loop);
    log_line("Bluetooth auto-connect enabled");
}

void stop_auto_mode() {
    if (!g_auto_mode.exchange(false)) return;
    if (g_auto_thread.joinable()) g_auto_thread.join();
    disarm_armed_connections();
    log_line("Bluetooth auto-connect disabled");
}

bool write_all(HANDLE pipe, const std::string& data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        DWORD written = 0;
        if (!WriteFile(
                pipe, data.data() + offset,
                static_cast<DWORD>(data.size() - offset),
                &written, nullptr)) {
            return false;
        }
        offset += written;
    }
    return true;
}

std::string handle_command(const std::string& command) {
    if (command == "PING") return "OK\n";
    if (command == "STATUS") return "OK " + status_json() + "\n";
    if (command == "LIST") {
        try {
            return "OK " + devices_json() + "\n";
        } catch (const winrt::hresult_error& exception) {
            return "ERR " + utf8(exception.message()) + "\n";
        }
    }
    if (command == "DISCONNECT") {
        stop_auto_mode();
        close_connection();
        set_state("disconnected");
        return "OK\n";
    }
    if (command == "AUTO ON") {
        start_auto_mode();
        return "OK\n";
    }
    if (command.rfind("AUTO PREF ", 0) == 0) {
        std::string preferred;
        if (!hex_decode(command.substr(10), preferred)) {
            return "ERR invalid preferred device\n";
        }
        g_auto_preferred_device =
            wide(winrt::to_hstring(preferred));
        start_auto_mode();
        return "OK\n";
    }
    if (command == "AUTO OFF") {
        stop_auto_mode();
        close_connection();
        set_state("disconnected");
        return "OK\n";
    }
    if (command.rfind("CONNECT ", 0) == 0) {
        std::string device_id;
        if (!hex_decode(command.substr(8), device_id)) {
            return "ERR invalid device id\n";
        }
        std::string error;
        if (connect_device(device_id, error)) {
            return "OK " + status_json() + "\n";
        }
        return "ERR " + error + "\n";
    }
    if (command == "SHUTDOWN") {
        stop_auto_mode();
        g_stop.store(true);
        return "OK\n";
    }
    return "ERR unknown command\n";
}

void pipe_loop() {
    while (!g_stop.load()) {
        HANDLE pipe = CreateNamedPipeW(
            g_pipe_name.c_str(), PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, 65536, 65536, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }
        const BOOL connected = ConnectNamedPipe(pipe, nullptr) ||
            GetLastError() == ERROR_PIPE_CONNECTED;
        if (!connected) {
            CloseHandle(pipe);
            continue;
        }

        std::string line;
        char chunk[512];
        bool keep_reading = true;
        while (!g_stop.load() && keep_reading) {
            DWORD read = 0;
            if (!ReadFile(pipe, chunk, sizeof(chunk), &read, nullptr) ||
                read == 0) {
                break;
            }
            for (DWORD i = 0; i < read; ++i) {
                const char c = chunk[i];
                if (c == '\n') {
                    while (!line.empty() &&
                           (line.back() == '\r' || line.back() == ' ')) {
                        line.pop_back();
                    }
                    const std::string response = handle_command(line);
                    if (!write_all(pipe, response)) {
                        keep_reading = false;
                    }
                    line.clear();
                } else if (c != '\r' && line.size() < 16384) {
                    line.push_back(c);
                }
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

void parse_arguments() {
    int argc = 0;
    LPWSTR* arguments =
        CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!arguments) return;
    for (int i = 1; i < argc; ++i) {
        const std::wstring argument(arguments[i]);
        if (argument == L"--pipe" && i + 1 < argc) {
            g_pipe_name = arguments[++i];
        } else if (argument == L"--state" && i + 1 < argc) {
            g_state_path = arguments[++i];
        } else if (argument == L"--log" && i + 1 < argc) {
            g_log_path = arguments[++i];
        }
    }
    LocalFree(arguments);
}

} // namespace

int APIENTRY wWinMain(
        HINSTANCE, HINSTANCE, LPWSTR, int) {
    parse_arguments();
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    if (!ApiInformation::IsTypePresent(
            winrt::name_of<AudioPlaybackConnection>())) {
        set_state(
            "error", {}, {},
            L"Windows AudioPlaybackConnection is unavailable");
        return 2;
    }

    const std::wstring mutex_name =
        L"Local\\Macast.BluetoothAudioBackend." +
        std::to_wstring(std::hash<std::wstring>{}(g_pipe_name));
    HANDLE mutex = CreateMutexW(
        nullptr, TRUE, mutex_name.c_str());
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        return 3;
    }

    log_line("Bluetooth audio backend started");
    start_device_watcher();
    set_state("disconnected");
    std::thread(pipe_loop).join();

    stop_auto_mode();
    stop_device_watcher();
    close_connection();
    log_line("Bluetooth audio backend stopped");
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    winrt::uninit_apartment();
    return 0;
}
