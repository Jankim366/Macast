#include "airplay/airplay2_setup.h"

#include <plist/plist.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::vector<unsigned char> serialize(plist_t root) {
    char* data = nullptr;
    uint32_t size = 0;
    plist_to_bin(root, &data, &size);
    require(data != nullptr && size > 0, "plist serialization");
    std::vector<unsigned char> result(
        reinterpret_cast<unsigned char*>(data),
        reinterpret_cast<unsigned char*>(data) + size);
    std::free(data);
    return result;
}

} // namespace

int main() {
    plist_t session = plist_new_dict();
    plist_dict_set_item(session, "deviceID", plist_new_string("AA:BB:CC:DD:EE:FF"));
    plist_dict_set_item(session, "name", plist_new_string("Macast"));
    plist_dict_set_item(session, "timingProtocol", plist_new_string("PTP"));
    const auto session_body = serialize(session);
    plist_free(session);

    ap::airplay::Airplay2SetupRequest parsed;
    require(ap::airplay::parse_airplay2_setup(
                session_body.data(), session_body.size(), parsed),
            "parse PTP session SETUP");
    require(!parsed.has_streams, "session SETUP has no streams");
    require(parsed.timing_protocol == "PTP", "timing protocol");
    require(parsed.device_id == "AA:BB:CC:DD:EE:FF", "device id");

    plist_t stream_setup = plist_new_dict();
    plist_t streams = plist_new_array();
    plist_t stream = plist_new_dict();
    plist_dict_set_item(stream, "type", plist_new_uint(103));
    plist_dict_set_item(stream, "streamID", plist_new_uint(42));
    plist_dict_set_item(stream, "audioBufferSize",
                        plist_new_uint(0x800000));
    const unsigned char key[32]{0x11};
    plist_dict_set_item(stream, "shk",
                        plist_new_data(
                            reinterpret_cast<const char*>(key), sizeof(key)));
    plist_array_append_item(streams, stream);
    plist_dict_set_item(stream_setup, "streams", streams);
    const auto stream_body = serialize(stream_setup);
    plist_free(stream_setup);

    ap::airplay::Airplay2SetupRequest stream_parsed;
    require(ap::airplay::parse_airplay2_setup(
                stream_body.data(), stream_body.size(), stream_parsed),
            "parse buffered stream SETUP");
    require(stream_parsed.has_streams &&
                stream_parsed.streams.size() == 1,
            "buffered stream count");
    require(stream_parsed.streams[0].type == 103,
            "buffered stream type");
    require(stream_parsed.streams[0].stream_key.size() == 32,
            "buffered stream key");

    plist_t anchor_plist = plist_new_dict();
    plist_dict_set_item(anchor_plist, "networkTimeSecs",
                        plist_new_uint(1234));
    plist_dict_set_item(anchor_plist, "networkTimeFrac",
                        plist_new_uint(UINT64_C(1) << 63));
    plist_dict_set_item(anchor_plist, "networkTimeTimelineID",
                        plist_new_uint(UINT64_C(0x0102030405060708)));
    plist_dict_set_item(anchor_plist, "rtpTime",
                        plist_new_uint(0xF0000000));
    plist_dict_set_item(anchor_plist, "rate", plist_new_uint(1));
    const auto anchor_body = serialize(anchor_plist);
    plist_free(anchor_plist);

    ap::airplay::Airplay2RateAnchor anchor;
    require(ap::airplay::parse_airplay2_rate_anchor(
                anchor_body.data(), anchor_body.size(), anchor),
            "parse SETRATEANCHORI");
    require(anchor.timeline_id == UINT64_C(0x0102030405060708),
            "rate anchor timeline");
    require(anchor.rtp_time == 0xF0000000, "rate anchor RTP time");
    require(anchor.network_time_ns ==
                UINT64_C(1234) * UINT64_C(1'000'000'000) +
                    UINT64_C(500'000'000),
            "rate anchor network time");
    require(anchor.playing, "rate anchor playing");
    require(ap::airplay::looks_like_hap_tlv(
                {0x00, 0x01, 0x00, 0x06, 0x01, 0x01}),
            "identify Method/SeqNo HAP M1");
    require(ap::airplay::looks_like_hap_tlv(
                {0x06, 0x01, 0x01, 0x00, 0x01, 0x00, 0x07, 0x01, 0x10}),
            "identify transient HAP M1");
    require(!ap::airplay::looks_like_hap_tlv(
                std::vector<unsigned char>(32, 0xA5)),
            "reject legacy raw public key");

    ap::airplay::Airplay2SetupResponse response;
    response.add_session(5000, 0, "192.168.1.2",
                         "AA:BB:CC:DD:EE:FF", true);
    ap::airplay::StreamAllocation allocation;
    allocation.data_port = 5001;
    allocation.control_port = 5002;
    response.add_streams(stream_parsed.streams, {allocation});
    const auto response_body = response.serialize();

    plist_t response_root = nullptr;
    plist_from_bin(
        reinterpret_cast<const char*>(response_body.data()),
        static_cast<uint32_t>(response_body.size()), &response_root);
    require(response_root != nullptr, "parse response plist");
    plist_t event_port = plist_dict_get_item(response_root, "eventPort");
    plist_t timing_peer = plist_dict_get_item(response_root, "timingPeerInfo");
    plist_t response_streams = plist_dict_get_item(response_root, "streams");
    plist_t response_stream = response_streams
        ? plist_array_get_item(response_streams, 0) : nullptr;
    plist_t stream_control = response_stream
        ? plist_dict_get_item(response_stream, "controlPort") : nullptr;
    require(event_port != nullptr && timing_peer != nullptr &&
                response_streams != nullptr &&
                plist_array_get_size(response_streams) == 1 &&
                stream_control == nullptr,
            "response AP2 fields");
    plist_free(response_root);

    std::cout << "PASS: AirPlay 2 session, buffered stream and rate anchor\n";
    return 0;
}
