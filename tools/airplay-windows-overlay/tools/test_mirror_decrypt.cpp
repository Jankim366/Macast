#include "crypto/mirror_decrypt.h"

#include <openssl/evp.h>
#include <openssl/kdf.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::array<unsigned char, 32> derive_key(
    const std::vector<unsigned char>& secret,
    uint64_t stream_id) {
    const std::string salt =
        "DataStream-Salt" + std::to_string(stream_id);
    const std::string info = "DataStream-Output-Encryption-Key";
    std::array<unsigned char, 32> key{};
    std::size_t key_len = key.size();

    EVP_PKEY_CTX* context = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    require(context != nullptr, "create HKDF context");
    require(EVP_PKEY_derive_init(context) == 1, "init HKDF");
    require(EVP_PKEY_CTX_set_hkdf_md(context, EVP_sha512()) == 1,
            "set HKDF digest");
    require(EVP_PKEY_CTX_set1_hkdf_salt(
                context,
                reinterpret_cast<const unsigned char*>(salt.data()),
                salt.size()) == 1,
            "set HKDF salt");
    require(EVP_PKEY_CTX_set1_hkdf_key(
                context, secret.data(), secret.size()) == 1,
            "set HKDF key");
    require(EVP_PKEY_CTX_add1_hkdf_info(
                context,
                reinterpret_cast<const unsigned char*>(info.data()),
                info.size()) == 1,
            "set HKDF info");
    require(EVP_PKEY_derive(context, key.data(), &key_len) == 1 &&
                key_len == key.size(),
            "derive HKDF key");
    EVP_PKEY_CTX_free(context);
    return key;
}

std::vector<unsigned char> encrypt_frame(
    const std::array<unsigned char, 32>& key,
    uint64_t counter,
    const std::array<unsigned char, 128>& header,
    const std::vector<unsigned char>& plaintext) {
    unsigned char nonce[12]{};
    for (int i = 0; i < 8; ++i) {
        nonce[4 + i] = static_cast<unsigned char>(
            (counter >> (8 * i)) & 0xff);
    }

    std::vector<unsigned char> output(plaintext.size() + 16);
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    require(context != nullptr, "create ChaCha context");
    int written = 0;
    int total = 0;
    require(EVP_EncryptInit_ex(
                context, EVP_chacha20_poly1305(), nullptr,
                key.data(), nonce) == 1,
            "init ChaCha");
    require(EVP_EncryptUpdate(
                context, nullptr, &written, header.data(),
                static_cast<int>(header.size())) == 1,
            "encrypt frame AAD");
    require(EVP_EncryptUpdate(
                context, output.data(), &written,
                plaintext.data(), static_cast<int>(plaintext.size())) == 1,
            "encrypt frame payload");
    total = written;
    require(EVP_EncryptFinal_ex(
                context, output.data() + total, &written) == 1,
            "finish frame payload");
    total += written;
    require(EVP_CIPHER_CTX_ctrl(
                context, EVP_CTRL_AEAD_GET_TAG, 16,
                output.data() + total) == 1,
            "read frame tag");
    output.resize(static_cast<std::size_t>(total) + 16);
    EVP_CIPHER_CTX_free(context);
    return output;
}

} // namespace

int main() {
    const std::vector<unsigned char> session_key(64, 0x42);
    constexpr uint64_t stream_id = UINT64_C(0x0102030405060708);
    const auto key = derive_key(session_key, stream_id);
    std::array<unsigned char, 128> header{};
    for (std::size_t i = 0; i < header.size(); ++i) {
        header[i] = static_cast<unsigned char>(i);
    }
    const std::vector<unsigned char> first_plain = {
        0, 0, 0, 8, 0x02, 0x01, 0xD0, 0x09,
        0x10, 0x20, 0x30, 0x40,
    };
    const std::vector<unsigned char> second_plain = {
        0, 0, 0, 5, 0x26, 0x01, 0xAA, 0xBB, 0xCC,
    };

    ap::crypto::MirrorDecrypt decryptor;
    require(decryptor.init_hap(session_key, stream_id),
            "initialize HAP video decryptor");
    require(decryptor.is_hap(), "select HAP video mode");

    auto first = encrypt_frame(key, 0, header, first_plain);
    require(decryptor.decrypt_hap(header.data(), header.size(), first),
            "decrypt first HAP video frame");
    require(first == first_plain, "first frame plaintext");

    auto second = encrypt_frame(key, 1, header, second_plain);
    require(decryptor.decrypt_hap(header.data(), header.size(), second),
            "decrypt second HAP video frame");
    require(second == second_plain, "counter advances per frame");

    auto corrupted = encrypt_frame(key, 2, header, second_plain);
    corrupted.back() ^= 0x01;
    require(!decryptor.decrypt_hap(header.data(), header.size(), corrupted),
            "reject modified video authentication tag");

    std::cout << "PASS: HomeKit video HKDF, ChaCha20, AAD, tag and counter\n";
    return 0;
}
