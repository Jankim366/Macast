#include "airplay/streams.h"
#include "log.h"

#include <cstring>
#include <random>
#include <sstream>

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
#endif

namespace ap::airplay {
namespace {

// `Transport: RTP/AVP/UDP;unicast;mode=record;control_port=6001;timing_port=6002`
// We pull out the client-side ports by substring; the full grammar (RFC 2326
// §12.39) has more, but this is what iOS actually sends.
uint16_t pick_port(const std::string& header, const char* key) {
    auto pos = header.find(key);
    if (pos == std::string::npos) return 0;
    pos += std::strlen(key);
    if (pos >= header.size() || header[pos] != '=') return 0;
    ++pos;
    uint16_t v = 0;
    while (pos < header.size() && std::isdigit(static_cast<unsigned char>(header[pos]))) {
        v = static_cast<uint16_t>(v * 10 + (header[pos] - '0'));
        ++pos;
    }
    return v;
}

// Bind a UDP socket on [::]:0 (dual-stack ephemeral) and return both
// the socket and the port the OS picked. Returns (INVALID_SOCK, 0) on
// failure. The socket accepts both IPv4 and IPv6 senders — when iOS
// reaches us over IPv6 it also sends its RTP audio/control packets
// to the v6 address returned here, and an IPv4-only UDP socket would
// silently drop those.
std::pair<socket_t, uint16_t> bind_udp() {
    socket_t s = ::socket(AF_INET6, SOCK_DGRAM, 0);
    if (s == INVALID_SOCK) {
        // Kernel without IPv6: fall back to v4-only.
        s = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (s == INVALID_SOCK) return {INVALID_SOCK, 0};

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port        = 0;
        if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ap::net::close_socket(s);
            return {INVALID_SOCK, 0};
        }
#if defined(_WIN32)
        int len = sizeof(addr);
#else
        socklen_t len = sizeof(addr);
#endif
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            ap::net::close_socket(s);
            return {INVALID_SOCK, 0};
        }
        return {s, ntohs(addr.sin_port)};
    }

    int v6only = 0;
    ::setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY,
                 reinterpret_cast<const char*>(&v6only), sizeof(v6only));

    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr   = in6addr_any;
    addr.sin6_port   = 0;
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ap::net::close_socket(s);
        return {INVALID_SOCK, 0};
    }
#if defined(_WIN32)
    int len = sizeof(addr);
#else
    socklen_t len = sizeof(addr);
#endif
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ap::net::close_socket(s);
        return {INVALID_SOCK, 0};
    }
    return {s, ntohs(addr.sin6_port)};
}

std::string random_session_id() {
    static std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<uint32_t> dist(1'000'000, 9'999'999);
    std::ostringstream os;
    os << dist(rng);
    return os.str();
}

std::pair<socket_t, uint16_t> bind_tcp() {
    auto bind_family = [](int family) -> std::pair<socket_t, uint16_t> {
        socket_t s = ::socket(family, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCK) return {INVALID_SOCK, 0};
        int reuse = 1;
        ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        if (family == AF_INET6) {
            int v6only = 0;
            ::setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY,
                         reinterpret_cast<const char*>(&v6only),
                         sizeof(v6only));
        }

        sockaddr_storage address{};
        int length = 0;
        if (family == AF_INET6) {
            auto* addr = reinterpret_cast<sockaddr_in6*>(&address);
            addr->sin6_family = AF_INET6;
            addr->sin6_addr = in6addr_any;
            addr->sin6_port = 0;
            length = sizeof(*addr);
        } else {
            auto* addr = reinterpret_cast<sockaddr_in*>(&address);
            addr->sin_family = AF_INET;
            addr->sin_addr.s_addr = htonl(INADDR_ANY);
            addr->sin_port = 0;
            length = sizeof(*addr);
        }
        if (::bind(s, reinterpret_cast<sockaddr*>(&address), length) != 0 ||
            ::listen(s, 4) != 0) {
            ap::net::close_socket(s);
            return {INVALID_SOCK, 0};
        }
#if defined(_WIN32)
        int bound_length = sizeof(address);
#else
        socklen_t bound_length = sizeof(address);
#endif
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&address),
                          &bound_length) != 0) {
            ap::net::close_socket(s);
            return {INVALID_SOCK, 0};
        }
        const uint16_t port =
            family == AF_INET6
                ? ntohs(reinterpret_cast<sockaddr_in6*>(&address)->sin6_port)
                : ntohs(reinterpret_cast<sockaddr_in*>(&address)->sin_port);
        return {s, port};
    };

    auto result = bind_family(AF_INET6);
    if (result.first == INVALID_SOCK) result = bind_family(AF_INET);
    return result;
}

} // namespace

StreamSession::StreamSession() : session_id_(random_session_id()) {}

StreamSession::~StreamSession() { teardown(); }

bool StreamSession::setup_legacy(const std::string& transport, StreamPorts& allocated) {
    client_ports_.server  = pick_port(transport, "server_port");   // rare from iOS
    client_ports_.control = pick_port(transport, "control_port");
    client_ports_.timing  = pick_port(transport, "timing_port");

    // Data socket: always bind. Control + timing only when iOS mentions them.
    auto [d_sock, d_port] = bind_udp();
    if (d_sock == INVALID_SOCK) {
        LOG_ERROR << "SETUP: cannot bind UDP data socket";
        return false;
    }
    data_sock_       = d_sock;
    allocated.server = d_port;

    if (client_ports_.control) {
        auto [c_sock, c_port] = bind_udp();
        if (c_sock == INVALID_SOCK) {
            LOG_ERROR << "SETUP: cannot bind UDP control socket";
            teardown();
            return false;
        }
        ctrl_sock_        = c_sock;
        allocated.control = c_port;
    }
    if (client_ports_.timing) {
        auto [t_sock, t_port] = bind_udp();
        if (t_sock == INVALID_SOCK) {
            LOG_ERROR << "SETUP: cannot bind UDP timing socket";
            teardown();
            return false;
        }
        timing_sock_     = t_sock;
        allocated.timing = t_port;
    }

    LOG_INFO << "SETUP session=" << session_id_
             << "  client(control=" << client_ports_.control
             << ",timing="           << client_ports_.timing
             << ")  server(data=" << allocated.server
             << ",control="         << allocated.control
             << ",timing="          << allocated.timing << ")";
    return true;
}

bool StreamSession::setup_session(
    uint16_t& event_port,
    uint16_t& timing_port,
    const std::vector<unsigned char>& shared_secret) {
    if (!shared_secret.empty()) {
        event_secret_ = shared_secret;
        event_server_ = std::make_unique<ap::net::TcpServer>();
        if (!event_server_->start(
                0,
                [secret = shared_secret](ap::net::ClientSocket client) {
                    // AP2 event 通道用于接收端主动通知，与 encrypted control
                    // 流分离；单设备场景只需保持连接并消费心跳帧。
                    ap::crypto::Ap2CipherChannel channel(4, secret);
                    if (!channel.ready()) {
                        LOG_WARN << "AP2 event channel cipher setup failed";
                        return;
                    }
                    char buffer[4096];
                    while (!client.cancelled->load() &&
                           channel.read_encrypted(
                               client.fd, buffer, sizeof(buffer)) > 0) {
                    }
                })) {
            LOG_ERROR << "SETUP (airplay2 session): cannot start event server";
            event_secret_.clear();
            return false;
        }
        event_port = event_server_->port();
    } else {
        auto [e_sock, e_port] = bind_udp();
        if (e_sock == INVALID_SOCK) {
            LOG_ERROR << "SETUP (airplay2 session): cannot bind event socket";
            return false;
        }
        event_sock_ = e_sock;
        event_port = e_port;
    }
    auto [t_sock, t_port] = bind_udp();
    if (t_sock == INVALID_SOCK) {
        LOG_ERROR << "SETUP (airplay2 session): cannot bind timing socket";
        if (event_server_) {
            event_server_->stop();
            event_server_.reset();
        }
        if (event_sock_ != INVALID_SOCK) {
            ap::net::close_socket(event_sock_);
            event_sock_ = INVALID_SOCK;
        }
        return false;
    }
    ap2_timing_sock  = t_sock;
    timing_port      = t_port;
    LOG_INFO << "SETUP session=" << session_id_
             << " airplay2 event=" << event_port << " timing=" << t_port;
    return true;
}

bool StreamSession::setup_stream(int type,
                                 uint16_t& data_port,
                                 uint16_t& control_port,
                                 const StreamOpts& opts) {
    control_port = 0;

    // Type 110 (mirror video) uses a TCP byte stream, not UDP RTP. iOS will
    // open a TCP connection to the returned dataPort right after SETUP and
    // push H.264 NAL units over it. UxPlay implements this in raop_rtp_mirror.c.
    if (type == 110) {
        mirror_ = std::make_unique<MirrorListener>();

        // Decoder initialization needs the renderer's D3D11 device.
        if (renderer_) mirror_->attach_renderer(renderer_);

        // Wire AES-CTR decryption before starting the accept loop.
        if (!opts.aes_key.empty() && opts.stream_connection_id != 0) {
            if (!mirror_->enable_decrypt(opts.aes_key, opts.stream_connection_id,
                                         opts.mirror_hwaccel)) {
                LOG_WARN << "SETUP stream 110: could not init AES-CTR decrypt "
                            "(stream will be logged encrypted)";
            }
        }

        uint16_t p = 0;
        if (!mirror_->start(p)) {
            mirror_.reset();
            return false;
        }
        data_port = p;
        if (audio_) audio_->set_synchronized(false);

        StreamChannel ch;
        ch.type         = type;
        ch.data_port    = p;
        ch.data_sock    = INVALID_SOCK; // owned by MirrorListener
        channels_.push_back(ch);

        LOG_INFO << "SETUP session=" << session_id_
                 << " stream type=110 (TCP) data=" << p;
        return true;
    }

    if (type == 103) {
        auto [data_listener, data_listener_port] = bind_tcp();
        if (data_listener == INVALID_SOCK) return false;

        buffered_audio_ =
            std::make_unique<ap::audio::BufferedAudioReceiver>();
        ap::audio::BufferedAudioReceiver::Config config;
        config.listener = data_listener;
        config.stream_key = opts.stream_key;
        config.ct = opts.ct;
        config.sample_rate = opts.sample_rate;
        config.spf = opts.spf;
        config.audio_buffer_size =
            opts.audio_buffer_size ? opts.audio_buffer_size : 0x800000;
        config.ptp = ptp_;
        if (!buffered_audio_->start(std::move(config))) {
            ap::net::close_socket(data_listener);
            buffered_audio_.reset();
            return false;
        }
        if (ap2_anchor_valid_) {
            buffered_audio_->set_ap2_rate_anchor(
                ap2_timeline_id_, ap2_anchor_rtp_,
                ap2_anchor_network_ns_, ap2_playing_);
        }

        data_port = data_listener_port;
        control_port = 0;
        StreamChannel channel;
        channel.type = type;
        channel.data_port = data_port;
        channel.control_port = control_port;
        channel.data_sock = INVALID_SOCK;
        channel.control_sock = INVALID_SOCK;
        channels_.push_back(channel);
        audio_control_session_ = true;
        LOG_INFO << "SETUP session=" << session_id_
                 << " stream type=103 (TCP buffered audio) data="
                 << data_port;
        return true;
    }

    // Type 96 (audio) and any other UDP-based stream.
    auto [d_sock, d_port] = bind_udp();
    if (d_sock == INVALID_SOCK) return false;
    auto [c_sock, c_port] = bind_udp();
    if (c_sock == INVALID_SOCK) { ap::net::close_socket(d_sock); return false; }

    data_port    = d_port;
    control_port = c_port;
    LOG_INFO << "SETUP session=" << session_id_ << " stream type=" << type
             << " (UDP) data=" << d_port << " control=" << c_port;

    if (type == 96) {
        // Start the audio receiver on the data socket. It takes ownership.
        audio_ = std::make_unique<ap::audio::AudioReceiver>();
        ap::audio::AudioReceiver::Config ac;
        ac.data_sock   = d_sock;
        ac.control_sock = c_sock;
        ac.ntp = ntp_.get();
        ac.synchronize = mirror_ == nullptr;
        ac.aes_key     = opts.aes_key;
        ac.aes_iv      = opts.aes_iv;
        ac.ct          = opts.ct;
        ac.sample_rate = opts.sample_rate;
        ac.renderer    = renderer_;
        if (!audio_->start(std::move(ac), std::move(paused_audio_))) {
            LOG_WARN << "SETUP stream 96: AudioReceiver failed to start "
                        "(UDP socket left bound, no decrypt)";
            // If start() failed before taking ownership, we close the sock here.
            ap::net::close_socket(d_sock);
            audio_.reset();
        }

        // AudioReceiver borrows the control socket to read RTP/NTP anchors.
        // Keep ownership here; stop the receiver before closing it.
        StreamChannel ch;
        ch.type         = type;
        ch.data_port    = d_port;
        ch.control_port = c_port;
        ch.data_sock    = INVALID_SOCK;   // consumed by AudioReceiver
        ch.control_sock = c_sock;
        channels_.push_back(ch);
        audio_control_session_ = true;
        return true;
    }

    // Fallback: keep both sockets in channels_ untouched.
    StreamChannel ch;
    ch.type         = type;
    ch.data_port    = d_port;
    ch.control_port = c_port;
    ch.data_sock    = d_sock;
    ch.control_sock = c_sock;
    channels_.push_back(ch);
    return true;
}

void StreamSession::stop_stream(int type) {
    LOG_INFO << "stop_stream type=" << type << " session=" << session_id_;
    if (type == 110 && mirror_) {
        mirror_->stop();
        mirror_.reset();
        if (audio_) audio_->set_synchronized(true);
    }
    if (type == 96 && audio_) {
        paused_audio_ = audio_->suspend();
        audio_.reset();
    }
    if (type == 103 && buffered_audio_) {
        buffered_audio_->stop();
        buffered_audio_.reset();
    }
    // Remove any audio channels matching the type.
    for (auto it = channels_.begin(); it != channels_.end(); ) {
        if (it->type == type) {
            if (it->data_sock    != INVALID_SOCK) ap::net::close_socket(it->data_sock);
            if (it->control_sock != INVALID_SOCK) ap::net::close_socket(it->control_sock);
            it = channels_.erase(it);
        } else {
            ++it;
        }
    }
}

bool StreamSession::start_ntp(const std::string& remote_ip, uint16_t remote_port) {
    if (ap2_timing_sock == INVALID_SOCK) {
        LOG_ERROR << "start_ntp: timing socket not bound (call setup_session first)";
        return false;
    }
    // Preserve the address borrowed by an active AudioReceiver on repeated SETUP.
    if (!ntp_) ntp_ = std::make_unique<NtpClient>();
    else ntp_->stop();
    return ntp_->start(ap2_timing_sock, remote_ip, remote_port);
}

void StreamSession::set_audio_volume_db(float db) {
    if (audio_) audio_->set_volume_db(db);
    if (buffered_audio_) buffered_audio_->set_volume_db(db);
}

void StreamSession::flush_audio(std::optional<uint32_t> boundary) {
    if (audio_) audio_->flush(boundary);
    if (buffered_audio_) buffered_audio_->flush();
}

void StreamSession::resume_audio() {
    if (audio_) audio_->resume();
}

void StreamSession::set_audio_track(const std::string& track) {
    if (!audio_track_.empty() && track != audio_track_) {
        paused_audio_ = {};
        if (audio_) audio_->discard_pause();
    }
    audio_track_ = track;
}

void StreamSession::set_ap2_rate_anchor(
    uint64_t timeline_id,
    uint32_t rtp_time,
    uint64_t network_time_ns,
    bool playing) {
    ap2_timeline_id_ = timeline_id;
    ap2_anchor_rtp_ = rtp_time;
    ap2_anchor_network_ns_ = network_time_ns;
    ap2_anchor_valid_ = true;
    ap2_playing_ = playing;
    if (buffered_audio_) {
        buffered_audio_->set_ap2_rate_anchor(
            timeline_id, rtp_time, network_time_ns, playing);
    }
}

bool StreamSession::audio_only() const {
    if (mirror_) return false;
    if (audio_control_session_) return true;
    if (audio_) return true;
    if (buffered_audio_) return true;
    for (const auto& channel : channels_) if (channel.type == 96) return true;
    return false;
}

void StreamSession::teardown() {
    audio_control_session_ = false;
    ap2_anchor_valid_ = false;
    ap2_playing_ = true;
    paused_audio_ = {};
    audio_track_.clear();
    LOG_INFO << "StreamSession::teardown begin session=" << session_id_
             << " ntp=" << (ntp_ ? "yes" : "no")
             << " mirror=" << (mirror_ ? "yes" : "no")
             << " audio=" << (audio_ ? "yes" : "no")
             << " channels=" << channels_.size();
    if (mirror_) { LOG_INFO << "teardown: mirror stop"; mirror_->stop(); mirror_.reset(); }
    if (audio_)  { LOG_INFO << "teardown: audio stop"; audio_->stop();  audio_.reset(); }
    if (buffered_audio_) {
        LOG_INFO << "teardown: buffered audio stop";
        buffered_audio_->stop();
        buffered_audio_.reset();
    }
    if (event_server_) {
        LOG_INFO << "teardown: event server stop";
        event_server_->stop();
        event_server_.reset();
    }

    if (ntp_)    { LOG_INFO << "teardown: ntp stop"; ntp_->stop(); ntp_.reset(); }

    LOG_INFO << "teardown: closing session sockets";
    for (auto* sp : { &data_sock_, &ctrl_sock_, &timing_sock_,
                       &event_sock_, &ap2_timing_sock }) {
        if (*sp != INVALID_SOCK) {
            ap::net::close_socket(*sp);
            *sp = INVALID_SOCK;
        }
    }
    LOG_INFO << "teardown: closing channel sockets";
    for (auto& ch : channels_) {
        if (ch.data_sock    != INVALID_SOCK) ap::net::close_socket(ch.data_sock);
        if (ch.control_sock != INVALID_SOCK) ap::net::close_socket(ch.control_sock);
    }
    channels_.clear();
    event_secret_.clear();
    LOG_INFO << "teardown: done";
}

} // namespace ap::airplay
