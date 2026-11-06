// Thin wrappers over Monocypher: X25519 key agreement, XChaCha20-Poly1305
// AEAD and BLAKE2b key derivation.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace rc::crypto {

using Key = std::array<uint8_t, 32>;

struct KeyPair {
    Key secretKey{};
    Key publicKey{};
    static KeyPair Generate();
};

struct SessionKeys {
    Key clientToServer{};
    Key serverToClient{};
};

// A headset's long-lived identity. It signs each connection's temporary key, so the PC
// can tell it's the same headset without trusting what the headset or a server claims.
using IdentitySeed = std::array<uint8_t, 32>;
using Signature = std::array<uint8_t, 64>;
IdentitySeed NewIdentitySeed();
Key IdentityPublicKey(const IdentitySeed& seed);
Signature SignConnection(const IdentitySeed& seed, const Key& connectionKey, const std::string& nonce);
bool VerifyConnection(const Key& identityKey, const Key& connectionKey, const std::string& nonce, const Signature& signature);

// Derives directional keys from an X25519 exchange. `context` binds extra
// data (e.g. both nonces and an optional pre-shared secret from the cloud).
SessionKeys DeriveSessionKeys(const Key& mySecret, const Key& peerPublic, const Key& clientPublic,
                              const Key& serverPublic, const std::string& context);

// Seals `plain` into `out` (must have room for size + 16 bytes MAC appended).
void Seal(const Key& key, uint64_t counter, uint8_t direction, const uint8_t* ad, size_t adSize, const uint8_t* plain,
          size_t size, uint8_t* out);

// Opens `cipher` (size includes trailing 16-byte MAC) into `out`. Returns false if tampered.
bool Open(const Key& key, uint64_t counter, uint8_t direction, const uint8_t* ad, size_t adSize, const uint8_t* cipher,
          size_t size, uint8_t* out);

// Keyed BLAKE2b-256, used as a MAC for cloud-issued session proofs.
Key Mac(const std::string& key, const void* data, size_t size);
std::string MacHex(const std::string& key, const std::string& message);

void Wipe(void* p, size_t size);

}  // namespace rc::crypto
