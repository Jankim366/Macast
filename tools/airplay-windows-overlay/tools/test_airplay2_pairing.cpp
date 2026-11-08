#include "sodium.h"
#include "pair.h"
#include "pair-tlv.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main() {
    const char* device_id = "AA:BB:CC:DD:EE:FF";
    pair_setup_context* server =
        pair_setup_new(PAIR_SERVER_HOMEKIT, nullptr, nullptr, nullptr, device_id);
    pair_setup_context* client =
        pair_setup_new(
            PAIR_CLIENT_HOMEKIT_TRANSIENT, nullptr, nullptr, nullptr, device_id);
    require(server != nullptr, "server pairing context");
    require(client != nullptr, "client pairing context");

    std::size_t m1_len = 0;
    uint8_t* m1 = pair_setup_request1(&m1_len, client);
    uint8_t* m2 = nullptr;
    std::size_t m2_len = 0;
    require(pair_setup_response1(server, m1, m1_len) == 0,
            "parse transient M1");
    m2 = pair_setup_request1(&m2_len, server);
    require(m2 != nullptr && m2_len > 64,
            "server M2 contains salt and SRP public key");
    require(pair_setup_response1(client, m2, m2_len) == 0,
            "client parses M2");

    std::size_t m3_len = 0;
    uint8_t* m3 = pair_setup_request2(&m3_len, client);
    require(m3 != nullptr && m3_len > 64, "client M3");
    require(pair_setup_response2(server, m3, m3_len) == 0,
            "server verifies M3 proof");

    std::size_t m4_len = 0;
    uint8_t* m4 = pair_setup_request2(&m4_len, server);
    require(m4 != nullptr && m4_len > 0, "server M4");
    require(pair_setup_response2(client, m4, m4_len) == 0,
            "client parses M4");

    pair_result* server_result = nullptr;
    pair_result* client_result = nullptr;
    require(pair_setup_result(nullptr, &server_result, server) == 0,
            "server transient result");
    require(pair_setup_result(nullptr, &client_result, client) == 0,
            "client transient result");
    require(server_result->shared_secret_len == 64,
            "server shared secret length");
    require(client_result->shared_secret_len ==
                server_result->shared_secret_len,
            "matching shared secret length");
    require(std::memcmp(
                server_result->shared_secret,
                client_result->shared_secret,
                server_result->shared_secret_len) == 0,
            "matching shared secret value");

    pair_cipher_context* server_cipher = pair_cipher_new(
        PAIR_SERVER_HOMEKIT, 3,
        server_result->shared_secret, server_result->shared_secret_len, "");
    pair_cipher_context* client_cipher = pair_cipher_new(
        PAIR_SERVER_HOMEKIT, 0,
        client_result->shared_secret, client_result->shared_secret_len, "");
    require(server_cipher != nullptr && client_cipher != nullptr,
            "encrypted control channels");

    const uint8_t request[] = "GET /info RTSP/1.0\r\nCSeq: 1\r\n\r\n";
    uint8_t* encrypted = nullptr;
    std::size_t encrypted_len = 0;
    require(pair_encrypt(
                &encrypted, &encrypted_len,
                request, sizeof(request) - 1, client_cipher) >= 0,
            "client encrypts control request");
    uint8_t* decrypted = nullptr;
    std::size_t decrypted_len = 0;
    require(pair_decrypt(
                &decrypted, &decrypted_len,
                encrypted, encrypted_len, server_cipher) >= 0,
            "server decrypts control request");
    require(decrypted_len == sizeof(request) - 1 &&
                std::memcmp(decrypted, request, decrypted_len) == 0,
            "control request round trip");

    std::free(encrypted);
    std::free(decrypted);
    pair_cipher_free(client_cipher);
    pair_cipher_free(server_cipher);
    std::free(m1);
    std::free(m2);
    std::free(m3);
    std::free(m4);
    pair_setup_free(client);
    pair_setup_free(server);
    std::cout << "PASS: HomeKit transient pair-setup M1-M4\n";
    return 0;
}
