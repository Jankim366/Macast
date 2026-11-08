#include "crypto/ap2_session.h"

#include "log.h"

#include "sodium.h"
#include "pair.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace ap::crypto {

Ap2Session::Ap2Session(std::string device_id)
    : device_id_(std::move(device_id)) {}

Ap2Session::~Ap2Session() {
    reset_pairing();
}

void Ap2Session::reset_pairing() {
    if (setup_) pair_setup_free(static_cast<struct pair_setup_context*>(setup_));
    setup_ = nullptr;
    if (verify_) pair_verify_free(static_cast<struct pair_verify_context*>(verify_));
    verify_ = nullptr;
    if (control_cipher_) {
        pair_cipher_free(static_cast<struct pair_cipher_context*>(control_cipher_));
        control_cipher_ = nullptr;
    }
    encryption_active_ = false;
    shared_secret_.clear();
    encrypted_input_.clear();
    decrypted_input_.clear();
}

bool Ap2Session::install_control_cipher(
    const unsigned char* secret, std::size_t size) {
    shared_secret_.assign(secret, secret + size);
    control_cipher_ = pair_cipher_new(
        PAIR_SERVER_HOMEKIT, 3, secret, size, "");
    if (!control_cipher_) {
        LOG_ERROR << "AP2: failed to create control-channel cipher";
        shared_secret_.clear();
        return false;
    }
    LOG_INFO << "AP2: HomeKit pairing established encrypted control channel";
    return true;
}

bool Ap2Session::process_pair_setup(
    const std::vector<unsigned char>& request,
    std::vector<unsigned char>& response) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!setup_) {
        setup_ = pair_setup_new(
            PAIR_SERVER_HOMEKIT, nullptr, nullptr, nullptr,
            device_id_.c_str());
        if (!setup_) {
            LOG_ERROR << "AP2: pair_setup_new failed";
            return false;
        }
    }

    uint8_t* output = nullptr;
    std::size_t output_size = 0;
    const int result = pair_setup(
        &output, &output_size,
        static_cast<struct pair_setup_context*>(setup_),
        request.data(), request.size());
    if (result < 0 || !output) {
        LOG_WARN << "AP2 pair-setup failed: "
                 << pair_setup_errmsg(
                        static_cast<struct pair_setup_context*>(setup_));
        std::free(output);
        return false;
    }
    response.assign(output, output + output_size);
    std::free(output);

    struct pair_result* pairing = nullptr;
    if (pair_setup_result(
            nullptr, &pairing,
            static_cast<struct pair_setup_context*>(setup_)) == 0 &&
        pairing && pairing->shared_secret_len > 0 &&
        !control_cipher_) {
        return install_control_cipher(
            pairing->shared_secret, pairing->shared_secret_len);
    }
    return true;
}

bool Ap2Session::process_pair_verify(
    const std::vector<unsigned char>& request,
    std::vector<unsigned char>& response) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!verify_) {
        verify_ = pair_verify_new(
            PAIR_SERVER_HOMEKIT, nullptr, nullptr, nullptr,
            device_id_.c_str());
        if (!verify_) {
            LOG_ERROR << "AP2: pair_verify_new failed";
            return false;
        }
    }

    uint8_t* output = nullptr;
    std::size_t output_size = 0;
    const int result = pair_verify(
        &output, &output_size,
        static_cast<struct pair_verify_context*>(verify_),
        request.data(), request.size());
    if (result < 0 || !output) {
        LOG_WARN << "AP2 pair-verify failed: "
                 << pair_verify_errmsg(
                        static_cast<struct pair_verify_context*>(verify_));
        std::free(output);
        return false;
    }
    response.assign(output, output + output_size);
    std::free(output);

    struct pair_result* pairing = nullptr;
    if (pair_verify_result(
            &pairing,
            static_cast<struct pair_verify_context*>(verify_)) == 0 &&
        pairing && !control_cipher_) {
        return install_control_cipher(
            pairing->shared_secret, pairing->shared_secret_len);
    }
    return true;
}

int Ap2Session::read_encrypted(
    socket_t socket, void* data, std::size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!encryption_ready() || size == 0) return -1;

    if (!decrypted_input_.empty()) {
        const std::size_t copied = std::min(size, decrypted_input_.size());
        std::memcpy(data, decrypted_input_.data(), copied);
        decrypted_input_.erase(
            decrypted_input_.begin(),
            decrypted_input_.begin() + copied);
        return static_cast<int>(copied);
    }

    unsigned char incoming[4096];
    while (decrypted_input_.empty()) {
        const int received = ::recv(
            socket, reinterpret_cast<char*>(incoming),
            sizeof(incoming), 0);
        if (received <= 0) return received;
        encrypted_input_.insert(
            encrypted_input_.end(), incoming, incoming + received);

        uint8_t* plain = nullptr;
        std::size_t plain_size = 0;
        const ssize_t consumed = pair_decrypt(
            &plain, &plain_size,
            encrypted_input_.data(), encrypted_input_.size(),
            static_cast<struct pair_cipher_context*>(control_cipher_));
        if (consumed < 0) {
            std::free(plain);
            LOG_WARN << "AP2: encrypted control frame authentication failed";
            return -1;
        }
        if (consumed > 0) {
            encrypted_input_.erase(
                encrypted_input_.begin(),
                encrypted_input_.begin() + consumed);
        }
        if (plain_size > 0 && plain) {
            decrypted_input_.assign(plain, plain + plain_size);
        }
        std::free(plain);
    }

    const std::size_t copied = std::min(size, decrypted_input_.size());
    std::memcpy(data, decrypted_input_.data(), copied);
    decrypted_input_.erase(
        decrypted_input_.begin(),
        decrypted_input_.begin() + copied);
    return static_cast<int>(copied);
}

bool Ap2Session::write_encrypted(
    socket_t socket, const void* data, std::size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!encryption_ready() || size == 0) return false;

    uint8_t* encrypted = nullptr;
    std::size_t encrypted_size = 0;
    const ssize_t result = pair_encrypt(
        &encrypted, &encrypted_size,
        static_cast<const uint8_t*>(data), size,
        static_cast<struct pair_cipher_context*>(control_cipher_));
    if (result < 0 || !encrypted) {
        std::free(encrypted);
        LOG_WARN << "AP2: encrypted control frame creation failed";
        return false;
    }
    const bool sent = ap::net::send_all(
        socket, reinterpret_cast<const char*>(encrypted),
        static_cast<int>(encrypted_size)) >= 0;
    std::free(encrypted);
    return sent;
}

std::vector<unsigned char> airplay2_public_key(
    const std::string& device_id) {
    std::vector<unsigned char> result(32);
    pair_public_key_get(
        PAIR_SERVER_HOMEKIT, result.data(), device_id.c_str());
    return result;
}

bool configure_ap2_pairing_seed(
    const std::vector<unsigned char>& seed) {
    if (seed.size() != 32) return false;
    pair_set_server_seed(seed.data());
    return true;
}

Ap2CipherChannel::Ap2CipherChannel(
    int channel,
    const std::vector<unsigned char>& shared_secret) {
    if (shared_secret.empty()) return;
    cipher_ = pair_cipher_new(
        PAIR_SERVER_HOMEKIT, channel,
        shared_secret.data(), shared_secret.size(), "");
}

Ap2CipherChannel::~Ap2CipherChannel() {
    if (cipher_) {
        pair_cipher_free(static_cast<struct pair_cipher_context*>(cipher_));
    }
}

int Ap2CipherChannel::read_encrypted(
    socket_t socket, void* data, std::size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!cipher_ || size == 0) return -1;
    if (!decrypted_input_.empty()) {
        const std::size_t copied = std::min(size, decrypted_input_.size());
        std::memcpy(data, decrypted_input_.data(), copied);
        decrypted_input_.erase(
            decrypted_input_.begin(),
            decrypted_input_.begin() + copied);
        return static_cast<int>(copied);
    }

    unsigned char incoming[4096];
    while (decrypted_input_.empty()) {
        const int received = ::recv(
            socket, reinterpret_cast<char*>(incoming),
            sizeof(incoming), 0);
        if (received <= 0) return received;
        encrypted_input_.insert(
            encrypted_input_.end(), incoming, incoming + received);
        uint8_t* plain = nullptr;
        std::size_t plain_size = 0;
        const ssize_t consumed = pair_decrypt(
            &plain, &plain_size,
            encrypted_input_.data(), encrypted_input_.size(),
            static_cast<struct pair_cipher_context*>(cipher_));
        if (consumed < 0) {
            std::free(plain);
            return -1;
        }
        if (consumed > 0) {
            encrypted_input_.erase(
                encrypted_input_.begin(),
                encrypted_input_.begin() + consumed);
        }
        if (plain_size > 0 && plain) {
            decrypted_input_.assign(plain, plain + plain_size);
        }
        std::free(plain);
    }

    const std::size_t copied = std::min(size, decrypted_input_.size());
    std::memcpy(data, decrypted_input_.data(), copied);
    decrypted_input_.erase(
        decrypted_input_.begin(),
        decrypted_input_.begin() + copied);
    return static_cast<int>(copied);
}

bool Ap2CipherChannel::write_encrypted(
    socket_t socket, const void* data, std::size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!cipher_ || size == 0) return false;
    uint8_t* encrypted = nullptr;
    std::size_t encrypted_size = 0;
    const ssize_t result = pair_encrypt(
        &encrypted, &encrypted_size,
        static_cast<const uint8_t*>(data), size,
        static_cast<struct pair_cipher_context*>(cipher_));
    if (result < 0 || !encrypted) {
        std::free(encrypted);
        return false;
    }
    const bool sent = ap::net::send_all(
        socket, reinterpret_cast<const char*>(encrypted),
        static_cast<int>(encrypted_size)) >= 0;
    std::free(encrypted);
    return sent;
}

} // namespace ap::crypto
