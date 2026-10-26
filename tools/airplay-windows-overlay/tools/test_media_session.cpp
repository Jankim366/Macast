#include "log.h"
#include "platform/windows_media_session.h"

#include <windows.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/base.h>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace {

LRESULT CALLBACK window_proc(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

HWND create_hidden_window() {
    constexpr wchar_t kClassName[] = L"MacastSmtcTestWindow";
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpfnWndProc = &window_proc;
    window_class.lpszClassName = kClassName;
    RegisterClassExW(&window_class);
    return CreateWindowExW(
        0, kClassName, L"Macast SMTC test",
        WS_OVERLAPPEDWINDOW, 0, 0, 640, 360,
        nullptr, nullptr, window_class.hInstance, nullptr);
}

} // namespace

int main() {
    ap::set_log_enabled(true);
    HWND window = create_hidden_window();
    if (!window) {
        std::cerr << "FAIL: could not create test window\n";
        return 1;
    }

    ap::platform::WindowsMediaSession media;
    if (!media.initialize(
            window,
            [](ap::airplay::MediaCommand) {})) {
        std::cerr << "FAIL: WindowsMediaSession initialization\n";
        DestroyWindow(window);
        return 1;
    }

    media.set_metadata("Macast SMTC Probe", "Macast", "AirPlay");
    media.set_playback(true);
    media.set_timeline(12000, 180000);
    media.set_controls_available(true);

    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        using winrt::Windows::Media::Control::
            GlobalSystemMediaTransportControlsSessionManager;
        auto manager = GlobalSystemMediaTransportControlsSessionManager::
            RequestAsync().get();

        bool found = false;
        for (int attempt = 0; attempt < 20 && !found; ++attempt) {
            const auto sessions = manager.GetSessions();
            if (attempt == 0 || attempt == 19) {
                std::cout << "SMTC sessions=" << sessions.Size() << '\n';
            }
            for (const auto& session : sessions) {
                std::cout << "  source="
                          << winrt::to_string(session.SourceAppUserModelId())
                          << '\n';
                if (winrt::to_string(session.SourceAppUserModelId()) !=
                    "Jankim366.Macast.AirPlay") {
                    continue;
                }
                const auto properties =
                    session.TryGetMediaPropertiesAsync().get();
                const auto controls = session.GetPlaybackInfo().Controls();
                if (attempt == 0 || attempt == 19) {
                    std::cout << "    title="
                              << winrt::to_string(properties.Title())
                              << " artist="
                              << winrt::to_string(properties.Artist())
                              << " play=" << controls.IsPlayEnabled()
                              << " pause=" << controls.IsPauseEnabled()
                              << " prev=" << controls.IsPreviousEnabled()
                              << " next=" << controls.IsNextEnabled()
                              << '\n';
                }
                if (winrt::to_string(properties.Title()) != "Macast SMTC Probe") {
                    continue;
                }
                if (!controls.IsPauseEnabled() ||
                    !controls.IsPreviousEnabled() ||
                    !controls.IsNextEnabled()) {
                    continue;
                }
                found = true;
                std::cout << "PASS: SMTC source="
                          << winrt::to_string(session.SourceAppUserModelId())
                          << " title=" << winrt::to_string(properties.Title())
                          << " artist=" << winrt::to_string(properties.Artist())
                          << " controls=pause,previous,next\n";
                break;
            }
            if (!found) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        }

        media.clear();
        if (!found) {
            std::cerr << "FAIL: SMTC session was not visible with expected "
                         "source, metadata and controls\n";
            DestroyWindow(window);
            return 1;
        }
    } catch (const winrt::hresult_error& error) {
        std::cerr << "FAIL: SMTC manager query: "
                  << winrt::to_string(error.message()) << '\n';
        DestroyWindow(window);
        return 1;
    }

    DestroyWindow(window);
    return 0;
}
