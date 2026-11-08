#include "audio/ap2_packet_crypto.h"

#include <openssl/evp.h>

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

} // namespace

int main() {
    const std::vector<unsigned char> key(32, 0x5A);
    const std::vector<unsigned char> plaintext = {
        0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
        0x98, 0xA9, 0xBA, 0xCB, 0xDC, 0xED, 0xFE, 0x0F,
    };
    unsigned char wire_nonce[8]{};
    for (int i = 0; i < 8; ++i) {
        wire_nonce[i] = static_cast<unsigned char>(0xA0 + i);
    }

    std::vector<unsigned char> packet(12, 0);
    packet[0] = 0x80;
    packet[1] = 0x60;
    packet[4] = 0x01;
    packet[5] = 0x02;
    packet[6] = 0x03;
    packet[7] = 0x04;
    packet[8] = 0x16;

    unsigned char nonce[12]{};
    std::memcpy(nonce + 4, wire_nonce, sizeof(wire_nonce));
    std::vector<unsigned char> ciphertext(plaintext.size());
    unsigned char tag[16]{};
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    require(ctx != nullptr, "create EVP context");
    int written = 0;
    int total = 0;
    require(EVP_EncryptInit_ex(
                ctx, EVP_chacha20_poly1305(), nullptr,
                key.data(), nonce) == 1,
            "init ChaCha20-Poly1305");
    require(EVP_EncryptUpdate(
                ctx, nullptr, &written, packet.data() + 4, 8) == 1,
            "authenticate RTP header");
    require(EVP_EncryptUpdate(
                ctx, ciphertext.data(), &written,
                plaintext.data(), static_cast<int>(plaintext.size())) == 1,
            "encrypt payload");
    total = written;
    require(EVP_EncryptFinal_ex(
                ctx, ciphertext.data() + total, &written) == 1,
            "finish encrypted payload");
    total += written;
    ciphertext.resize(static_cast<std::size_t>(total));
    require(EVP_CIPHER_CTX_ctrl(
                ctx, EVP_CTRL_AEAD_GET_TAG, 16, tag) == 1,
            "read authentication tag");
    EVP_CIPHER_CTX_free(ctx);

    packet.insert(packet.end(), ciphertext.begin(), ciphertext.end());
    packet.insert(packet.end(), tag, tag + sizeof(tag));
    packet.insert(packet.end(), wire_nonce, wire_nonce + sizeof(wire_nonce));

    std::vector<unsigned char> decrypted;
    require(ap::audio::decrypt_buffered_packet(
                key, packet, decrypted),
            "decrypt AP2 buffered packet");
    require(decrypted == plaintext, "decrypted payload integrity");

    packet[packet.size() - 9] ^= 0x01;
    require(!ap::audio::decrypt_buffered_packet(
                key, packet, decrypted),
            "reject modified authentication tag");

    std::cout << "PASS: AP2 buffered packet nonce, AAD, tag and payload\n";
    return 0;
}
