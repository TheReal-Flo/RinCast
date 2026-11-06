#include "rc/crypto.h"

#include <cstring>
#include <vector>

#include "monocypher.h"
#include "rc/platform.h"

namespace rc::crypto {

KeyPair KeyPair::Generate() {
    KeyPair kp;
    RandomBytes(kp.secretKey.data(), kp.secretKey.size());
    crypto_x25519_public_key(kp.publicKey.data(), kp.secretKey.data());
    return kp;
}

IdentitySeed NewIdentitySeed() {
    IdentitySeed seed;
    RandomBytes(seed.data(), seed.size());
    return seed;
}

Key IdentityPublicKey(const IdentitySeed& seed) {
    uint8_t secret[64];
    Key publicKey;
    IdentitySeed copy = seed;  // the key pair function wipes its input
    crypto_eddsa_key_pair(secret, publicKey.data(), copy.data());
    crypto_wipe(secret, sizeof(secret));
    return publicKey;
}

static std::vector<uint8_t> ConnectionMessage(const Key& connectionKey, const std::string& nonce) {
    static const char kLabel[] = "rincast-device-v1";
    std::vector<uint8_t> m(kLabel, kLabel + sizeof(kLabel));
    m.insert(m.end(), connectionKey.begin(), connectionKey.end());
    m.insert(m.end(), nonce.begin(), nonce.end());
    return m;
}

Signature SignConnection(const IdentitySeed& seed, const Key& connectionKey, const std::string& nonce) {
    uint8_t secret[64];
    Key publicKey;
    IdentitySeed copy = seed;
    crypto_eddsa_key_pair(secret, publicKey.data(), copy.data());
    const auto m = ConnectionMessage(connectionKey, nonce);
    Signature s;
    crypto_eddsa_sign(s.data(), secret, m.data(), m.size());
    crypto_wipe(secret, sizeof(secret));
    return s;
}

bool VerifyConnection(const Key& identityKey, const Key& connectionKey, const std::string& nonce, const Signature& signature) {
    const auto m = ConnectionMessage(connectionKey, nonce);
    return crypto_eddsa_check(signature.data(), identityKey.data(), m.data(), m.size()) == 0;
}

SessionKeys DeriveSessionKeys(const Key& mySecret, const Key& peerPublic, const Key& clientPublic,
                              const Key& serverPublic, const std::string& context) {
    uint8_t shared[32];
    crypto_x25519(shared, mySecret.data(), peerPublic.data());

    std::vector<uint8_t> transcript;
    static const char kLabel[] = "rincast-session-v3";
    transcript.insert(transcript.end(), kLabel, kLabel + sizeof(kLabel) - 1);
    transcript.insert(transcript.end(), clientPublic.begin(), clientPublic.end());
    transcript.insert(transcript.end(), serverPublic.begin(), serverPublic.end());
    transcript.insert(transcript.end(), context.begin(), context.end());

    uint8_t okm[64];
    crypto_blake2b_keyed(okm, sizeof(okm), shared, sizeof(shared), transcript.data(), transcript.size());

    SessionKeys keys;
    std::memcpy(keys.clientToServer.data(), okm, 32);
    std::memcpy(keys.serverToClient.data(), okm + 32, 32);
    crypto_wipe(shared, sizeof(shared));
    crypto_wipe(okm, sizeof(okm));
    return keys;
}

static void MakeNonce(uint64_t counter, uint8_t direction, uint8_t nonce[24]) {
    std::memset(nonce, 0, 24);
    nonce[0] = direction;
    for (int i = 0; i < 8; i++) nonce[16 + i] = uint8_t(counter >> (8 * i));
}

void Seal(const Key& key, uint64_t counter, uint8_t direction, const uint8_t* ad, size_t adSize, const uint8_t* plain,
          size_t size, uint8_t* out) {
    uint8_t nonce[24];
    MakeNonce(counter, direction, nonce);
    crypto_aead_lock(out, out + size, key.data(), nonce, ad, adSize, plain, size);
}

bool Open(const Key& key, uint64_t counter, uint8_t direction, const uint8_t* ad, size_t adSize, const uint8_t* cipher,
          size_t size, uint8_t* out) {
    if (size < 16) return false;
    uint8_t nonce[24];
    MakeNonce(counter, direction, nonce);
    const size_t textSize = size - 16;
    return crypto_aead_unlock(out, cipher + textSize, key.data(), nonce, ad, adSize, cipher, textSize) == 0;
}

Key Mac(const std::string& key, const void* data, size_t size) {
    Key out{};
    // BLAKE2b keys are limited to 64 bytes; hash longer keys first.
    uint8_t k[64];
    size_t kSize = key.size();
    if (kSize > 64) {
        crypto_blake2b(k, 64, reinterpret_cast<const uint8_t*>(key.data()), key.size());
        kSize = 64;
    } else {
        std::memcpy(k, key.data(), kSize);
    }
    crypto_blake2b_keyed(out.data(), out.size(), k, kSize, static_cast<const uint8_t*>(data), size);
    crypto_wipe(k, sizeof(k));
    return out;
}

std::string MacHex(const std::string& key, const std::string& message) {
    const Key mac = Mac(key, message.data(), message.size());
    return HexEncode(mac.data(), mac.size());
}

void Wipe(void* p, size_t size) { crypto_wipe(p, size); }

}  // namespace rc::crypto
