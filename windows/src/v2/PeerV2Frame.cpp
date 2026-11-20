#include "PeerV2Frame.hpp"

namespace Propods::v2 {

namespace {

constexpr std::uint8_t kMagic[4] = {'E', 'P', 'H', '2'};
constexpr std::uint8_t kVersion = 2;

FrameType checkedType(std::uint8_t id) {
    if (id < 1 || id > 11) throw std::invalid_argument("unknown_type");
    return static_cast<FrameType>(id);
}

// Returns the full frame size once the header is validated.
std::size_t parseHeader(const std::uint8_t* h) {
    for (int i = 0; i < 4; ++i)
        if (h[i] != kMagic[i]) throw std::invalid_argument("invalid_magic");
    if (h[4] != kVersion) throw std::invalid_argument("unsupported_version");
    checkedType(h[5]);
    const std::uint32_t size = (std::uint32_t{h[6]} << 24) | (std::uint32_t{h[7]} << 16) |
                               (std::uint32_t{h[8]} << 8) | std::uint32_t{h[9]};
    if (size > kMaxPayload) throw std::invalid_argument("oversized_payload");
    return kHeaderSize + size;
}

}  // namespace

const char* frameTypeName(FrameType t) {
    switch (t) {
        case FrameType::Hello: return "HELLO";
        case FrameType::Snapshot: return "SNAPSHOT";
        case FrameType::Ping: return "PING";
        case FrameType::Pong: return "PONG";
        case FrameType::AcquireRequest: return "ACQUIRE_REQUEST";
        case FrameType::AcquireGrant: return "ACQUIRE_GRANT";
        case FrameType::AcquireDeny: return "ACQUIRE_DENY";
        case FrameType::AcquireCancel: return "ACQUIRE_CANCEL";
        case FrameType::RelayData: return "RELAY_DATA";
        case FrameType::AcquireStart: return "ACQUIRE_START";
        case FrameType::AcquireStartAck: return "ACQUIRE_START_ACK";
    }
    return "?";
}

std::vector<std::uint8_t> encodeFrame(const Frame& frame) {
    if (frame.payload.size() > kMaxPayload) throw std::invalid_argument("oversized_payload");
    const auto size = static_cast<std::uint32_t>(frame.payload.size());
    std::vector<std::uint8_t> out{kMagic[0], kMagic[1], kMagic[2], kMagic[3], kVersion,
        static_cast<std::uint8_t>(frame.type),
        static_cast<std::uint8_t>(size >> 24), static_cast<std::uint8_t>(size >> 16),
        static_cast<std::uint8_t>(size >> 8), static_cast<std::uint8_t>(size)};
    out.insert(out.end(), frame.payload.begin(), frame.payload.end());
    return out;
}

std::vector<Frame> FrameDecoder::feed(std::span<const std::uint8_t> bytes) {
    m_pending.insert(m_pending.end(), bytes.begin(), bytes.end());
    std::vector<Frame> out;
    while (m_pending.size() >= kHeaderSize) {
        const std::size_t size = parseHeader(m_pending.data());
        if (m_pending.size() < size) break;
        Frame f{static_cast<FrameType>(m_pending[5]),
                std::string(reinterpret_cast<const char*>(m_pending.data() + kHeaderSize), size - kHeaderSize)};
        out.push_back(std::move(f));
        m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(size));
    }
    return out;
}

}  // namespace Propods::v2
