#pragma once

#include "airplay/media_control.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace ap::platform {

// Windows 要求在创建首个顶层窗口前设置显式 AppUserModelID，否则 SMTC 可能
// 已启用却不出现在任务栏媒体控制中心。该函数可重复调用，内部只执行一次。
void configure_process_app_user_model_id();

// 管理 Windows System Media Transport Controls 会话。
//
// 所有 WinRT 调用固定在专用 STA 工作线程执行；元数据、播放状态和可用性
// 可由任意线程提交，并在下一次工作线程唤醒时合并发布。
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
