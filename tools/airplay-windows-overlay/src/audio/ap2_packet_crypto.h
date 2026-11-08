#pragma once

#include <vector>

namespace ap::audio {

// 解密 AirPlay 2 buffered audio 的 type-103 RTP 包。
//
// 包内固定布局为 RTP header(12) + ciphertext + Poly1305 tag(16) +
// 8-byte nonce；密钥必须是 SETUP `shk` 提供的 32 字节 ChaCha20 键。
bool decrypt_buffered_packet(
    const std::vector<unsigned char>& stream_key,
    const std::vector<unsigned char>& packet,
    std::vector<unsigned char>& plaintext);

} // namespace ap::audio
