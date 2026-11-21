#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace Propods::crossdevice {

inline constexpr std::string_view kServiceUuid =
    "1abbb9a4-10e4-4000-a75c-8953c5471342";

// "EveryPodsComputerPeer": the same 4-byte packets, between two computers
// (Windows <-> Windows, Windows <-> Mac). Every computer client listens on it;
// phones never do. Kept apart from kServiceUuid so a computer is never
// mistaken for a phone (phone discovery and the phone-only v2 protocol key
// on kServiceUuid). When both sides dial at once, the link dialled by the
// lower Bluetooth address is kept and the other is closed.
inline constexpr std::string_view kComputerServiceUuid =
    "fc163292-cca9-4217-a075-81566bca4af3";

using Packet4 = std::array<std::uint8_t, 4>;

inline constexpr Packet4 kAirPodsConnected      { 0x00, 0x01, 0x00, 0x01 };
inline constexpr Packet4 kAirPodsDisconnected   { 0x00, 0x01, 0x00, 0x00 };
inline constexpr Packet4 kRequestDisconnect     { 0x00, 0x02, 0x00, 0x00 };
inline constexpr Packet4 kRequestBatteryBytes   { 0x00, 0x02, 0x00, 0x01 };
inline constexpr Packet4 kRequestAncBytes       { 0x00, 0x02, 0x00, 0x02 };
inline constexpr Packet4 kRequestConnectionStat { 0x00, 0x02, 0x00, 0x03 };
inline constexpr Packet4 kRequestHandover       { 0x00, 0x02, 0x00, 0x04 };
inline constexpr Packet4 kAirPodsDataHeader     { 0x00, 0x04, 0x00, 0x01 };
// Windows → Android: report whether an audio session (call/meeting) is active locally.
// Android uses these to gate takeover on the user's "takeover during call" preference.
inline constexpr Packet4 kWindowsAudioActive    { 0x00, 0x03, 0x00, 0x01 };
inline constexpr Packet4 kWindowsAudioIdle      { 0x00, 0x03, 0x00, 0x00 };

enum class Incoming {
    Unknown,
    AirPodsConnected,
    AirPodsDisconnected,
    RequestDisconnect,   // peer wants AirPods for music; we may reject if in a call
    RequestHandover,     // peer wants AirPods for a call; we MUST release (call priority)
    RequestBatteryBytes,
    RequestAncBytes,
    RequestConnectionStatus,
    RelayHeader,
    WindowsAudioActive,
    WindowsAudioIdle,
};

inline bool equals4(std::span<const std::uint8_t> data, const Packet4& p) {
    return data.size() == 4 && std::memcmp(data.data(), p.data(), 4) == 0;
}

inline bool startsWith4(std::span<const std::uint8_t> data, const Packet4& p) {
    return data.size() >= 4 && std::memcmp(data.data(), p.data(), 4) == 0;
}

// Legacy RFCOMM is a byte stream, not a sequence of read-sized packets: one
// read can hold two packets, or half of one. Mirrors Android's
// LegacyPeerStreamDecoder so both ends split the stream the same way:
//  - known 4-byte commands are 4 bytes;
//  - kAirPodsDataHeader + an AACP packet, and a bare AACP packet
//    (04 00 04 00 <type> ...), use the AACP length for that type
//    (battery 22, ear detection 8, control/ANC 11);
//  - anything else has no length field, so everything buffered is passed on
//    as one opaque packet (e.g. the phone's raw battery bytes 00 05 00 01 ..),
//    never searched for command bytes.
// One instance per connection.
class StreamDecoder {
public:
    std::vector<std::vector<std::uint8_t>> feed(std::span<const std::uint8_t> bytes) {
        m_pending.insert(m_pending.end(), bytes.begin(), bytes.end());
        if (m_pending.size() > 4096) m_pending.clear();  // garbage; resync on next read
        std::vector<std::vector<std::uint8_t>> out;
        while (m_pending.size() >= 4) {
            long length = -1;   // -1: opaque, take everything
            bool needMore = false;
            if (isControl()) {
                length = 4;
            } else if (std::memcmp(m_pending.data(), kAirPodsDataHeader.data(), 4) == 0) {
                const long aacp = aacpLength(4, needMore);
                if (!needMore && aacp >= 0) length = aacp + 4;
            } else if (std::memcmp(m_pending.data(), kAacp.data(), 4) == 0) {
                length = aacpLength(0, needMore);
            }
            if (needMore) break;
            const size_t size = length < 0 ? m_pending.size() : static_cast<size_t>(length);
            if (m_pending.size() < size) break;
            out.emplace_back(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(size));
            m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(size));
        }
        return out;
    }

private:
    static constexpr Packet4 kAacp{0x04, 0x00, 0x04, 0x00};

    bool isControl() const {
        static constexpr Packet4 kControls[] = {
            kAirPodsConnected, kAirPodsDisconnected, kRequestDisconnect, kRequestBatteryBytes,
            kRequestAncBytes, kRequestConnectionStat, kRequestHandover, kWindowsAudioActive,
            kWindowsAudioIdle,
            Packet4{0x00, 0x02, 0x00, 0x05},  // REQUEST_TAKEOVER (legacy courtesy)
            Packet4{0x00, 0x06, 0x00, 0x00},  // COURTESY_GRANT
            Packet4{0x00, 0x06, 0x00, 0x01},  // COURTESY_DENY
        };
        for (const auto& p : kControls)
            if (std::memcmp(m_pending.data(), p.data(), 4) == 0) return true;
        return false;
    }

    // AACP packet length at `offset`, -1 if not a known AACP packet,
    // needMore=true if more bytes are required to tell.
    long aacpLength(size_t offset, bool& needMore) const {
        needMore = false;
        const size_t available = m_pending.size() - offset;
        if (available == 0) { needMore = true; return 0; }
        for (size_t i = 0; i < std::min<size_t>(available, 4); ++i)
            if (m_pending[offset + i] != kAacp[i]) return -1;
        if (available < 6) { needMore = true; return 0; }
        switch (m_pending[offset + 4]) {
            case 4: return 22;  // battery
            case 6: return 8;   // ear detection
            case 9: return 11;  // control / ANC
            default: return -1;
        }
    }

    std::vector<std::uint8_t> m_pending;
};

inline Incoming classify(std::span<const std::uint8_t> data) {
    if (equals4(data, kRequestDisconnect))        return Incoming::RequestDisconnect;
    if (equals4(data, kRequestHandover))          return Incoming::RequestHandover;
    if (equals4(data, kAirPodsConnected))         return Incoming::AirPodsConnected;
    if (equals4(data, kAirPodsDisconnected))      return Incoming::AirPodsDisconnected;
    if (equals4(data, kRequestBatteryBytes))      return Incoming::RequestBatteryBytes;
    if (equals4(data, kRequestAncBytes))          return Incoming::RequestAncBytes;
    if (equals4(data, kRequestConnectionStat))    return Incoming::RequestConnectionStatus;
    if (equals4(data, kWindowsAudioActive))       return Incoming::WindowsAudioActive;
    if (equals4(data, kWindowsAudioIdle))         return Incoming::WindowsAudioIdle;
    if (startsWith4(data, kAirPodsDataHeader))    return Incoming::RelayHeader;
    return Incoming::Unknown;
}

}
