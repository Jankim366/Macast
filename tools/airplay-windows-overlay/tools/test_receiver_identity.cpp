#include "airplay/receiver_identity.h"

#include <cstdlib>
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

} // namespace

int main() {
    std::vector<unsigned char> first_key(32);
    std::vector<unsigned char> second_key(32);
    for (std::size_t i = 0; i < first_key.size(); ++i) {
        first_key[i] = static_cast<unsigned char>(i);
        second_key[i] = static_cast<unsigned char>(255 - i);
    }

    const auto first =
        ap::airplay::derive_receiver_identity(first_key);
    const auto repeated =
        ap::airplay::derive_receiver_identity(first_key);
    const auto second =
        ap::airplay::derive_receiver_identity(second_key);

    require(first.device_id == repeated.device_id,
            "device ID is stable for the same identity");
    require(first.public_id == repeated.public_id,
            "public ID is stable for the same identity");
    require(first.device_id != second.device_id,
            "different identities produce different device IDs");
    require(first.public_id != second.public_id,
            "different identities produce different public IDs");
    require(first.device_id.size() == 17,
            "device ID has MAC address length");
    require(first.public_id.size() == 36,
            "public ID has UUID length");

    const int first_octet =
        std::stoi(first.device_id.substr(0, 2), nullptr, 16);
    require((first_octet & 0x02) != 0,
            "device ID uses the locally administered bit");
    require((first_octet & 0x01) == 0,
            "device ID is unicast");
    require(first.public_id[14] == '5',
            "public ID carries a UUID version");
    require(std::string("89ab").find(first.public_id[19]) !=
                std::string::npos,
            "public ID carries a UUID variant");

    std::cout << "PASS: stable unique receiver identity "
              << first.device_id << ' ' << first.public_id << '\n';
    return 0;
}
