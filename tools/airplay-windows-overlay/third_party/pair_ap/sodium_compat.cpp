#include "sodium.h"

#include <cstring>

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace {

EVP_PKEY* ed25519_from_seed(const unsigned char* seed) {
    return EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr, seed, crypto_sign_SEEDBYTES);
}

EVP_PKEY* ed25519_public(const unsigned char* public_key) {
    return EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr, public_key,
        crypto_sign_PUBLICKEYBYTES);
}

EVP_PKEY* x25519_private(const unsigned char* secret_key) {
    return EVP_PKEY_new_raw_private_key(
        EVP_PKEY_X25519, nullptr, secret_key,
        crypto_box_SECRETKEYBYTES);
}

EVP_PKEY* x25519_public(const unsigned char* public_key) {
    return EVP_PKEY_new_raw_public_key(
        EVP_PKEY_X25519, nullptr, public_key,
        crypto_box_PUBLICKEYBYTES);
}

int export_ed25519_public(
    EVP_PKEY* key,
    unsigned char* public_key) {
    std::size_t length = crypto_sign_PUBLICKEYBYTES;
    return EVP_PKEY_get_raw_public_key(
        key, public_key, &length) == 1 &&
        length == crypto_sign_PUBLICKEYBYTES;
}

} // namespace

extern "C" {

int sodium_init(void) {
    return 0;
}

int crypto_sign_seed_keypair(
    unsigned char* public_key,
    unsigned char* secret_key,
    const unsigned char* seed) {
    EVP_PKEY* key = ed25519_from_seed(seed);
    if (!key) return -1;
    const int result =
        export_ed25519_public(key, public_key) ? 0 : -1;
    EVP_PKEY_free(key);
    if (result != 0) return result;

    // libsodium 的 Ed25519 secret key 格式为 seed || public key。
    std::memcpy(secret_key, seed, crypto_sign_SEEDBYTES);
    std::memcpy(secret_key + crypto_sign_SEEDBYTES,
                public_key, crypto_sign_PUBLICKEYBYTES);
    return 0;
}

int crypto_sign_keypair(
    unsigned char* public_key,
    unsigned char* secret_key) {
    unsigned char seed[crypto_sign_SEEDBYTES]{};
    if (RAND_bytes(seed, sizeof(seed)) != 1) return -1;
    return crypto_sign_seed_keypair(public_key, secret_key, seed);
}

int crypto_sign_ed25519_sk_to_pk(
    unsigned char* public_key,
    const unsigned char* secret_key) {
    std::memcpy(public_key, secret_key + crypto_sign_SEEDBYTES,
                crypto_sign_PUBLICKEYBYTES);
    return 0;
}

int crypto_sign_detached(
    unsigned char* signature,
    unsigned long long* signature_len,
    const unsigned char* message,
    unsigned long long message_len,
    const unsigned char* secret_key) {
    EVP_PKEY* key = ed25519_from_seed(secret_key);
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (!key || !context) {
        EVP_MD_CTX_free(context);
        EVP_PKEY_free(key);
        return -1;
    }
    std::size_t length = crypto_sign_BYTES;
    const bool ok =
        EVP_DigestSignInit(context, nullptr, nullptr, nullptr, key) == 1 &&
        EVP_DigestSign(context, signature, &length,
                       message, message_len) == 1 &&
        length == crypto_sign_BYTES;
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    if (!ok) return -1;
    if (signature_len) *signature_len = length;
    return 0;
}

int crypto_sign_verify_detached(
    const unsigned char* signature,
    const unsigned char* message,
    unsigned long long message_len,
    const unsigned char* public_key) {
    EVP_PKEY* key = ed25519_public(public_key);
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (!key || !context) {
        EVP_MD_CTX_free(context);
        EVP_PKEY_free(key);
        return -1;
    }
    const bool ok =
        EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, key) == 1 &&
        EVP_DigestVerify(context, signature, crypto_sign_BYTES,
                         message, message_len) == 1;
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    return ok ? 0 : -1;
}

int crypto_box_keypair(
    unsigned char* public_key,
    unsigned char* secret_key) {
    EVP_PKEY_CTX* context = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    EVP_PKEY* key = nullptr;
    if (!context || EVP_PKEY_keygen_init(context) != 1 ||
        EVP_PKEY_keygen(context, &key) != 1) {
        EVP_PKEY_free(key);
        EVP_PKEY_CTX_free(context);
        return -1;
    }
    std::size_t public_len = crypto_box_PUBLICKEYBYTES;
    std::size_t secret_len = crypto_box_SECRETKEYBYTES;
    const bool ok =
        EVP_PKEY_get_raw_public_key(
            key, public_key, &public_len) == 1 &&
        EVP_PKEY_get_raw_private_key(
            key, secret_key, &secret_len) == 1 &&
        public_len == crypto_box_PUBLICKEYBYTES &&
        secret_len == crypto_box_SECRETKEYBYTES;
    EVP_PKEY_free(key);
    EVP_PKEY_CTX_free(context);
    return ok ? 0 : -1;
}

int crypto_scalarmult(
    unsigned char* shared,
    const unsigned char* secret_key,
    const unsigned char* public_key) {
    EVP_PKEY* private_key = x25519_private(secret_key);
    EVP_PKEY* peer_key = x25519_public(public_key);
    EVP_PKEY_CTX* context =
        private_key ? EVP_PKEY_CTX_new(private_key, nullptr) : nullptr;
    std::size_t length = crypto_scalarmult_BYTES;
    const bool ok =
        context &&
        EVP_PKEY_derive_init(context) == 1 &&
        EVP_PKEY_derive_set_peer(context, peer_key) == 1 &&
        EVP_PKEY_derive(context, shared, &length) == 1 &&
        length == crypto_scalarmult_BYTES;
    EVP_PKEY_CTX_free(context);
    EVP_PKEY_free(peer_key);
    EVP_PKEY_free(private_key);
    return ok ? 0 : -1;
}

} // extern "C"
