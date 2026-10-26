#pragma once

#include "airplay/media_control.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace ap::platform {

class MacastBridge {
public:
    using CommandHandler =
        std::function<bool(ap::airplay::MediaCommand, uint32_t)>;

    static MacastBridge& instance();

    void start(CommandHandler handler);
    void stop();
    void update_metadata(const std::string& title,
                         const std::string& artist,
                         const std::string& album,
                         uint32_t duration_ms);
    void update_progress(uint32_t elapsed_ms, uint32_t duration_ms);
    void update_rate(float rate);
    void update_control(bool available);
    void update_cover();
    void clear_media();

private:
    MacastBridge() = default;
    void pipe_loop();
    void publish_locked();

    std::mutex mutex_;
    CommandHandler handler_;
    std::atomic<bool> running_{false};
    std::thread pipe_thread_;
    std::string title_;
    std::string artist_;
    std::string album_;
    uint32_t elapsed_ms_{0};
    uint32_t duration_ms_{0};
    float rate_{0.0f};
    bool control_available_{false};
    bool has_cover_{false};
};

} // namespace ap::platform
