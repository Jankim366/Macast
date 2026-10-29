#include "airplay/receiver_identity.h"

#include <openssl/evp.h>

#include <array>
#include <cctype>
#include <cstdio>

namespace ap::airplay {
namespace {

using Digest = std::array<unsigned char, 32>;

Digest digest_identity(
        const std::vector<unsigned char>& public_key,
        const char* domain) {
    Digest output{};
    unsigned int output_size = 0;
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (!context) return output;
    const bool ok =
        EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1 &&
        EVP_DigestUpdate(
            context, domain, std::char_traits<char>::length(domain)) == 1 &&
        EVP_DigestUpdate(
            context, public_key.data(), public_key.size()) == 1 &&
        EVP_DigestFinal_ex(
            context, output.data(), &output_size) == 1 &&
        output_size == output.size();
    EVP_MD_CTX_free(context);
    if (!ok) output.fill(0);
    return output;
}

std::string format_device_id(const Digest& digest) {
    unsigned char first = static_cast<unsigned char>(
        (digest[0] | 0x02U) & 0xFEU);
    char result[18]{};
    std::snprintf(
        result, sizeof(result),
        "%02X:%02X:%02X:%02X:%02X:%02X",
        first, digest[1], digest[2], digest[3], digest[4], digest[5]);
    return result;
}

std::string normalize_device_id(const std::string& value) {
    std::array<unsigned char, 6> bytes{};
    std::size_t count = 0;
    int high = -1;
    for (unsigned char c : value) {
        if (c == ':' || c == '-' || c == '.') continue;
        if (!std::isxdigit(c) || count >= bytes.size()) return {};
        const int digit = std::isdigit(c)
            ? c - '0'
            : std::tolower(c) - 'a' + 10;
        if (high < 0) {
            high = digit;
        } else {
            bytes[count++] =
                static_cast<unsigned char>((high << 4) | digit);
            high = -1;
        }
    }
    if (count != bytes.size() || high >= 0) return {};
    char result[18]{};
    std::snprintf(
        result, sizeof(result),
        "%02X:%02X:%02X:%02X:%02X:%02X",
        bytes[0], bytes[1], bytes[2],
        bytes[3], bytes[4], bytes[5]);
    return result;
}

std::string format_public_id(Digest digest) {
    digest[6] = static_cast<unsigned char>(
        (digest[6] & 0x0FU) | 0x50U);
    digest[8] = static_cast<unsigned char>(
        (digest[8] & 0x3FU) | 0x80U);
    char result[37]{};
    std::snprintf(
        result, sizeof(result),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-"
        "%02x%02x-%02x%02x%02x%02x%02x%02x",
        digest[0], digest[1], digest[2], digest[3],
        digest[4], digest[5], digest[6], digest[7],
        digest[8], digest[9], digest[10], digest[11],
        digest[12], digest[13], digest[14], digest[15]);
    return result;
}

} // namespace

ReceiverIdentity derive_receiver_identity(
        const std::vector<unsigned char>& public_key,
        const std::string& preferred_device_id) {
    const std::string normalized =
        normalize_device_id(preferred_device_id);
    const Digest device_digest = digest_identity(
        public_key, "Macast AirPlay deviceid v1");
    const std::string device_id = normalized.empty()
        ? format_device_id(device_digest)
        : normalized;

    EVP_MD_CTX* context = EVP_MD_CTX_new();
    Digest public_digest{};
    unsigned int public_size = 0;
    if (!context) return {};
    const bool ok =
        EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1 &&
        EVP_DigestUpdate(
            context, "Macast AirPlay public-id v2:",
            std::char_traits<char>::length(
                "Macast AirPlay public-id v2:")) == 1 &&
        EVP_DigestUpdate(
            context, public_key.data(), public_key.size()) == 1 &&
        EVP_DigestUpdate(
            context, device_id.data(), device_id.size()) == 1 &&
        EVP_DigestFinal_ex(
            context, public_digest.data(), &public_size) == 1 &&
        public_size == public_digest.size();
    EVP_MD_CTX_free(context);
    if (!ok) return {};

    return {
        device_id,
        format_public_id(public_digest),
    };
}

} // namespace ap::airplay
