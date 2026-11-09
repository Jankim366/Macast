#include "airplay/airplay2_setup.h"
#include "log.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <plist/plist.h>

namespace ap::airplay {
namespace {

// --- Small libplist helpers (same pattern as UxPlay) ------------------------

std::string get_str(plist_t parent, const char* key) {
    plist_t n = plist_dict_get_item(parent, key);
    if (!n || plist_get_node_type(n) != PLIST_STRING) return {};
    char* raw = nullptr;
    plist_get_string_val(n, &raw);
    std::string s = raw ? raw : "";
    std::free(raw);
    return s;
}

uint64_t get_uint(plist_t parent, const char* key, uint64_t fallback = 0) {
    plist_t n = plist_dict_get_item(parent, key);
    if (!n || plist_get_node_type(n) != PLIST_UINT) return fallback;
    uint64_t v = 0;
    plist_get_uint_val(n, &v);
    return v;
}

bool get_bool(plist_t parent, const char* key, bool fallback = false) {
    plist_t n = plist_dict_get_item(parent, key);
    if (!n) return fallback;
    uint8_t v = 0;
    plist_get_bool_val(n, &v);
    return v != 0;
}

std::vector<unsigned char> get_data(plist_t parent, const char* key) {
    plist_t n = plist_dict_get_item(parent, key);
    if (!n || plist_get_node_type(n) != PLIST_DATA) return {};
    char*    raw = nullptr;
    uint64_t len = 0;
    plist_get_data_val(n, &raw, &len);
    std::vector<unsigned char> out(
        reinterpret_cast<unsigned char*>(raw),
        reinterpret_cast<unsigned char*>(raw) + len);
    std::free(raw);
    return out;
}

void set_uint(plist_t dict, const char* key, uint64_t v) {
    plist_dict_set_item(dict, key, plist_new_uint(v));
}

} // namespace

// --- Parser -----------------------------------------------------------------

bool parse_airplay2_setup(const unsigned char* body, std::size_t len,
                          Airplay2SetupRequest& out) {
    out = {};
    if (!body || len < 8) {
        LOG_WARN << "airplay2 setup: body too short (" << len << " bytes)";
        return false;
    }

    plist_t root = nullptr;
    plist_from_bin(reinterpret_cast<const char*>(body),
                   static_cast<uint32_t>(len), &root);
    if (!root) {
        LOG_WARN << "airplay2 setup: body is not a valid bplist";
        return false;
    }
    if (plist_get_node_type(root) != PLIST_DICT) {
        LOG_WARN << "airplay2 setup: top-level is not a dict";
        plist_free(root);
        return false;
    }

    // --- Session-setup branch: detected by ekey + eiv presence ------------
    out.ekey = get_data(root, "ekey");
    out.eiv  = get_data(root, "eiv");
    out.device_id = get_str(root, "deviceID");
    out.model = get_str(root, "model");
    out.name = get_str(root, "name");
    out.timing_protocol = get_str(root, "timingProtocol");
    out.timing_rport = get_uint(root, "timingPort");
    out.is_remote_control_only = get_bool(root, "isRemoteControlOnly");
    if (!out.ekey.empty() && !out.eiv.empty()) {
        out.has_keys               = true;

        LOG_INFO << "airplay2 SETUP (session) deviceID=" << out.device_id
                 << " model=" << out.model
                 << " name=" << out.name
                 << " timingProtocol=" << out.timing_protocol
                 << " timingPort(remote)=" << out.timing_rport
                 << " ekey=" << out.ekey.size() << "B"
                 << " eiv=" << out.eiv.size() << "B";
    }

    // --- Stream-setup branch: detected by streams[] presence ---------------
    plist_t streams = plist_dict_get_item(root, "streams");
    if (streams && plist_get_node_type(streams) == PLIST_ARRAY) {
        out.has_streams = true;
        uint32_t n = plist_array_get_size(streams);
        out.streams.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            plist_t s = plist_array_get_item(streams, i);
            if (!s || plist_get_node_type(s) != PLIST_DICT) continue;
            StreamRequest sr;
            sr.type              = static_cast<int>(get_uint(s, "type"));
            sr.stream_conn_id    = get_uint(s, "streamConnectionID");
            sr.remote_control_port = static_cast<int>(get_uint(s, "controlPort"));
            sr.ct                = static_cast<int>(get_uint(s, "ct"));
            sr.spf               = static_cast<int>(get_uint(s, "spf"));
            sr.audio_format      = get_uint(s, "audioFormat");
            sr.sample_rate       = static_cast<int>(get_uint(s, "sr"));
            sr.stream_id         =
                static_cast<uint32_t>(get_uint(s, "streamID"));
            sr.audio_buffer_size =
                static_cast<uint32_t>(get_uint(s, "audioBufferSize"));
            sr.stream_key        = get_data(s, "shk");
            sr.stream_iv         = get_data(s, "shiv");
            out.streams.push_back(sr);
            LOG_INFO << "  stream[" << i << "] type=" << sr.type
                     << " streamConnectionID=" << sr.stream_conn_id
                     << " remoteControlPort=" << sr.remote_control_port
                     << " sr=" << sr.sample_rate
                     << " ct=" << sr.ct
                     << " spf=" << sr.spf
                     << " shk=" << sr.stream_key.size() << "B"
                     << " shiv=" << sr.stream_iv.size() << "B";
        }
    }

    plist_free(root);
    return out.has_keys || out.has_streams ||
           !out.timing_protocol.empty() ||
           !out.device_id.empty() ||
           out.is_remote_control_only;
}

bool parse_airplay2_rate_anchor(
    const unsigned char* body,
    std::size_t len,
    Airplay2RateAnchor& out) {
    out = {};
    if (!body || len < 8) return false;

    plist_t root = nullptr;
    plist_from_bin(
        reinterpret_cast<const char*>(body),
        static_cast<uint32_t>(len), &root);
    if (!root || plist_get_node_type(root) != PLIST_DICT) {
        if (root) plist_free(root);
        return false;
    }

    const uint64_t seconds = get_uint(root, "networkTimeSecs");
    const uint64_t wire_fraction = get_uint(root, "networkTimeFrac");
    const uint64_t rtp_time = get_uint(root, "rtpTime");
    const uint64_t rate = get_uint(root, "rate");
    out.timeline_id = get_uint(root, "networkTimeTimelineID");

    // AirPlay 把秒以下部分编码成 64 位定点小数。先取高 32 位再乘
    // 10^9，可避免直接乘 64 位值时的溢出，精度约为 1/4 ns。
    const uint64_t fraction_high = wire_fraction >> 32;
    const uint64_t nanoseconds =
        (fraction_high * UINT64_C(1'000'000'000)) >> 32;
    out.network_time_ns =
        seconds * UINT64_C(1'000'000'000) + nanoseconds;
    out.rtp_time = static_cast<uint32_t>(rtp_time);
    out.playing = (rate & 1U) != 0;
    plist_free(root);
    return true;
}

bool looks_like_hap_tlv(const std::vector<unsigned char>& body) {
    if (body.size() < 2) return false;
    std::size_t offset = 0;
    bool saw_known_type = false;
    while (offset < body.size()) {
        if (body.size() - offset < 2) return false;
        const unsigned char type = body[offset];
        const std::size_t length = body[offset + 1];
        offset += 2;
        if (length > body.size() - offset) return false;
        saw_known_type |= type <= 7;
        offset += length;
    }
    return saw_known_type;
}

// --- Response builder -------------------------------------------------------

struct Airplay2SetupResponse::Impl {
    plist_t root = plist_new_dict();
    ~Impl() { if (root) plist_free(root); }
};

Airplay2SetupResponse::Airplay2SetupResponse()
    : impl_(std::make_unique<Impl>()) {}
Airplay2SetupResponse::~Airplay2SetupResponse() = default;

void Airplay2SetupResponse::add_session(
    uint16_t event_lport,
    uint16_t timing_lport,
    const std::string& local_ip,
    const std::string& device_id,
    bool ptp) {
    set_uint(impl_->root, "eventPort", event_lport);
    set_uint(impl_->root, "timingPort", ptp ? 0 : timing_lport);
    if (ptp) {
        plist_t peer = plist_new_dict();
        plist_t addresses = plist_new_array();
        plist_array_append_item(
            addresses, plist_new_string(local_ip.c_str()));
        plist_dict_set_item(peer, "Addresses", addresses);
        plist_dict_set_item(peer, "ID", plist_new_string(device_id.c_str()));
        plist_dict_set_item(impl_->root, "timingPeerInfo", peer);
    }
    LOG_INFO << "airplay2 SETUP response: eventPort=" << event_lport
             << " timingPort=" << (ptp ? 0 : timing_lport)
             << (ptp ? " PTP peer" : " NTP");
}

void Airplay2SetupResponse::add_streams(
    const std::vector<StreamRequest>&    requests,
    const std::vector<StreamAllocation>& allocated) {

    plist_t arr = plist_new_array();
    for (std::size_t i = 0; i < requests.size() && i < allocated.size(); ++i) {
        plist_t s = plist_new_dict();
        const int type = requests[i].type;
        set_uint(s, "type",     type);
        set_uint(s, "dataPort", allocated[i].data_port);
        // Per UxPlay: type 96 (audio) also carries controlPort in the response,
        // type 110 (mirror video) doesn't.
        if (type == 96) {
            set_uint(s, "controlPort", allocated[i].control_port);
        } else if (type == 103) {
            set_uint(s, "audioBufferSize",
                     requests[i].audio_buffer_size
                         ? requests[i].audio_buffer_size
                         : 0x800000);
        }
        plist_array_append_item(arr, s);
        LOG_INFO << "  stream[" << i << "] type=" << type
                 << " -> dataPort=" << allocated[i].data_port
                 << (type == 96 ? " controlPort=" : "")
                 << (type == 96 ? std::to_string(allocated[i].control_port) : std::string());
    }
    plist_dict_set_item(impl_->root, "streams", arr);
}

std::vector<unsigned char> Airplay2SetupResponse::serialize() {
    char*    buf = nullptr;
    uint32_t len = 0;
    plist_to_bin(impl_->root, &buf, &len);
    std::vector<unsigned char> out;
    if (buf) {
        out.assign(reinterpret_cast<unsigned char*>(buf),
                   reinterpret_cast<unsigned char*>(buf) + len);
        std::free(buf);
    }
    return out;
}

} // namespace ap::airplay
