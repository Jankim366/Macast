#include "audio/ap2_packet_crypto.h"

#include <cstring>

#include <openssl/evp.h>

namespace ap::audio {

bool decrypt_buffered_packet(
    const std::vector<unsigned char>& stream_key,
    const std::vector<unsigned char>& packet,
    std::vector<unsigned char>& plaintext) {
    if (stream_key.size() != 32 || packet.size() < 12 + 16 + 8) {
        return false;
    }
    const std::size_t cipher_size = packet.size() - 12 - 16 - 8;
    const unsigned char* tag = packet.data() + packet.size() - 24;
    const unsigned char* wire_nonce = packet.data() + packet.size() - 8;
    const unsigned char* aad = packet.data() + 4;

    // IETF ChaCha20-Poly1305 要求 12 字节 nonce。AP2 只传低 8 字节，
    // 必须左补 4 个零字节；直接传 8 字节指针会让 EVP 越界读取。
    unsigned char nonce[12]{};
    std::memcpy(nonce + 4, wire_nonce, 8);

    plaintext.resize(cipher_size);
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (!context) return false;
    int written = 0;
    int total = 0;
    bool ok =
        EVP_DecryptInit_ex(
            context, EVP_chacha20_poly1305(), nullptr,
            stream_key.data(), nonce) == 1 &&
        EVP_DecryptUpdate(context, nullptr, &written, aad, 8) == 1 &&
        EVP_DecryptUpdate(
            context, plaintext.data(), &written,
            packet.data() + 12, static_cast<int>(cipher_size)) == 1;
    if (ok) {
        total = written;
        ok = EVP_CIPHER_CTX_ctrl(
                 context, EVP_CTRL_AEAD_SET_TAG, 16,
                 const_cast<unsigned char*>(tag)) == 1 &&
             EVP_DecryptFinal_ex(
                 context, plaintext.data() + total, &written) == 1;
        total += written;
    }
    EVP_CIPHER_CTX_free(context);
    if (!ok) return false;
    plaintext.resize(static_cast<std::size_t>(total));
    return true;
}

} // namespace ap::audio
