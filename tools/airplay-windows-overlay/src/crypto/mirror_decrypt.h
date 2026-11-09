#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Forward-declare OpenSSL type so this header stays self-contained.
struct evp_cipher_ctx_st;
typedef struct evp_cipher_ctx_st EVP_CIPHER_CTX;

namespace ap::crypto {

// Per-stream video decryption context for the AirPlay 2 mirror stream.
// Legacy pairing uses AES-128-CTR, while HomeKit transient pairing derives
// a per-stream ChaCha20-Poly1305 key with HKDF-SHA512. Key derivation follows
// UxPlay's mirror_buffer implementation.
//
//   aes_key_video = SHA-512("AirPlayStreamKey<streamConnectionID>" || aes_key_audio)[0..16]
//   aes_iv_video  = SHA-512("AirPlayStreamIV<streamConnectionID>"  || aes_key_audio)[0..16]
//
// where aes_key_audio is the 16-byte output of fairplay_decrypt() over the
// 72-byte ekey iOS sends in SETUP, and streamConnectionID is the uint64
// field iOS sends inside `streams[]` of the per-stream SETUP.
//
// The resulting AES-CTR context is used as a CONTINUOUS stream across all
// encrypted frames of the session — we never re-init between frames, the
// counter advances naturally. UxPlay's mirror_buffer_decrypt manages partial
// blocks by hand; OpenSSL's EVP AES-CTR does it automatically, so our
// `decrypt(data, len)` is simply EVP_EncryptUpdate.
class MirrorDecrypt {
public:
    MirrorDecrypt();
    ~MirrorDecrypt();

    MirrorDecrypt(const MirrorDecrypt&)            = delete;
    MirrorDecrypt& operator=(const MirrorDecrypt&) = delete;

    // Derive the video key/IV and initialise the AES-CTR context. Returns
    // false on any OpenSSL or length error.
    bool init(const std::vector<unsigned char>& aes_key_audio,
              uint64_t stream_connection_id);

    // HomeKit transient-pairing video path:
    //   salt = "DataStream-Salt<streamConnectionID>"
    //   info = "DataStream-Output-Encryption-Key"
    // Each frame authenticates the 128-byte mirror frame header as AAD and
    // carries a 16-byte Poly1305 tag at the end of the payload.
    bool init_hap(
        const std::vector<unsigned char>& homekit_session_key,
        uint64_t stream_connection_id);

    // In-place decrypt `len` bytes of encrypted NAL payload. The EVP context
    // carries the CTR counter between calls.
    bool decrypt(unsigned char* data, int len);
    bool decrypt_hap(
        const unsigned char* aad,
        std::size_t aad_len,
        std::vector<unsigned char>& payload);

    bool is_initialized() const { return ctx_ != nullptr || hap_ready_; }
    bool is_hap() const { return hap_ready_; }

private:
    EVP_CIPHER_CTX* ctx_{nullptr};
    std::vector<unsigned char> hap_key_;
    uint64_t hap_counter_{0};
    bool hap_ready_{false};
};

} // namespace ap::crypto
