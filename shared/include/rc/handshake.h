// Plaintext handshake packets (Hello / Welcome / Reject) that bootstrap an
// encrypted Connection.
//
//   client -> server  Hello   [header][32B client X25519 pk][JSON]
//   server -> client  Welcome [header][32B server X25519 pk][JSON]   (sessionId assigned)
//   server -> client  Reject  [header][JSON {reason}]
//
// Local sessions are trust-on-first-use, gated by an "allow this headset?"
// prompt on the PC. Cloud sessions mix a per-session secret issued by the
// RinCast server into both the proofs and the key derivation, so a party
// without that secret cannot complete (or man-in-the-middle) the exchange.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rc/crypto.h"
#include "rc/protocol.h"

namespace rc {

struct HandshakePacket {
    PacketType type = PacketType::Hello;
    uint32_t sessionId = 0;
    crypto::Key publicKey{};
    std::string json;
};

std::vector<uint8_t> BuildHandshake(const HandshakePacket& packet);
bool ParseHandshake(const uint8_t* data, size_t size, HandshakePacket& out);

// Returns true if the datagram is a plaintext handshake / punch packet.
bool IsHandshakeDatagram(const uint8_t* data, size_t size);

// Proofs for cloud-authorized sessions.
std::string HelloProof(const std::string& connectSecret, const crypto::Key& clientPublic);
std::string WelcomeProof(const std::string& connectSecret, const crypto::Key& serverPublic,
                         const crypto::Key& clientPublic);

// Context string mixed into the KDF.
std::string SessionContext(const std::string& clientNonce, const std::string& serverNonce,
                           const std::string& connectSecret);

}  // namespace rc
