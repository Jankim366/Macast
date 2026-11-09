#include "audio/buffered_audio_receiver.h"

#include "audio/ap2_packet_crypto.h"
#include "log.h"

#include <algorithm>
#include <chrono>
#include <thread>

#if defined(_WIN32)
#include <ws2tcpip.h>
#else
#include <sys/select.h>
#endif

namespace ap::audio {
namespace {

constexpr uint32_t kAlac44100S16Stereo = 0x0000FACE;
constexpr uint32_t kAlac48000S24Stereo = 0x15000000;
constexpr uint32_t kAac44100Stereo = 0x16000000;
constexpr uint32_t kAac48000Stereo = 0x17000000;
constexpr int64_t kOutputLeadNs = 40'000'000LL;
constexpr int64_t kLateDropNs = 250'000'000LL;
constexpr int64_t kFallbackBufferNs = 250'000'000LL;

uint16_t read_u16(const unsigned char* data) {
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

uint32_t read_u32(const unsigned char* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

int64_t steady_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int32_t timestamp_delta(uint32_t timestamp, uint32_t anchor) {
    return static_cast<int32_t>(timestamp - anchor);
}

} // namespace

BufferedAudioReceiver::BufferedAudioReceiver() = default;

BufferedAudioReceiver::~BufferedAudioReceiver() {
    stop();
}

bool BufferedAudioReceiver::start(Config config) {
    if (config.listener == INVALID_SOCK ||
        config.stream_key.size() != 32 ||
        config.channels <= 0) {
        LOG_ERROR << "BufferedAudioReceiver: invalid SETUP parameters";
        return false;
    }

    config_ = std::move(config);
    running_.store(true);
    worker_ = std::thread(&BufferedAudioReceiver::receive_loop, this);
    LOG_INFO << "BufferedAudioReceiver listening; codec/rate selected from "
                "the first encrypted RTP packet";
    return true;
}

void BufferedAudioReceiver::set_volume_db(float db) {
    std::lock_guard<std::mutex> lock(audio_mutex_);
    if (output_) output_->set_volume_db(db);
}

void BufferedAudioReceiver::flush() {
    {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        pending_.clear();
        pending_samples_ = 0;
    }
    std::lock_guard<std::mutex> audio_lock(audio_mutex_);
    if (output_) output_->clear();
}

void BufferedAudioReceiver::set_ap2_rate_anchor(
    uint64_t timeline_id,
    uint32_t rtp_time,
    uint64_t network_time_ns,
    bool playing) {
    bool clear_audio = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        anchor_timeline_id_ = timeline_id;
        anchor_rtp_ = rtp_time;
        anchor_network_ns_ = network_time_ns;
        anchor_valid_ = playing;
        playing_ = playing;
        fallback_clock_valid_ = false;
        clear_audio = !playing;
        if (clear_audio) {
            pending_.clear();
            pending_samples_ = 0;
        }
    }

    std::lock_guard<std::mutex> lock(audio_mutex_);
    if (!output_) return;
    if (clear_audio) output_->clear();
    output_->set_paused(!playing);
    LOG_INFO << "BufferedAudioReceiver timing anchor: rtp=" << rtp_time
             << " timeline=0x" << std::hex << timeline_id << std::dec
             << " playing=" << (playing ? "yes" : "no");
}

bool BufferedAudioReceiver::read_exact(
    socket_t socket, void* data, std::size_t size) {
    auto* cursor = static_cast<unsigned char*>(data);
    std::size_t received = 0;
    while (received < size && running_.load()) {
        const int count = ::recv(
            socket, reinterpret_cast<char*>(cursor + received),
            static_cast<int>(size - received), 0);
        if (count <= 0) return false;
        received += static_cast<std::size_t>(count);
    }
    return received == size;
}

bool BufferedAudioReceiver::configure_decoder(uint32_t ssrc) {
    if (configured_ssrc_ == ssrc && decoder_ && output_) return true;

    int sample_rate = 0;
    int bit_depth = 16;
    int spf = 0;
    int ct = 0;
    switch (ssrc) {
        case kAlac44100S16Stereo:
            sample_rate = 44100;
            bit_depth = 16;
            spf = 352;
            ct = 2;
            break;
        case kAlac48000S24Stereo:
            sample_rate = 48000;
            bit_depth = 24;
            spf = 352;
            ct = 2;
            break;
        case kAac44100Stereo:
            sample_rate = 44100;
            spf = 1024;
            ct = 3;
            break;
        case kAac48000Stereo:
            sample_rate = 48000;
            spf = 1024;
            ct = 3;
            break;
        default:
            LOG_WARN << "BufferedAudioReceiver: unsupported SSRC 0x"
                     << std::hex << ssrc << std::dec;
            return false;
    }

    auto decoder = std::make_unique<AacDecoder>();
    AacDecoder::Config decoder_config;
    decoder_config.ct = ct;
    decoder_config.sample_rate = sample_rate;
    decoder_config.channels = config_.channels;
    decoder_config.spf = spf;
    decoder_config.bit_depth = bit_depth;
    if (!decoder->init(decoder_config)) {
        LOG_ERROR << "BufferedAudioReceiver: decoder init failed for SSRC 0x"
                  << std::hex << ssrc << std::dec;
        return false;
    }

    auto output = std::make_unique<SdlAudioOutput>();
    if (!output->start(sample_rate, config_.channels)) {
        LOG_ERROR << "BufferedAudioReceiver: audio output init failed";
        return false;
    }
    output->set_paused(output_paused());

    {
        std::lock_guard<std::mutex> lock(audio_mutex_);
        decoder_ = std::move(decoder);
        output_ = std::move(output);
    }
    configured_ssrc_ = ssrc;
    config_.sample_rate = sample_rate;
    config_.ct = ct;
    config_.spf = spf;
    LOG_INFO << "BufferedAudioReceiver codec ready: SSRC=0x"
             << std::hex << ssrc << std::dec << " "
             << (ct == 2 ? "ALAC" : "AAC-LC") << " "
             << sample_rate << "Hz/" << bit_depth << "bit/"
             << config_.channels << "ch";
    return true;
}

bool BufferedAudioReceiver::output_paused() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return !playing_;
}

bool BufferedAudioReceiver::due_local_ns(
    const PendingPcm& pending,
    uint64_t& due_ns) {
    uint64_t timeline_id = 0;
    uint32_t anchor_rtp = 0;
    uint64_t anchor_network_ns = 0;
    bool anchor_valid = false;
    bool playing = false;
    bool fallback_active = false;
    const ap::airplay::PtpListener* ptp = nullptr;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        timeline_id = anchor_timeline_id_;
        anchor_rtp = anchor_rtp_;
        anchor_network_ns = anchor_network_ns_;
        anchor_valid = anchor_valid_;
        playing = playing_;
        fallback_active = fallback_clock_valid_;
        ptp = config_.ptp;
    }
    if (!playing) return false;

    uint64_t anchor_local_ns = 0;
    if (!fallback_active && anchor_valid && ptp &&
        ptp->master_to_local(
            anchor_network_ns, anchor_local_ns, timeline_id)) {
        const int64_t frames =
            timestamp_delta(pending.rtp_timestamp, anchor_rtp);
        const int64_t offset_ns =
            frames * INT64_C(1'000'000'000) / config_.sample_rate;
        const int64_t target =
            static_cast<int64_t>(anchor_local_ns) + offset_ns;
        const int64_t now = steady_now_ns();
        // A valid-looking anchor can still be in a different epoch or several
        // minutes ahead. For single-device playback, keep bounded startup
        // latency instead of enqueueing the whole burst before first sound.
        if (target >= now - 1'000'000'000LL &&
            target <= now + 1'000'000'000LL) {
            due_ns = static_cast<uint64_t>(target);
            return true;
        }
    }

    // 没有 PTP 样本时仍保持可播放，但只按到达时间提供约 250 ms 的
    // 本地缓冲；时钟同步不可用会在启动时单独记录告警。
    // 无 PTP 时建立一次性本地锚点，仍按 RTP 时间戳递增播放，避免发送端
    // 一次突发多秒数据时把所有块压进 SDL 队列。
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!fallback_clock_valid_) {
        fallback_clock_valid_ = true;
        fallback_rtp_ = pending.rtp_timestamp;
        fallback_local_ns_ =
            static_cast<uint64_t>(pending.arrival_local_ns) +
            kFallbackBufferNs;
    }
    const int64_t frames =
        timestamp_delta(pending.rtp_timestamp, fallback_rtp_);
    const int64_t offset_ns =
        frames * INT64_C(1'000'000'000) / config_.sample_rate;
    const int64_t target =
        static_cast<int64_t>(fallback_local_ns_) + offset_ns;
    if (target < 0) return false;
    due_ns = static_cast<uint64_t>(target);
    return true;
}

void BufferedAudioReceiver::drain_pending() {
    while (running_.load()) {
        PendingPcm pending;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (pending_.empty()) return;
            pending = std::move(pending_.front());
            pending_.pop_front();
            pending_samples_ -= pending.samples.size();
            if (!playing_) continue;
        }

        uint64_t due_ns = 0;
        if (!due_local_ns(pending, due_ns)) {
            // 锚点尚未到达或缓冲不足：把块放回队首稍后再试。
            std::lock_guard<std::mutex> lock(state_mutex_);
            pending_samples_ += pending.samples.size();
            pending_.push_front(std::move(pending));
            return;
        }

        const int64_t now = steady_now_ns();
        const int64_t wake_at = static_cast<int64_t>(due_ns) - kOutputLeadNs;
        if (now < wake_at) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            pending_samples_ += pending.samples.size();
            pending_.push_front(std::move(pending));
            const int64_t sleep_ns = std::min<int64_t>(
                wake_at - now, 10'000'000LL);
            std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns));
            return;
        }
        if (static_cast<int64_t>(due_ns) < now - kLateDropNs) {
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(audio_mutex_);
            if (!output_) return;
            output_->push(
                pending.samples.data(),
                static_cast<int>(pending.samples.size()));
        }
    }
}

void BufferedAudioReceiver::receive_loop() {
    sockaddr_storage peer{};
#if defined(_WIN32)
    int peer_size = sizeof(peer);
#else
    socklen_t peer_size = sizeof(peer);
#endif
    client_ = ::accept(
        config_.listener, reinterpret_cast<sockaddr*>(&peer), &peer_size);
    if (client_ == INVALID_SOCK) {
        LOG_WARN << "BufferedAudioReceiver: sender never connected";
        return;
    }
    LOG_INFO << "BufferedAudioReceiver: sender connected";

    std::vector<int16_t> pcm(8192);
    bool warned_no_ptp = false;
    uint64_t packets = 0;
    uint64_t decoded_frames = 0;
    const std::size_t max_samples =
        std::max<std::size_t>(
            config_.audio_buffer_size / sizeof(int16_t),
            static_cast<std::size_t>(config_.sample_rate) *
                static_cast<std::size_t>(config_.channels) * 8);
    while (running_.load()) {
        drain_pending();

        // Apply TCP backpressure instead of dropping the oldest decoded
        // blocks. Senders intentionally pre-buffer ahead of playback; dropping
        // that pre-buffer creates audible gaps and stuttering even though the
        // connection remains healthy.
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (pending_samples_ >= max_samples) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
        }

        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(client_, &readable);
        timeval timeout{0, 5000};
        const int selected = ::select(
            static_cast<int>(client_) + 1,
            &readable, nullptr, nullptr, &timeout);
        if (!running_.load()) break;
        if (selected <= 0 || !FD_ISSET(client_, &readable)) continue;

        unsigned char size_bytes[2];
        if (!read_exact(client_, size_bytes, sizeof(size_bytes))) break;
        const uint16_t packet_size = read_u16(size_bytes);
        if (packet_size < 34 || packet_size > 64 * 1024) {
            LOG_WARN << "BufferedAudioReceiver: invalid frame size "
                     << packet_size;
            break;
        }
        std::vector<unsigned char> packet(packet_size - 2);
        if (!read_exact(client_, packet.data(), packet.size())) break;

        const uint32_t rtp_timestamp = read_u32(packet.data() + 4);
        const uint32_t ssrc = read_u32(packet.data() + 8);
        // SSRC_NONE is used for control/heartbeat blocks interleaved with the
        // encrypted audio stream and carries no codec payload.
        if (ssrc == 0) continue;
        if (!configure_decoder(ssrc)) continue;
        ++packets;

        if (!warned_no_ptp && config_.ptp && !config_.ptp->ready()) {
            LOG_WARN << "BufferedAudioReceiver: PTP clock not acquired; "
                        "using local fallback buffer";
            warned_no_ptp = true;
        }

        std::vector<unsigned char> payload;
        if (!decrypt_packet(packet, payload)) {
            LOG_WARN << "BufferedAudioReceiver: packet authentication failed";
            continue;
        }
        if (decoder_->decode(
                payload.data(), static_cast<int>(payload.size())) < 0) {
            continue;
        }
        ++decoded_frames;

        uint32_t next_timestamp = rtp_timestamp;
        while (true) {
            const int count = decoder_->pull_pcm_s16(
                pcm.data(), static_cast<int>(pcm.size()));
            if (count <= 0) break;
            PendingPcm pending;
            pending.rtp_timestamp = next_timestamp;
            pending.arrival_local_ns = steady_now_ns();
            pending.samples.assign(pcm.begin(), pcm.begin() + count);
            next_timestamp += static_cast<uint32_t>(
                count / std::max(1, config_.channels));
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                pending_samples_ += static_cast<std::size_t>(count);
                pending_.push_back(std::move(pending));
            }
        }

        if (packets == 1 || packets % 500 == 0) {
            std::size_t queued = 0;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                queued = pending_samples_;
            }
            LOG_INFO << "BufferedAudioReceiver: packets=" << packets
                     << " decoded=" << decoded_frames
                     << " queued_samples=" << queued
                     << " ptp=" << (config_.ptp && config_.ptp->ready()
                                        ? "yes" : "no");
        }
    }
}

bool BufferedAudioReceiver::decrypt_packet(
    std::vector<unsigned char>& packet,
    std::vector<unsigned char>& plaintext) {
    return decrypt_buffered_packet(config_.stream_key, packet, plaintext);
}

void BufferedAudioReceiver::stop() {
    if (!running_.exchange(false)) return;
    if (config_.listener != INVALID_SOCK) {
        ap::net::close_socket(config_.listener);
        config_.listener = INVALID_SOCK;
    }
    if (client_ != INVALID_SOCK) {
#if defined(_WIN32)
        ::shutdown(client_, SD_BOTH);
#else
        ::shutdown(client_, SHUT_RDWR);
#endif
        ap::net::close_socket(client_);
        client_ = INVALID_SOCK;
    }
    if (worker_.joinable()) worker_.join();
    {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        pending_.clear();
        pending_samples_ = 0;
    }
    std::lock_guard<std::mutex> audio_lock(audio_mutex_);
    if (output_) {
        output_->stop();
        output_.reset();
    }
    decoder_.reset();
}

} // namespace ap::audio
