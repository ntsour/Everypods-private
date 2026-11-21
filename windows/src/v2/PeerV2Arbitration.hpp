#pragma once

// Pure link-selection rules shared by every EveryPods peer (docs/peer-mesh-spec.md
// section 1.2; Android PeerConnectionArbitration and CrossDevice.registerV2Connection).
// No Windows or Bluetooth types here so the unit tests can cover them.

#include <chrono>
#include <string>
#include <vector>

namespace Propods::v2 {

// Identifies one v2 link by the HELLO that opened it. Ordered like Android's
// PeerConnectionKey (initiator installation id, then connection id).
struct ConnectionKey {
    std::string initiatorId;
    std::string connectionId;
    bool operator<(const ConnectionKey& o) const {
        if (initiatorId != o.initiatorId) return initiatorId < o.initiatorId;
        return connectionId < o.connectionId;
    }
    bool operator==(const ConnectionKey& o) const {
        return initiatorId == o.initiatorId && connectionId == o.connectionId;
    }
};

// Index of the smallest key (the link both ends keep), or -1 if there are none.
inline int preferredConnection(const std::vector<ConnectionKey>& keys) {
    int best = -1;
    for (int i = 0; i < static_cast<int>(keys.size()); ++i)
        if (best < 0 || keys[static_cast<std::size_t>(i)] < keys[static_cast<std::size_t>(best)]) best = i;
    return best;
}

// The peer with the smaller installationId dials first; the other waits
// kSecondDialerDelay before its first dial. An unknown peer id means no delay.
inline constexpr std::chrono::milliseconds kSecondDialerDelay{3000};

inline bool dialsFirst(const std::string& ownId, const std::string& peerId) {
    if (ownId.empty() || peerId.empty() || ownId == peerId) return true;
    return ownId < peerId;
}

inline std::chrono::milliseconds firstDialDelay(const std::string& ownId, const std::string& peerId) {
    return dialsFirst(ownId, peerId) ? std::chrono::milliseconds{0} : kSecondDialerDelay;
}

enum class LegacyLinkChoice { KeepExisting, UseNew };

// Two legacy links to the same peer are up at once (one dialled by us, one by
// the peer). When both installation ids are known keep the link dialled by the
// smaller id; otherwise keep the one that came up first. `existingDialledByUs`
// says who dialled the link that was up first.
inline LegacyLinkChoice chooseLegacyLink(const std::string& ownId, const std::string& peerId,
                                         bool existingDialledByUs) {
    if (!ownId.empty() && !peerId.empty() && ownId != peerId) {
        const bool keepOurDialled = ownId < peerId;
        return keepOurDialled == existingDialledByUs ? LegacyLinkChoice::KeepExisting
                                                     : LegacyLinkChoice::UseNew;
    }
    return LegacyLinkChoice::KeepExisting;
}

}  // namespace Propods::v2
