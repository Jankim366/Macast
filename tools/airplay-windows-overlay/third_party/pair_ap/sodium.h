#pragma once

/*
 * pair_ap 所需的最小 libsodium 兼容接口。
 *
 * Shairport Sync 的 pair_ap 已使用 OpenSSL 实现 HKDF 和
 * ChaCha20-Poly1305，仅公钥操作依赖 libsodium。此处复用 OpenSSL，避免为了
 * 少数几个原语给 Windows 后端额外引入运行时依赖。
 */

#include <stddef.h>

#if defined(_MSC_VER)
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#ifndef __attribute__
#define __attribute__(value)
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define crypto_sign_PUBLICKEYBYTES 32
#define crypto_sign_SECRETKEYBYTES 64
#define crypto_sign_SEEDBYTES 32
#define crypto_sign_BYTES 64
#define crypto_box_PUBLICKEYBYTES 32
#define crypto_box_SECRETKEYBYTES 32
#define crypto_scalarmult_BYTES 32

int sodium_init(void);

int crypto_sign_seed_keypair(
    unsigned char* public_key,
    unsigned char* secret_key,
    const unsigned char* seed);
int crypto_sign_keypair(
    unsigned char* public_key,
    unsigned char* secret_key);
int crypto_sign_ed25519_sk_to_pk(
    unsigned char* public_key,
    const unsigned char* secret_key);
int crypto_sign_detached(
    unsigned char* signature,
    unsigned long long* signature_len,
    const unsigned char* message,
    unsigned long long message_len,
    const unsigned char* secret_key);
int crypto_sign_verify_detached(
    const unsigned char* signature,
    const unsigned char* message,
    unsigned long long message_len,
    const unsigned char* public_key);

int crypto_box_keypair(
    unsigned char* public_key,
    unsigned char* secret_key);
int crypto_scalarmult(
    unsigned char* shared,
    const unsigned char* secret_key,
    const unsigned char* public_key);

#ifdef __cplusplus
}
#endif
