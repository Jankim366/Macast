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
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using winrt::Windows::Devices::Enumeration::DeviceInformation;
using winrt::Windows::Foundation::Metadata::ApiInformation;
using winrt::Windows::Media::Audio::AudioPlaybackConnection;
using winrt::Windows::Media::Audio::AudioPlaybackConnectionOpenResultStatus;
using winrt::Windows::Media::Audio::AudioPlaybackConnectionState;

constexpr wchar_t kMutexName[] =
    L"Local\\Macast.BluetoothAudioBackend.v1";
constexpr wchar_t kDefaultPipeName[] =
    L"\\\\.\\pipe\\Macast-BluetoothAudio-Control-v1";

std::atomic<bool> g_stop{false};
std::mutex g_state_mutex;
std::wstring g_pipe_name = kDefaultPipeName;
std::wstring g_state_path;
std::wstring g_device_id;
std::wstring g_device_name;
std::wstring g_error;
std::string g_state = "disconnected";
AudioPlaybackConnection g_connection{nullptr};
winrt::event_token g_state_token{};
uint64_t g_generation = 0;

std::string utf8(const winrt::hstring& value) {
    return winrt::to_string(value);
}

std::string utf8(const std::wstring& value) {
    return winrt::to_string(winrt::hstring(value));
}

std::wstring wide(const winrt::hstring& value) {
    return std::wstring(value.c_str(), value.size());
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

std::vector<std::pair<std::wstring, std::wstring>> list_devices() {
    std::vector<std::pair<std::wstring, std::wstring>> result;
    const auto selector = AudioPlaybackConnection::GetDeviceSelector();
    const auto devices = DeviceInformation::FindAllAsync(selector).get();
    for (const auto& device : devices) {
        result.emplace_back(
            std::wstring(device.Id()), std::wstring(device.Name()));
    }
    return result;
}

std::string devices_json() {
    std::ostringstream out;
    out << "{\"devices\":[";
    bool first = true;
    for (const auto& [id, name] : list_devices()) {
        if (!first) out << ',';
        first = false;
        out << "{\"id_hex\":\"" << encode_hstring(id)
            << "\",\"name_hex\":\"" << encode_hstring(name)
            << "\"}";
    }
    out << "]}";
    return out.str();
}

bool connect_device(
        const std::string& device_id_utf8,
        std::string& error) {
    try {
        close_connection();
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
        } catch (...) {
        }

        auto connection =
            AudioPlaybackConnection::TryCreateFromId(device_id);
        if (!connection) {
            set_state(
                "error", device_id_wide,
                device_name, L"AudioPlaybackConnection unavailable");
            error = "AudioPlaybackConnection unavailable";
            return false;
        }

        uint64_t generation = 0;
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
                            "connected", device_id_wide, device_name);
                    } else if (sender.State() ==
                                   AudioPlaybackConnectionState::Closed) {
                        set_state(
                            "disconnected", device_id_wide, device_name);
                    }
                } catch (...) {
                }
            });

        connection.StartAsync().get();
        const auto result = connection.OpenAsync().get();
        if (result.Status() !=
                AudioPlaybackConnectionOpenResultStatus::Success) {
            std::wstring status =
                result.Status() ==
                    AudioPlaybackConnectionOpenResultStatus::RequestTimedOut
                ? L"The Bluetooth request timed out"
                : result.Status() ==
                    AudioPlaybackConnectionOpenResultStatus::DeniedBySystem
                ? L"The Bluetooth request was denied by Windows"
                : L"Unknown Bluetooth connection failure";
            set_state("error", device_id_wide, device_name, status);
            error = utf8(status);
            close_connection();
            return false;
        }

        set_state("connected", device_id_wide, device_name);
        return true;
    } catch (const winrt::hresult_error& exception) {
        error = utf8(exception.message());
        set_state("error", {}, {}, wide(exception.message()));
        close_connection();
        return false;
    } catch (const std::exception& exception) {
        error = exception.what();
        set_state("error", {}, {}, wide(winrt::to_hstring(error)));
        close_connection();
        return false;
    }
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

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        return 3;
    }

    set_state("disconnected");
    std::thread(pipe_loop).join();

    close_connection();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    winrt::uninit_apartment();
    return 0;
}
