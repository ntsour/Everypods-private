#pragma once

// EPH2 framing for EveryPods peer protocol v2 (Android PeerV2Frame):
// "EPH2", version 2 (u8), type (u8), payload length (u32 big-endian, <= 4096),
// then the UTF-8 JSON payload. The decoder handles fragmented and coalesced
// reads and throws std::invalid_argument on bad magic/version/type/size;
// the caller closes that peer's link (no resynchronisation).

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace Propods::v2 {

enum class FrameType : std::uint8_t {
    Hello = 1, Snapshot = 2, Ping = 3, Pong = 4, AcquireRequest = 5, AcquireGrant = 6,
    AcquireDeny = 7, AcquireCancel = 8, RelayData = 9, AcquireStart = 10, AcquireStartAck = 11,
};

const char* frameTypeName(FrameType t);

struct Frame {
    FrameType type;
    std::string payload;  // UTF-8 JSON
};

inline constexpr std::size_t kHeaderSize = 10;
inline constexpr std::size_t kMaxPayload = 4096;

std::vector<std::uint8_t> encodeFrame(const Frame& frame);

class FrameDecoder {
public:
    std::vector<Frame> feed(std::span<const std::uint8_t> bytes);
    std::size_t buffered() const { return m_pending.size(); }

private:
    std::vector<std::uint8_t> m_pending;
};

}  // namespace Propods::v2
