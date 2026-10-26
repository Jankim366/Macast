#include "platform/windows_media_session.h"

#include "log.h"

#if defined(_WIN32)
#include <windows.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <systemmediatransportcontrolsinterop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/base.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#endif

namespace ap::platform {

#if defined(_WIN32)
namespace {

constexpr wchar_t kAppUserModelId[] = L"Jankim366.Macast.AirPlay";
constexpr wchar_t kShortcutName[] = L"Macast AirPlay.lnk";
const PROPERTYKEY kPkeyAppUserModelId = {
    {0x9F4C2855, 0x9F79, 0x4B39,
     {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}},
    5
};

using winrt::Windows::Foundation::TimeSpan;
using winrt::Windows::Media::MediaPlaybackStatus;
using winrt::Windows::Media::MediaPlaybackType;
using winrt::Windows::Media::SystemMediaTransportControls;
using winrt::Windows::Media::SystemMediaTransportControlsButton;
using winrt::Windows::Media::SystemMediaTransportControlsButtonPressedEventArgs;
using winrt::Windows::Media::SystemMediaTransportControlsTimelineProperties;

TimeSpan to_time_span(uint32_t milliseconds) {
    return TimeSpan{std::chrono::milliseconds(milliseconds)};
}

bool create_start_menu_shortcut() {
    wchar_t executable[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, executable, MAX_PATH)) return false;

    PWSTR programs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs))) {
        return false;
    }
    std::filesystem::path shortcut(programs);
    CoTaskMemFree(programs);
    shortcut /= kShortcutName;

    winrt::com_ptr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(link.put())))) {
        return false;
    }
    link->SetPath(executable);
    link->SetIconLocation(executable, 0);
    link->SetDescription(L"Macast AirPlay Receiver");

    winrt::com_ptr<IPropertyStore> properties;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(properties.put())))) {
        PROPVARIANT value;
        PropVariantInit(&value);
        value.vt = VT_LPWSTR;
        if (SUCCEEDED(SHStrDupW(kAppUserModelId, &value.pwszVal))) {
            properties->SetValue(kPkeyAppUserModelId, value);
            properties->Commit();
        }
        PropVariantClear(&value);
    }

    winrt::com_ptr<IPersistFile> file;
    if (FAILED(link->QueryInterface(IID_PPV_ARGS(file.put())))) return false;
    return SUCCEEDED(file->Save(shortcut.c_str(), TRUE));
}

} // namespace

struct WindowsMediaSession::Impl {
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::thread worker;
    HWND hwnd{nullptr};
    CommandHandler handler;

    bool stop{false};
    bool initialize_finished{false};
    bool initialized{false};
    std::string initialize_error;
    uint64_t revision{0};
    uint64_t applied_revision{0};

    bool available{false};
    bool playing{false};
    std::string title;
    std::string artist;
    std::string album;
    uint32_t elapsed_ms{0};
    uint32_t duration_ms{0};

    void wake() {
        ++revision;
        cv.notify_all();
    }
};

WindowsMediaSession::WindowsMediaSession()
    : impl_(std::make_unique<Impl>()) {}

WindowsMediaSession::~WindowsMediaSession() {
    if (!impl_) return;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stop = true;
    }
    impl_->cv.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
}

bool WindowsMediaSession::initialize(void* hwnd, CommandHandler handler) {
    if (!impl_ || !hwnd || impl_->worker.joinable()) return false;

    SetCurrentProcessExplicitAppUserModelID(kAppUserModelId);

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->hwnd = static_cast<HWND>(hwnd);
        impl_->handler = std::move(handler);
    }
    impl_->worker = std::thread([this] {
        Impl& state = *impl_;
        SystemMediaTransportControls smtc{nullptr};
        winrt::event_token button_token{};
        bool apartment_initialized = false;

        try {
            winrt::init_apartment(winrt::apartment_type::single_threaded);
            apartment_initialized = true;
            if (!create_start_menu_shortcut()) {
                LOG_WARN << "Windows SMTC could not create the Start menu shortcut";
            }

            auto interop = winrt::get_activation_factory<
                SystemMediaTransportControls,
                ISystemMediaTransportControlsInterop>();
            winrt::check_hresult(interop->GetForWindow(
                state.hwnd,
                winrt::guid_of<SystemMediaTransportControls>(),
                winrt::put_abi(smtc)));

            smtc.IsEnabled(false);
            smtc.IsStopEnabled(false);
            button_token = smtc.ButtonPressed(
                [this](const SystemMediaTransportControls&,
                       const SystemMediaTransportControlsButtonPressedEventArgs& args) {
                    CommandHandler command_handler;
                    bool available = false;
                    {
                        std::lock_guard<std::mutex> lock(impl_->mutex);
                        command_handler = impl_->handler;
                        available = impl_->available;
                    }
                    if (!available || !command_handler) return;

                    switch (args.Button()) {
                        case SystemMediaTransportControlsButton::Play:
                            command_handler(ap::airplay::MediaCommand::Play);
                            break;
                        case SystemMediaTransportControlsButton::Pause:
                            command_handler(ap::airplay::MediaCommand::Pause);
                            break;
                        case SystemMediaTransportControlsButton::Previous:
                            command_handler(ap::airplay::MediaCommand::Previous);
                            break;
                        case SystemMediaTransportControlsButton::Next:
                            command_handler(ap::airplay::MediaCommand::Next);
                            break;
                        default:
                            break;
                    }
                });

            {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.initialized = true;
                state.initialize_finished = true;
            }
            state.cv.notify_all();
            LOG_INFO << "Windows SMTC initialized with AppUserModelID "
                     << winrt::to_string(kAppUserModelId);
        } catch (const winrt::hresult_error& error) {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.initialize_finished = true;
            state.initialized = false;
            state.initialize_error = winrt::to_string(error.message());
        } catch (const std::exception& error) {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.initialize_finished = true;
            state.initialized = false;
            state.initialize_error = error.what();
        }

        if (!state.initialized) {
            state.cv.notify_all();
            LOG_WARN << "Windows SMTC initialization failed: "
                     << state.initialize_error;
            return;
        }

        std::unique_lock<std::mutex> lock(state.mutex);
        while (!state.stop) {
            state.cv.wait(lock, [&] {
                return state.stop || state.revision != state.applied_revision;
            });
            if (state.stop) break;

            const uint64_t applying_revision = state.revision;
            const bool available = state.available;
            const bool playing = state.playing;
            const std::string title = state.title;
            const std::string artist = state.artist;
            const std::string album = state.album;
            const uint32_t elapsed_ms = state.elapsed_ms;
            const uint32_t duration_ms = state.duration_ms;
            lock.unlock();

            try {
                const bool enabled = !title.empty() || !artist.empty();
                smtc.IsEnabled(enabled);
                smtc.IsPlayEnabled(available && !playing);
                smtc.IsPauseEnabled(available && playing);
                smtc.IsPreviousEnabled(available);
                smtc.IsNextEnabled(available);
                smtc.PlaybackStatus(
                    !enabled ? MediaPlaybackStatus::Closed
                    : playing ? MediaPlaybackStatus::Playing
                              : MediaPlaybackStatus::Paused);

                auto updater = smtc.DisplayUpdater();
                if (!enabled) {
                    updater.ClearAll();
                } else {
                    updater.Type(MediaPlaybackType::Music);
                    auto properties = updater.MusicProperties();
                    properties.Title(winrt::to_hstring(title));
                    properties.Artist(winrt::to_hstring(artist));
                    properties.AlbumTitle(winrt::to_hstring(album));
                }
                updater.Update();

                if (enabled) {
                    SystemMediaTransportControlsTimelineProperties timeline;
                    timeline.StartTime(to_time_span(0));
                    timeline.MinSeekTime(to_time_span(0));
                    timeline.Position(to_time_span(elapsed_ms));
                    timeline.EndTime(to_time_span(duration_ms));
                    timeline.MaxSeekTime(to_time_span(duration_ms));
                    smtc.UpdateTimelineProperties(timeline);
                }
            } catch (const winrt::hresult_error& error) {
                LOG_WARN << "Windows SMTC update failed: "
                         << winrt::to_string(error.message());
            }

            lock.lock();
            state.applied_revision = applying_revision;
        }

        lock.unlock();
        try {
            if (button_token) smtc.ButtonPressed(button_token);
            auto updater = smtc.DisplayUpdater();
            updater.ClearAll();
            updater.Update();
            smtc.IsEnabled(false);
            smtc = nullptr;
        } catch (const winrt::hresult_error&) {
        }
        if (apartment_initialized) winrt::uninit_apartment();
        LOG_INFO << "Windows SMTC stopped";
    });

    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->cv.wait_for(lock, std::chrono::seconds(3), [&] {
        return impl_->initialize_finished;
    });
    if (!impl_->initialize_finished || !impl_->initialized) {
        LOG_WARN << "Windows SMTC unavailable"
                 << (impl_->initialize_error.empty()
                         ? std::string{}
                         : ": " + impl_->initialize_error);
        return false;
    }
    return true;
}

void WindowsMediaSession::set_metadata(const std::string& title,
                                       const std::string& artist,
                                       const std::string& album) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->title == title && impl_->artist == artist &&
        impl_->album == album) {
        return;
    }
    impl_->title = title;
    impl_->artist = artist;
    impl_->album = album;
    impl_->wake();
}

void WindowsMediaSession::set_playback(bool playing) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->playing == playing) return;
    impl_->playing = playing;
    impl_->wake();
}

void WindowsMediaSession::set_timeline(uint32_t elapsed_ms,
                                       uint32_t duration_ms) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->elapsed_ms == elapsed_ms &&
        impl_->duration_ms == duration_ms) {
        return;
    }
    impl_->elapsed_ms = elapsed_ms;
    impl_->duration_ms = duration_ms;
    impl_->wake();
}

void WindowsMediaSession::set_controls_available(bool available) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->available == available) return;
    impl_->available = available;
    impl_->wake();
}

void WindowsMediaSession::clear() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->title.clear();
    impl_->artist.clear();
    impl_->album.clear();
    impl_->elapsed_ms = 0;
    impl_->duration_ms = 0;
    impl_->playing = false;
    impl_->available = false;
    impl_->wake();
}
#else
struct WindowsMediaSession::Impl {};
WindowsMediaSession::WindowsMediaSession() = default;
WindowsMediaSession::~WindowsMediaSession() = default;
bool WindowsMediaSession::initialize(void*, CommandHandler) { return false; }
void WindowsMediaSession::set_metadata(
    const std::string&, const std::string&, const std::string&) {}
void WindowsMediaSession::set_playback(bool) {}
void WindowsMediaSession::set_timeline(uint32_t, uint32_t) {}
void WindowsMediaSession::set_controls_available(bool) {}
void WindowsMediaSession::clear() {}
#endif

} // namespace ap::platform
