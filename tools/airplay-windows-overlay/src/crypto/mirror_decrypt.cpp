#include "crypto/mirror_decrypt.h"
#include "log.h"

#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <openssl/evp.h>
#include <openssl/kdf.h>

#if defined(_MSC_VER)
#  include <intrin.h>
#endif

namespace ap::crypto {
namespace {

// One-shot probe so we know whether AES-CTR is going through AES-NI on
// this host. Logged at the first MirrorDecrypt::init() of the process.
//
//   1. CPUID.1:ECX[25] — does the CPU expose the AES-NI ISA extension.
//   2. Encrypt 8 MiB of zeros with EVP_aes_128_ctr() and report MB/s.
//      AES-NI typically lands at 3-5 GB/s; pure-software OpenSSL AES
//      sits around 200-400 MB/s on the same hardware. The ~10× gap
//      makes the path obvious without parsing OpenSSL internals.
void log_aesni_status_once() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        bool cpu_aesni = false;
#if defined(_MSC_VER)
        int cpuid_info[4] = {0, 0, 0, 0};
        __cpuid(cpuid_info, 1);
        cpu_aesni = (cpuid_info[2] & (1 << 25)) != 0;
#elif defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
        unsigned int eax, ebx, ecx, edx;
        if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
            cpu_aesni = (ecx & (1u << 25)) != 0;
        }
#endif
        LOG_INFO << "AES-NI CPU support: "
                 << (cpu_aesni ? "yes" : "NO");

        constexpr std::size_t bench_bytes = 8 * 1024 * 1024;
        std::vector<unsigned char> buf(bench_bytes, 0);
        unsigned char key[16] = {0};
        unsigned char iv[16]  = {0};
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) return;
        if (EVP_EncryptInit_ex(ctx, EVP_aes_128_ctr(), nullptr, key, iv) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return;
        }
        const auto t0 = std::chrono::steady_clock::now();
        int out_len = 0;
        EVP_EncryptUpdate(ctx, buf.data(), &out_len,
                          buf.data(), static_cast<int>(buf.size()));
        const auto t1 = std::chrono::steady_clock::now();
        EVP_CIPHER_CTX_free(ctx);

        const double sec  = std::chrono::duration<double>(t1 - t0).count();
        const double mbps = (bench_bytes / (1024.0 * 1024.0)) / sec;
        LOG_INFO << "AES-128-CTR bench: " << static_cast<int>(mbps)
                 << " MB/s ("
                 << (mbps > 1500.0 ? "AES-NI active"
                                   : "software path — AES-NI not engaged")
                 << ')';
    });
}

// SHA-512(salt || secret) → first 16 bytes of digest.
bool sha512_first_16(const char*          salt, std::size_t salt_len,
                     const unsigned char* secret, std::size_t secret_len,
                     unsigned char        out[16]) {
    unsigned char digest[64];
    unsigned int  dlen = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    bool ok = ctx &&
              EVP_DigestInit_ex(ctx, EVP_sha512(), nullptr) == 1 &&
              EVP_DigestUpdate(ctx, salt, salt_len)         == 1 &&
              EVP_DigestUpdate(ctx, secret, secret_len)     == 1 &&
              EVP_DigestFinal_ex(ctx, digest, &dlen)        == 1 &&
              dlen == 64;
    if (ctx) EVP_MD_CTX_free(ctx);
    if (!ok) return false;

    std::memcpy(out, digest, 16);
    return true;
}

bool hkdf_sha512_32(
    const std::vector<unsigned char>& secret,
    const std::string& salt,
    const std::string& info,
    unsigned char out[32]) {
    EVP_PKEY_CTX* context = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    if (!context) return false;
    std::size_t out_len = 32;
    bool ok =
        EVP_PKEY_derive_init(context) == 1 &&
        EVP_PKEY_CTX_set_hkdf_md(context, EVP_sha512()) == 1 &&
        EVP_PKEY_CTX_set1_hkdf_salt(
            context,
            reinterpret_cast<const unsigned char*>(salt.data()),
            static_cast<int>(salt.size())) == 1 &&
        EVP_PKEY_CTX_set1_hkdf_key(
            context, secret.data(),
            static_cast<int>(secret.size())) == 1 &&
        EVP_PKEY_CTX_add1_hkdf_info(
            context,
            reinterpret_cast<const unsigned char*>(info.data()),
            static_cast<int>(info.size())) == 1 &&
        EVP_PKEY_derive(context, out, &out_len) == 1 &&
        out_len == 32;
    EVP_PKEY_CTX_free(context);
    return ok;
}

} // namespace

MirrorDecrypt::MirrorDecrypt() = default;

MirrorDecrypt::~MirrorDecrypt() {
    if (ctx_) EVP_CIPHER_CTX_free(ctx_);
}

bool MirrorDecrypt::init(const std::vector<unsigned char>& aes_key_audio,
                         uint64_t stream_connection_id) {
    if (aes_key_audio.size() != 16) {
        LOG_ERROR << "MirrorDecrypt::init: aes_key_audio size "
                  << aes_key_audio.size() << " (want 16)";
        return false;
    }
    if (ctx_) {
        EVP_CIPHER_CTX_free(ctx_);
        ctx_ = nullptr;
    }
    hap_key_.clear();
    hap_counter_ = 0;
    hap_ready_ = false;

    // The strings are formatted with the stream ID as PRIu64 to match UxPlay.
    char key_salt[64];
    char iv_salt[64];
    std::snprintf(key_salt, sizeof(key_salt),
                  "AirPlayStreamKey%" PRIu64, stream_connection_id);
    std::snprintf(iv_salt, sizeof(iv_salt),
                  "AirPlayStreamIV%"  PRIu64, stream_connection_id);

    unsigned char video_key[16], video_iv[16];
    if (!sha512_first_16(key_salt, std::strlen(key_salt),
                         aes_key_audio.data(), aes_key_audio.size(),
                         video_key)) return false;
    if (!sha512_first_16(iv_salt,  std::strlen(iv_salt),
                         aes_key_audio.data(), aes_key_audio.size(),
                         video_iv))  return false;

    ctx_ = EVP_CIPHER_CTX_new();
    if (!ctx_) return false;
    if (EVP_EncryptInit_ex(ctx_, EVP_aes_128_ctr(), nullptr,
                           video_key, video_iv) != 1) {
        LOG_ERROR << "MirrorDecrypt: EVP_EncryptInit_ex(aes-128-ctr) failed";
        EVP_CIPHER_CTX_free(ctx_);
        ctx_ = nullptr;
        return false;
    }
    log_aesni_status_once();
    LOG_INFO << "MirrorDecrypt initialised (sid=" << stream_connection_id << ')';
    return true;
}

bool MirrorDecrypt::init_hap(
    const std::vector<unsigned char>& homekit_session_key,
    uint64_t stream_connection_id) {
    if (homekit_session_key.size() < 32) {
        LOG_ERROR << "MirrorDecrypt::init_hap: session key size "
                  << homekit_session_key.size() << " (want at least 32)";
        return false;
    }
    if (ctx_) {
        EVP_CIPHER_CTX_free(ctx_);
        ctx_ = nullptr;
    }

    const std::string salt =
        "DataStream-Salt" + std::to_string(stream_connection_id);
    const std::string info = "DataStream-Output-Encryption-Key";
    unsigned char key[32]{};
    if (!hkdf_sha512_32(homekit_session_key, salt, info, key)) {
        LOG_ERROR << "MirrorDecrypt: HKDF-SHA512 video key derivation failed";
        return false;
    }

    hap_key_.assign(key, key + sizeof(key));
    hap_counter_ = 0;
    hap_ready_ = true;
    LOG_INFO << "MirrorDecrypt HAP-ChaCha20 initialized for sid="
             << stream_connection_id;
    return true;
}

bool MirrorDecrypt::decrypt(unsigned char* data, int len) {
    if (!ctx_ || len <= 0) return false;
    int out_len = 0;
    // AES-CTR is symmetric; EVP_EncryptUpdate does the XOR in-place.
    if (EVP_EncryptUpdate(ctx_, data, &out_len, data, len) != 1) return false;
    return out_len == len;
}

bool MirrorDecrypt::decrypt_hap(
    const unsigned char* aad,
    std::size_t aad_len,
    std::vector<unsigned char>& payload) {
    if (!hap_ready_ || !aad || payload.size() <= 16) return false;

    const std::size_t cipher_len = payload.size() - 16;
    unsigned char nonce[12]{};
    uint64_t counter = hap_counter_++;
    for (int i = 0; i < 8; ++i) {
        nonce[4 + i] = static_cast<unsigned char>((counter >> (8 * i)) & 0xff);
    }

    unsigned char tag[16]{};
    std::memcpy(tag, payload.data() + cipher_len, sizeof(tag));
    std::vector<unsigned char> plaintext(cipher_len);

    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (!context) return false;
    int written = 0;
    int total = 0;
    bool ok =
        EVP_DecryptInit_ex(
            context, EVP_chacha20_poly1305(), nullptr,
            hap_key_.data(), nonce) == 1 &&
        EVP_DecryptUpdate(
            context, nullptr, &written, aad,
            static_cast<int>(aad_len)) == 1 &&
        EVP_DecryptUpdate(
            context, plaintext.data(), &written,
            payload.data(), static_cast<int>(cipher_len)) == 1;
    if (ok) {
        total = written;
        ok = EVP_CIPHER_CTX_ctrl(
                 context, EVP_CTRL_AEAD_SET_TAG, sizeof(tag), tag) == 1 &&
             EVP_DecryptFinal_ex(
                 context, plaintext.data() + total, &written) == 1;
        total += written;
    }
    EVP_CIPHER_CTX_free(context);
    if (!ok) {
        LOG_WARN << "MirrorDecrypt: HAP video frame authentication failed";
        return false;
    }
    plaintext.resize(static_cast<std::size_t>(total));
    payload = std::move(plaintext);
    return true;
}

} // namespace ap::crypto
