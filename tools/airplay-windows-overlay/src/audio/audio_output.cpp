#include "audio/audio_output.h"
#include "log.h"

#include <SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace ap::audio {
namespace {

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool looks_like_remote_or_virtual(const char* device_name) {
    if (!device_name) return false;
    const std::string lower = lower_ascii(device_name);
    return lower.find("virtual") != std::string::npos ||
           lower.find("todesk") != std::string::npos ||
           lower.find("remotecontrol") != std::string::npos ||
           lower.find(u8"虚拟") != std::string::npos;
}

std::string choose_output_device() {
    if (const char* configured = std::getenv("MACAST_AIRPLAY_AUDIO_DEVICE");
        configured && *configured) {
        return configured;
    }

    char* default_name = nullptr;
    SDL_AudioSpec default_spec{};
    std::string selected;
    if (SDL_GetDefaultAudioInfo(&default_name, &default_spec, 0) == 0 &&
        default_name) {
        selected = default_name;
        LOG_INFO << "SDL default output: " << selected;
    }
    SDL_free(default_name);

    if (!looks_like_remote_or_virtual(selected.c_str())) return selected;

    const int count = SDL_GetNumAudioDevices(0);
    for (int i = 0; i < count; ++i) {
        const char* candidate = SDL_GetAudioDeviceName(i, 0);
        if (!candidate || looks_like_remote_or_virtual(candidate)) continue;
        selected = candidate;
        LOG_WARN << "Default audio output is remote/virtual; using \""
                 << selected << "\" instead";
        return selected;
    }
    LOG_WARN << "Default audio output is remote/virtual and no local "
                "playback endpoint was found";
    return {};
}

} // namespace

SdlAudioOutput::SdlAudioOutput()  = default;
SdlAudioOutput::~SdlAudioOutput() { stop(); }

bool SdlAudioOutput::start(int sample_rate, int channels) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        LOG_ERROR << "SDL_InitSubSystem(AUDIO) failed: " << SDL_GetError();
        return false;
    }

    SDL_AudioSpec want{};
    want.freq     = sample_rate;
    want.format   = AUDIO_S16SYS;     // interleaved int16, host byte order
    want.channels = static_cast<Uint8>(channels);
    want.samples  = 1024;             // buffer granularity (~23 ms @ 44.1 kHz)
    want.callback = nullptr;          // push mode via SDL_QueueAudio

    SDL_AudioSpec got{};
    const std::string selected_device = choose_output_device();
    const char* open_device =
        selected_device.empty() ? nullptr : selected_device.c_str();
    // PCM is always supplied at the decoder's frequency/channel count.
    // SDL performs any hardware conversion instead of changing this contract.
    device_ = SDL_OpenAudioDevice(open_device, 0, &want, &got, 0);
    if (device_ == 0) {
        LOG_ERROR << "SDL_OpenAudioDevice failed: " << SDL_GetError();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }

    sample_rate_ = got.freq;
    channels_    = got.channels;
    buffer_frames_ = got.samples;
    SDL_PauseAudioDevice(device_, 0);
    LOG_INFO << "SdlAudioOutput playing via "
             << (SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "unknown")
             << " to \"" << (open_device ? open_device : "SDL default")
             << "\" at " << sample_rate_ << " Hz x "
             << channels_ << "ch (format int16)";
    return true;
}

void SdlAudioOutput::stop() {
    if (device_) {
        SDL_PauseAudioDevice(device_, 1);
        SDL_ClearQueuedAudio(device_);
        SDL_CloseAudioDevice(device_);
        device_ = 0;
        // Deliberately NOT calling SDL_QuitSubSystem(SDL_INIT_AUDIO):
        // stop() runs on the RTSP server thread during session
        // teardown, and on Windows shutting down the SDL audio
        // subsystem cross-thread can knock out the video event pump
        // (spontaneous SDL_QUIT / window close). SDL_Quit() in the
        // renderer's shutdown path will clean up the subsystem when
        // the app actually exits.
    }
}

void SdlAudioOutput::push(const int16_t* samples, int count) {
    if (!device_ || !samples || count <= 0) return;

    const float gain = gain_.load(std::memory_order_relaxed);

    // Fast path: unity gain → send as-is.
    if (gain >= 0.9999f && gain <= 1.0001f) {
        const uint32_t bytes = static_cast<uint32_t>(count) * sizeof(int16_t);
        if (SDL_QueueAudio(device_, samples, bytes) != 0) {
            LOG_WARN << "SDL_QueueAudio failed: " << SDL_GetError();
        }
        return;
    }

    // Apply gain into a scratch buffer, saturate to int16 range.
    static thread_local std::vector<int16_t> scratch;
    scratch.resize(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        float s = static_cast<float>(samples[i]) * gain;
        if (s >  32767.f) s =  32767.f;
        if (s < -32768.f) s = -32768.f;
        scratch[i] = static_cast<int16_t>(s);
    }
    const uint32_t bytes = static_cast<uint32_t>(count) * sizeof(int16_t);
    if (SDL_QueueAudio(device_, scratch.data(), bytes) != 0) {
        LOG_WARN << "SDL_QueueAudio failed: " << SDL_GetError();
    }
}

void SdlAudioOutput::set_volume_db(float db) {
    // AirPlay: 0 dB = full, -144 dB = mute. Anything below -100 treat as 0.
    float lin = (db <= -100.f) ? 0.f
              : (db >=    0.f) ? 1.f
              : std::pow(10.f, db / 20.f);
    gain_.store(lin, std::memory_order_relaxed);
    LOG_INFO << "audio volume: " << db << " dB (gain=" << lin << ')';
}

uint32_t SdlAudioOutput::queued_bytes() const {
    return device_ ? SDL_GetQueuedAudioSize(device_) : 0u;
}

int64_t SdlAudioOutput::pending_ns() const {
    if (!device_ || sample_rate_ <= 0 || channels_ <= 0) return 0;
    const int64_t frames = queued_bytes() / (channels_ * sizeof(int16_t));
    // SDL does not expose the hardware presentation clock. Include one
    // negotiated device buffer as an estimate, rather than a fixed 25 ms.
    return (frames + buffer_frames_) * 1'000'000'000LL / sample_rate_;
}

void SdlAudioOutput::set_paused(bool paused) {
    if (device_) SDL_PauseAudioDevice(device_, paused ? 1 : 0);
}

void SdlAudioOutput::clear() {
    if (device_) SDL_ClearQueuedAudio(device_);
}

} // namespace ap::audio
