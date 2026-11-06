#include "rc/handshake.h"

#include <cstring>

#include "rc/platform.h"

namespace rc {

std::vector<uint8_t> BuildHandshake(const HandshakePacket& packet) {
    PacketHeader header{};
    header.version = uint8_t(kProtocolVersion);
    header.type = uint8_t(packet.type);
    header.flags = kFlagNone;
    header.sessionId = packet.sessionId;
    header.sequence = 0;

    const bool hasKey = packet.type != PacketType::Reject;
    std::vector<uint8_t> out(sizeof(header) + (hasKey ? 32 : 0) + packet.json.size());
    std::memcpy(out.data(), &header, sizeof(header));
    size_t offset = sizeof(header);
    if (hasKey) {
        std::memcpy(out.data() + offset, packet.publicKey.data(), 32);
        offset += 32;
    }
    std::memcpy(out.data() + offset, packet.json.data(), packet.json.size());
    if (out.size() > kMaxDatagram) RC_LOGW("Handshake packet exceeds MTU (%zu bytes)", out.size());
    return out;
}

bool IsHandshakeDatagram(const uint8_t* data, size_t size) {
    if (size < sizeof(PacketHeader)) return false;
    PacketHeader header;
    std::memcpy(&header, data, sizeof(header));
    if (header.flags & kFlagEncrypted) return false;
    const auto t = PacketType(header.type);
    return t == PacketType::Hello || t == PacketType::Welcome || t == PacketType::Reject || t == PacketType::Punch;
}

bool ParseHandshake(const uint8_t* data, size_t size, HandshakePacket& out) {
    if (size < sizeof(PacketHeader)) return false;
    PacketHeader header;
    std::memcpy(&header, data, sizeof(header));
    if (header.version != uint8_t(kProtocolVersion) || (header.flags & kFlagEncrypted)) return false;
    out.type = PacketType(header.type);
    out.sessionId = header.sessionId;
    size_t offset = sizeof(header);
    if (out.type == PacketType::Hello || out.type == PacketType::Welcome) {
        if (size < offset + 32) return false;
        std::memcpy(out.publicKey.data(), data + offset, 32);
        offset += 32;
    } else if (out.type != PacketType::Reject) {
        return false;
    }
    out.json.assign(reinterpret_cast<const char*>(data + offset), size - offset);
    return true;
}

std::string HelloProof(const std::string& connectSecret, const crypto::Key& clientPublic) {
    return crypto::MacHex(connectSecret, "hello|" + HexEncode(clientPublic.data(), clientPublic.size()));
}

std::string WelcomeProof(const std::string& connectSecret, const crypto::Key& serverPublic,
                         const crypto::Key& clientPublic) {
    return crypto::MacHex(connectSecret, "welcome|" + HexEncode(serverPublic.data(), serverPublic.size()) + "|" +
                                             HexEncode(clientPublic.data(), clientPublic.size()));
}

std::string SessionContext(const std::string& clientNonce, const std::string& serverNonce,
                           const std::string& connectSecret) {
    return clientNonce + "|" + serverNonce + "|" + connectSecret;
}

}  // namespace rc
