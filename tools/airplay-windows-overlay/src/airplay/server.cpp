#include "airplay/server.h"
#include "airplay/client_session.h"
#include "airplay/hls_session.h"
#include "video/hls_player.h"
#include "airplay/reverse_channel.h"
#include "log.h"
#include "platform/macast_bridge.h"

namespace ap::airplay {

bool Server::start(const DeviceContext& ctx, uint16_t port) {
    ctx_ = ctx;
    return tcp_.start(port, [this](ap::net::ClientSocket c) {
        handle_client(std::move(c));
    });
}

void Server::stop() {
    tcp_.stop();
}

void Server::disconnect_clients() {
    tcp_.close_all_clients([this] {
        if (ctx_.hls_player) ctx_.hls_player->stop();
        HlsSessionRegistry::instance().set_active_session("");
        if (ctx_.renderer) {
            ctx_.renderer->clear_session();
            ctx_.renderer->clear_active_device();
        }
    });
}

bool Server::audio_control_available(const std::string& session_id) {
    if (session_id.empty()) return false;
    std::lock_guard<std::mutex> lock(remotes_mutex_);
    for (const auto& entry : remotes_) if (entry.second->available(session_id)) return true;
    return false;
}

bool Server::control_audio(MediaCommand command, uint32_t position_ms, const std::string& session_id) {
    if (session_id.empty()) return false;
    std::shared_ptr<DacpRemote> selected;
    {
        std::lock_guard<std::mutex> lock(remotes_mutex_);
        for (const auto& entry : remotes_) {
            if (entry.second->available(session_id)) { selected = entry.second; break; }
        }
    }
    return selected && selected->send(command, position_ms, session_id);
}

void Server::handle_client(ap::net::ClientSocket client) {
    // TCP probes are acknowledged even when media is paused or the screen
    // is static. An unreachable phone must not retain the session forever.
    if (!ap::net::enable_tcp_keepalive(client.fd)) {
        LOG_WARN << "Could not configure control TCP keepalive: "
                 << ap::net::last_error_string();
    }
    RequestReader reader(client.cancelled);
    Request req;

    // Peer is formatted by tcp_server as "ip:port" for v4 and v4-mapped,
    // "[ip]:port" for real IPv6. Strip brackets first when present, then
    // cut at the final ':'. The bracketed form is unambiguous; the
    // unbracketed-v4 form has exactly one ':'.
    std::string peer_ip = client.peer;
    if (!peer_ip.empty() && peer_ip.front() == '[') {
        const auto close_br = peer_ip.find(']');
        peer_ip = (close_br == std::string::npos)
                    ? peer_ip.substr(1)
                    : peer_ip.substr(1, close_br - 1);
    } else {
        const auto colon = peer_ip.rfind(':');
        if (colon != std::string::npos) peer_ip.resize(colon);
    }

    auto remote = std::make_shared<DacpRemote>(peer_ip);
    {
        std::lock_guard<std::mutex> lock(remotes_mutex_);
        remotes_[client.fd] = remote;
    }
    ClientSession session(*ctx_.identity, peer_ip,
                          static_cast<int>(client.fd), ctx_.renderer,
                          ctx_.mirror_hwaccel);
    session.live = ctx_.live;
    session.preempt_other_clients = [this, fd = client.fd] {
        tcp_.close_clients_except(fd, [this] {
            if (ctx_.hls_player) ctx_.hls_player->stop();
            HlsSessionRegistry::instance().set_active_session("");
            if (ctx_.renderer) {
                ctx_.renderer->clear_session();
                ctx_.renderer->clear_active_device();
            }
        });
    };

    while (true) {
        // AP2 HomeKit 在处理最后一条 pair-setup 请求时创建 cipher，但该响应
        // 仍必须明文发送；从下一帧完整请求开始使用加密通道。
        const RequestReader::Source source =
            [&](char* data, std::size_t size) -> int {
                if (session.ap2_session &&
                    session.ap2_session->encryption_ready()) {
                    return session.ap2_session->read_encrypted(
                        client.fd, data, size);
                }
                return ap::net::recv_interruptible(
                    client.fd, data, static_cast<int>(size),
                    client.cancelled.get());
            };
        if (!reader.read(source, req)) break;

        const bool encrypted_request =
            session.ap2_session &&
            session.ap2_session->encryption_ready();
        if (req.method == "SETUP" || req.method == "ANNOUNCE" || req.method == "RECORD") {
            const std::string dacp_id = req.header("dacp-id");
            const std::string active_remote = req.header("active-remote");
            LOG_INFO << "control " << client.peer << " method=" << req.method
                     << " content-type=" << req.header("content-type")
                     << " DACP-ID=" << (dacp_id.empty() ? "-" : dacp_id)
                     << " Active-Remote-length=" << active_remote.size();
        }
        Response res = dispatch(ctx_, session, req);
        remote->update(req.header("dacp-id"), req.header("active-remote"),
                       session.streams && session.streams->audio_only(),
                       session.streams ? session.streams->session_id() : std::string{});
        if (client.cancelled->load()) break;
        if (res.silent) continue;
        std::string wire = res.serialize();
        const bool sent = encrypted_request
            ? session.ap2_session->write_encrypted(
                  client.fd, wire.data(), wire.size())
            : ap::net::send_all(client.fd, wire.data(),
                                static_cast<int>(wire.size())) >= 0;
        if (!sent) {
            LOG_WARN << client.peer << ": send failed";
            break;
        }
        if ((req.uri == "/pair-setup" || req.uri == "/pair-verify") &&
            session.ap2_session &&
            session.ap2_session->cipher_established()) {
            session.ap2_session->activate_encryption();
        }
    }
    if (!session.reverse_session_id.empty()) {
        ReverseChannelRegistry::instance().unregister_socket(
            session.reverse_session_id);
    }
    LOG_INFO << "control read ended peer=" << client.peer
             << " receiver_cancelled=" << client.cancelled->load()
             << " streams=" << (session.streams ? "yes" : "no");
    // Close the media transports before releasing the control connection.
    const std::string ended_session = session.streams
        ? session.streams->session_id() : std::string{};
    session.streams.reset();
    if (ctx_.renderer && !ended_session.empty()) {
        if (ctx_.renderer->clear_session_if_active(ended_session)) {
            ap::platform::MacastBridge::instance().clear_media();
        }
    }
    LOG_INFO << "closing " << client.peer;
    {
        std::lock_guard<std::mutex> lock(remotes_mutex_);
        remotes_.erase(client.fd);
    }
}

} // namespace ap::airplay
