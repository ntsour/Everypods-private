#pragma once

// Identity and hashing helpers for peer protocol v2, byte-compatible with
// Android's PeerIdentityStore and PassiveAcquisitionCoordinator.

#include <cstdint>
#include <string>
#include <vector>

namespace Propods::v2 {

// Lowercase canonical UUID check ("xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx").
bool isCanonicalUuid(const std::string& s);

// Random version-4 UUID, lowercase.
std::string randomUuid();

// SHA-256 as 64 lowercase hex characters.
std::string sha256Hex(const std::string& data);

// Android PeerIdentityStore.headsetIdFromAddress: SHA-256 of the uppercase
// "AA:BB:CC:DD:EE:FF" address, first 16 bytes, version nibble 8, variant 10.
std::string headsetIdFromAddress(std::uint64_t address);

// "AA:BB:CC:DD:EE:FF" from a 48-bit Bluetooth address.
std::string formatAddress(std::uint64_t address);

// SHA-256 over the sorted member ids joined by "\n" with a trailing "\n".
// `sortedMembers` must already be sorted and unique.
std::string membershipDigest(const std::vector<std::string>& sortedMembers);

// Stable installation id, persisted in %APPDATA%\Propods\peer-identity.txt,
// and a session id that is new for every process.
struct LocalIdentity {
    std::string installationId;
    std::string sessionId;
};
const LocalIdentity& localIdentity();

}  // namespace Propods::v2
