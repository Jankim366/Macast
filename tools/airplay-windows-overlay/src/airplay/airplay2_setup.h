#pragma once

// AirPlay 2 SETUP — binary-plist body. Ported from UxPlay's
// lib/raop_handlers.h `raop_handler_setup` (GPL-3.0). Two request flavours
// can fire on the SAME request (UxPlay treats them as independent branches):
//
//   "session setup"  — dict contains `ekey` (72-byte FairPlay-encrypted
//                      AES-128 session key) and `eiv` (16-byte IV). Triggers
//                      the fairplay_decrypt step and adds `eventPort` +
//                      `timingPort` to the response.
//
//   "stream setup"   — dict contains `streams: [ { type, … } ]`. For each
//                      entry we bind UDP sockets and append a corresponding
//                      response entry with dataPort / controlPort / type.
//
// Stream types observed in practice:
//   96  — realtime audio (UDP ALAC/AAC)
//  103 — buffered audio（TCP + ChaCha20-Poly1305）
//   110 — mirroring video (H.264)

#include "airplay/streams.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ap::airplay {

struct StreamRequest {
    int      type             = 0;   // 96 = audio, 110 = mirror video
    uint64_t stream_conn_id   = 0;   // type 110: seeds the AES-CTR video key
    int      remote_control_port = 0; // type 96: iOS's own control port
    int      ct               = 0;   // type 96: compression type
    int      spf              = 0;   // type 96: samples per frame
    uint64_t audio_format     = 0;
    uint32_t stream_id         = 0;
    uint32_t audio_buffer_size = 0;
    std::vector<unsigned char> stream_key; // type 103: shk
};

struct Airplay2SetupRequest {
    // Session-setup branch.
    bool                    has_keys = false;     // true iff ekey+eiv both present
    std::vector<unsigned char> ekey;              // 72 bytes (RSA-AES blob)
    std::vector<unsigned char> eiv;               // 16 bytes
    std::string             device_id;
    std::string             model;
    std::string             name;
    std::string             timing_protocol;      // typically "NTP"
    uint64_t                timing_rport = 0;     // iOS's own timing port
    bool                    is_remote_control_only = false;

    // Stream-setup branch.
    bool                       has_streams = false;
    std::vector<StreamRequest> streams;
};

bool parse_airplay2_setup(const unsigned char* body, std::size_t len,
                          Airplay2SetupRequest& out);

// AirPlay 2 buffered audio 的 SETRATEANCHORI 控制消息。
//
// `rtp_time` 与 `network_time_ns` 共同定义发送端时间轴锚点，播放端再通过
// PTP 时钟把该锚点换算成本机 steady_clock 时间。`playing` 对应 rate bit 0。
struct Airplay2RateAnchor {
    uint64_t timeline_id = 0;
    uint64_t network_time_ns = 0;
    uint32_t rtp_time = 0;
    bool playing = false;
};

bool parse_airplay2_rate_anchor(
    const unsigned char* body,
    std::size_t len,
    Airplay2RateAnchor& out);

// A binary pair-setup body is either modern HAP TLV or the legacy 32-byte
// ed25519 public-key response. Accept both legacy HAP M1 layouts:
// State/Method and Method/SeqNo.
bool looks_like_hap_tlv(const std::vector<unsigned char>& body);

// --- Response builder ---
//
// UxPlay builds ONE response dict by unconditionally letting both branches
// append their fields, so the builder mirrors that: start with an empty
// dict, add session fields if the request had keys, add streams array if
// the request had streams.

struct StreamAllocation {
    uint16_t data_port    = 0;
    uint16_t control_port = 0;
};

class Airplay2SetupResponse {
public:
    Airplay2SetupResponse();
    ~Airplay2SetupResponse();

    // Session 分支：返回 AP2 event 监听端口，并按模式返回 NTP timing 端口
    // 或 PTP peer 信息。
    void add_session(
        uint16_t event_lport,
        uint16_t timing_lport,
        const std::string& local_ip,
        const std::string& device_id,
        bool ptp);

    // Stream branch. `requests` and `allocated` must be the same size.
    void add_streams(const std::vector<StreamRequest>& requests,
                     const std::vector<StreamAllocation>& allocated);

    // Serialize to bplist00.
    std::vector<unsigned char> serialize();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ap::airplay
