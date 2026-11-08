#pragma once

#include "net/socket.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ap::airplay {

// 监听 UDP 319/320 的轻量 AirPlay 2 PTP timing 接收器。
//
// 单设备音频链路需要发送端的 PTP clock 建立 media-time domain。本类只在
// 物理 LAN 加入 PTP multicast，跟踪 Sync/Follow_Up 时间并保存 master 与
// local 的偏移。多设备 clock 选举和完整 IEEE 1588 BMCA 不在实现范围内。
class PtpListener {
public:
    PtpListener();
    ~PtpListener();

    PtpListener(const PtpListener&) = delete;
    PtpListener& operator=(const PtpListener&) = delete;

    // 在物理网卡上绑定 PTP event/general 端口，成功一个即可继续。
    bool start();
    // 停止接收线程并关闭 PTP socket。
    void stop();
    bool ready() const;

    // 将发送端的 PTP 时间换算为本机 steady_clock 时间。
    //
    // `expected_clock_id` 非零时会校验当前 PTP master，避免把旧会话的锚点
    // 应用到新设备。时间样本超过 2 秒未更新时返回 false，让播放器回退到
    // 本地缓冲策略，而不是继续使用已经漂移的时钟。
    bool master_to_local(
        uint64_t master_ns,
        uint64_t& local_ns,
        uint64_t expected_clock_id = 0) const;
    uint64_t clock_id() const;

private:
    struct ClockSample {
        int64_t master_minus_local_ns = 0;
        uint64_t local_ns = 0;
    };

    void run();
    void process_packet(const unsigned char* data, std::size_t size);

    std::vector<socket_t> sockets_;
    std::atomic<bool> running_{false};
    std::atomic<bool> ready_{false};
    std::thread thread_;
    mutable std::mutex sample_mutex_;
    std::unordered_map<uint64_t, ClockSample> clock_samples_;
    uint64_t latest_clock_id_{0};
};

} // namespace ap::airplay
