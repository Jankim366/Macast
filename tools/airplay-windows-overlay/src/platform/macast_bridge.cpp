#include "platform/macast_bridge.h"

#include "log.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace ap::platform {
namespace {

constexpr const char* kStateFileName = "macast_airplay_state.ini";

#if defined(_WIN32)
constexpr wchar_t kPipeName[] =
    L"\\\\.\\pipe\\Macast-AirPlay-Control-v1";
#endif

std::string hex_encode(const std::string& value) {
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setfill('0');
    for (const unsigned char c : value) {
        out << std::setw(2) << static_cast<unsigned>(c);
    }
    return out.str();
}

bool parse_command(const std::string& line,
                   ap::airplay::MediaCommand& command,
                   uint32_t& position_ms) {
    if (line == "play") {
        command = ap::airplay::MediaCommand::Play;
    } else if (line == "pause") {
        command = ap::airplay::MediaCommand::Pause;
    } else if (line == "playpause") {
        command = ap::airplay::MediaCommand::PlayPause;
    } else if (line == "previtem") {
        command = ap::airplay::MediaCommand::Previous;
    } else if (line == "nextitem") {
        command = ap::airplay::MediaCommand::Next;
    } else if (line == "volumeup") {
        command = ap::airplay::MediaCommand::VolumeUp;
    } else if (line == "volumedown") {
        command = ap::airplay::MediaCommand::VolumeDown;
    } else if (line == "mutetoggle") {
        command = ap::airplay::MediaCommand::MuteToggle;
    } else if (line.rfind("seek:", 0) == 0) {
        try {
            position_ms = static_cast<uint32_t>(
                std::stoul(line.substr(5)));
            command = ap::airplay::MediaCommand::Seek;
        } catch (...) {
            return false;
        }
    } else {
        return false;
    }
    return true;
}

} // namespace

MacastBridge& MacastBridge::instance() {
    static MacastBridge bridge;
    return bridge;
}

void MacastBridge::start(CommandHandler handler) {
    if (running_.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handler_ = std::move(handler);
        title_.clear();
        artist_.clear();
        album_.clear();
        elapsed_ms_ = 0;
        duration_ms_ = 0;
        rate_ = 0.0f;
        control_available_ = false;
        has_cover_ = false;
        publish_locked();
    }
    pipe_thread_ = std::thread(&MacastBridge::pipe_loop, this);
}

void MacastBridge::stop() {
    if (!running_.exchange(false)) return;
#if defined(_WIN32)
    // Wake a server blocked in ConnectNamedPipe so the worker can join.
    HANDLE wake = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (wake != INVALID_HANDLE_VALUE) CloseHandle(wake);
#endif
    if (pipe_thread_.joinable()) pipe_thread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    handler_ = {};
}

void MacastBridge::update_metadata(const std::string& title,
                                   const std::string& artist,
                                   const std::string& album,
                                   uint32_t duration_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    title_ = title;
    artist_ = artist;
    album_ = album;
    if (duration_ms) duration_ms_ = duration_ms;
    has_cover_ = false;
    publish_locked();
}

void MacastBridge::update_progress(uint32_t elapsed_ms,
                                   uint32_t duration_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    elapsed_ms_ = elapsed_ms;
    if (duration_ms) duration_ms_ = duration_ms;
    publish_locked();
}

void MacastBridge::update_rate(float rate) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (rate_ == rate) return;
    rate_ = rate;
    publish_locked();
}

void MacastBridge::update_control(bool available) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (control_available_ == available) return;
    control_available_ = available;
    publish_locked();
}

void MacastBridge::update_cover() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (has_cover_) return;
    has_cover_ = true;
    publish_locked();
}

void MacastBridge::clear_media() {
    std::lock_guard<std::mutex> lock(mutex_);
    title_.clear();
    artist_.clear();
    album_.clear();
    elapsed_ms_ = 0;
    duration_ms_ = 0;
    rate_ = 0.0f;
    control_available_ = false;
    has_cover_ = false;
    publish_locked();
}

void MacastBridge::publish_locked() {
    const std::string temporary = std::string(kStateFileName) + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            LOG_WARN << "Macast bridge: cannot write state file";
            return;
        }
        out << "version=1\n"
            << "title_hex=" << hex_encode(title_) << '\n'
            << "artist_hex=" << hex_encode(artist_) << '\n'
            << "album_hex=" << hex_encode(album_) << '\n'
            << "elapsed_ms=" << elapsed_ms_ << '\n'
            << "duration_ms=" << duration_ms_ << '\n'
            << "rate=" << rate_ << '\n'
            << "control=" << (control_available_ ? 1 : 0) << '\n'
            << "has_cover=" << (has_cover_ ? 1 : 0) << '\n'
            << "cover_path=" << (has_cover_ ? "cover.jpg" : "") << '\n';
    }
#if defined(_WIN32)
    if (!MoveFileExA(temporary.c_str(), kStateFileName,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        LOG_WARN << "Macast bridge: cannot replace state file";
    }
#else
    std::rename(temporary.c_str(), kStateFileName);
#endif
}

void MacastBridge::pipe_loop() {
#if defined(_WIN32)
    while (running_.load()) {
        HANDLE pipe = CreateNamedPipeW(
            kPipeName, PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, 4096, 4096, 0, nullptr);
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
        while (running_.load() && keep_reading) {
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

                    ap::airplay::MediaCommand command;
                    uint32_t position_ms = 0;
                    CommandHandler handler;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        handler = handler_;
                    }
                    const bool ok = handler &&
                        parse_command(line, command, position_ms) &&
                        handler(command, position_ms);
                    const char* reply = ok ? "OK\n" : "ERR\n";
                    DWORD written = 0;
                    WriteFile(pipe, reply, static_cast<DWORD>(strlen(reply)),
                              &written, nullptr);
                    line.clear();
                } else if (c != '\r' && line.size() < 256) {
                    line.push_back(c);
                }
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
#else
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
#endif
}

} // namespace ap::platform
