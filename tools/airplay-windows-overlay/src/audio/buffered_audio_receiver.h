#pragma once

#include "airplay/ptp_listener.h"
#include "audio/aac_decoder.h"
#include "audio/audio_output.h"
#include "net/socket.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace ap::audio {

// 通过 TCP 接收 AirPlay 2 buffered audio（stream type 103）。
//
// 每个 RTP 包前带 big-endian uint16 长度；音频使用 SETUP `shk` 密钥和
// ChaCha20-Poly1305，末尾 8 字节为 nonce，其前 16 字节为 tag。本地小队列
// 用于吸收网络抖动，只服务单设备同步；多房间 clock domain 不在本类范围。
class BufferedAudioReceiver {
public:
    struct Config {
        socket_t listener = INVALID_SOCK; // ownership transferred
        std::vector<unsigned char> stream_key;
        int ct = 8;
        int sample_rate = 44100;
        int channels = 2;
        int spf = 480;
        uint32_t audio_buffer_size = 0x800000;
        const ap::airplay::PtpListener* ptp = nullptr;
    };

    BufferedAudioReceiver();
    ~BufferedAudioReceiver();

    BufferedAudioReceiver(const BufferedAudioReceiver&) = delete;
    BufferedAudioReceiver& operator=(const BufferedAudioReceiver&) = delete;

    // 启动 TCP 监听和播放线程；成功接管 listener socket 所有权。
    bool start(Config config);
    // 停止网络与音频线程，并释放本次 SETUP 创建的 socket。
    void stop();
    // 清空 SDL 中尚未播放的 PCM，对应 FLUSHBUFFERED。
    void flush();
    void set_volume_db(float db);

    // 应用 SETRATEANCHORI：把发送端的 RTP 时间轴锚定到 PTP 网络时间。
    // `playing=false` 对应 rate bit 0，暂停并清空旧音频；恢复时会使用新的锚点。
    void set_ap2_rate_anchor(
        uint64_t timeline_id,
        uint32_t rtp_time,
        uint64_t network_time_ns,
        bool playing);

private:
    struct PendingPcm {
        uint32_t rtp_timestamp = 0;
        int64_t arrival_local_ns = 0;
        std::vector<int16_t> samples;
    };

    void receive_loop();
    bool read_exact(socket_t socket, void* data, std::size_t size);
    bool configure_decoder(uint32_t ssrc);
    void drain_pending();
    bool due_local_ns(
        const PendingPcm& pending,
        uint64_t& due_ns);
    bool output_paused() const;
    bool decrypt_packet(
        std::vector<unsigned char>& packet,
        std::vector<unsigned char>& plaintext);

    Config config_;
    socket_t client_{INVALID_SOCK};
    std::atomic<bool> running_{false};
    std::thread worker_;
    mutable std::mutex state_mutex_;
    mutable std::mutex audio_mutex_;
    std::deque<PendingPcm> pending_;
    std::size_t pending_samples_{0};
    std::unique_ptr<AacDecoder> decoder_;
    std::unique_ptr<SdlAudioOutput> output_;
    uint32_t configured_ssrc_{0};
    uint32_t anchor_rtp_{0};
    uint64_t anchor_timeline_id_{0};
    uint64_t anchor_network_ns_{0};
    bool anchor_valid_{false};
    bool playing_{true};
    bool fallback_clock_valid_{false};
    uint32_t fallback_rtp_{0};
    uint64_t fallback_local_ns_{0};
};

} // namespace ap::audio
