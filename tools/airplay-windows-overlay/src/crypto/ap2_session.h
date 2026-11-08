#pragma once

#include "net/socket.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace ap::crypto {

// 对 pair_ap 的 HomeKit AirPlay 2 配对与加密通道适配层。
//
// AP2 临时配对在 pair-setup 阶段生成 shared secret，control/event 通道再由
// 该密钥分别派生 ChaCha20-Poly1305 密钥。此类隔离 pair_ap 的 C 状态和 TCP
// 半帧缓存，避免协议细节泄漏到 RTSP 分发层。
class Ap2Session {
public:
    explicit Ap2Session(std::string device_id);
    ~Ap2Session();

    Ap2Session(const Ap2Session&) = delete;
    Ap2Session& operator=(const Ap2Session&) = delete;

    // 处理 AP2 HomeKit pair-setup 的一个 TLV 阶段并生成对应响应。
    bool process_pair_setup(
        const std::vector<unsigned char>& request,
        std::vector<unsigned char>& response);
    // 处理持久配对的 pair-verify TLV 阶段；临时配对通常不会走到这里。
    bool process_pair_verify(
        const std::vector<unsigned char>& request,
        std::vector<unsigned char>& response);

    bool encryption_ready() const {
        return control_cipher_ != nullptr && encryption_active_;
    }
    bool cipher_established() const { return control_cipher_ != nullptr; }
    void activate_encryption() { encryption_active_ = cipher_established(); }
    const std::vector<unsigned char>& shared_secret() const {
        return shared_secret_;
    }

    // 从加密 TCP 字节流中读取并解密至少一个完整明文块。
    int read_encrypted(socket_t socket, void* data, std::size_t size);
    // 将一段明文按 pair_ap 的 AP2 block 格式加密后完整发送。
    bool write_encrypted(socket_t socket, const void* data, std::size_t size);

private:
    bool install_control_cipher(const unsigned char* secret, std::size_t size);
    void reset_pairing();

    std::string device_id_;
    void* setup_{nullptr};
    void* verify_{nullptr};
    void* control_cipher_{nullptr};
    bool encryption_active_{false};
    std::vector<unsigned char> shared_secret_;
    std::vector<unsigned char> encrypted_input_;
    std::vector<unsigned char> decrypted_input_;
    std::mutex mutex_;
};

// 返回 pair_ap 根据 device_id/种子生成的 Ed25519 公钥。
// mDNS `pk` 与 pair-setup 必须使用同一公钥，否则客户端会拒绝后续验证。
std::vector<unsigned char> airplay2_public_key(const std::string& device_id);
// 设置 HomeKit pair-setup 使用的 32 字节 Ed25519 私钥种子。
bool configure_ap2_pairing_seed(
    const std::vector<unsigned char>& seed);

// 一个双向密钥配对的 AP2 加密通道。
// RTSP control 使用 channel 3，发送端反向连接的 event 通道使用 channel 4。
class Ap2CipherChannel {
public:
    Ap2CipherChannel(
        int channel,
        const std::vector<unsigned char>& shared_secret);
    ~Ap2CipherChannel();

    Ap2CipherChannel(const Ap2CipherChannel&) = delete;
    Ap2CipherChannel& operator=(const Ap2CipherChannel&) = delete;

    bool ready() const { return cipher_ != nullptr; }
    int read_encrypted(socket_t socket, void* data, std::size_t size);
    bool write_encrypted(socket_t socket, const void* data, std::size_t size);

private:
    void* cipher_{nullptr};
    std::vector<unsigned char> encrypted_input_;
    std::vector<unsigned char> decrypted_input_;
    std::mutex mutex_;
};

} // namespace ap::crypto
