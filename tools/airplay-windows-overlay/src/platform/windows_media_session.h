#pragma once

#include "airplay/media_control.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace ap::platform {

// Owns the Windows System Media Transport Controls session.
//
// All WinRT calls happen on one dedicated STA worker thread. Metadata,
// playback and availability updates may be posted from any thread and are
// coalesced into the next worker wake-up.
class WindowsMediaSession {
public:
    using CommandHandler =
        std::function<void(ap::airplay::MediaCommand)>;

    WindowsMediaSession();
    ~WindowsMediaSession();

    WindowsMediaSession(const WindowsMediaSession&) = delete;
    WindowsMediaSession& operator=(const WindowsMediaSession&) = delete;

    bool initialize(void* hwnd, CommandHandler handler);
    void set_metadata(const std::string& title,
                      const std::string& artist,
                      const std::string& album);
    void set_playback(bool playing);
    void set_timeline(uint32_t elapsed_ms, uint32_t duration_ms);
    void set_controls_available(bool available);
    void clear();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ap::platform
