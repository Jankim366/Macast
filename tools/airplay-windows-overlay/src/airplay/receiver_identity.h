#pragma once

#include <string>
#include <vector>

namespace ap::airplay {

struct ReceiverIdentity {
    std::string device_id;
    std::string public_id;
};

// Derives stable, per-install AirPlay identity values from the persistent
// Ed25519 public key. The device ID is a locally administered unicast MAC and
// the public ID is a UUID-shaped value. Using cryptographic identity material
// avoids collisions when multiple PCs use the same network adapter, VPN or
// receiver name.
ReceiverIdentity derive_receiver_identity(
    const std::vector<unsigned char>& public_key);

} // namespace ap::airplay
