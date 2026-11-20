#include "PeerV2Identity.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>

#pragma comment(lib, "bcrypt.lib")

namespace Propods::v2 {

namespace {

std::string hex(const std::uint8_t* data, std::size_t n) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        out += kDigits[data[i] >> 4];
        out += kDigits[data[i] & 0x0F];
    }
    return out;
}

std::string uuidFromBytes(const std::uint8_t b[16]) {
    const std::string h = hex(b, 16);
    return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" + h.substr(20);
}

void sha256(const void* data, std::size_t len, std::uint8_t out[32]) {
    const NTSTATUS st = BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
        static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(len), out, 32);
    if (st != 0) throw std::runtime_error("sha256_failed");
}

std::filesystem::path identityPath() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD len = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    std::filesystem::path dir = (len > 0 && len < MAX_PATH)
        ? std::filesystem::path{buf} / L"Propods"
        : std::filesystem::current_path();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / L"peer-identity.txt";
}

}  // namespace

bool isCanonicalUuid(const std::string& s) {
    if (s.size() != 36) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

std::string randomUuid() {
    std::uint8_t b[16];
    if (BCryptGenRandom(nullptr, b, sizeof(b), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("random_failed");
    b[6] = static_cast<std::uint8_t>((b[6] & 0x0F) | 0x40);
    b[8] = static_cast<std::uint8_t>((b[8] & 0x3F) | 0x80);
    return uuidFromBytes(b);
}

std::string sha256Hex(const std::string& data) {
    std::uint8_t d[32];
    sha256(data.data(), data.size(), d);
    return hex(d, 32);
}

std::string formatAddress(std::uint64_t address) {
    char buf[18];
    std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
        static_cast<unsigned>((address >> 40) & 0xFF), static_cast<unsigned>((address >> 32) & 0xFF),
        static_cast<unsigned>((address >> 24) & 0xFF), static_cast<unsigned>((address >> 16) & 0xFF),
        static_cast<unsigned>((address >> 8) & 0xFF), static_cast<unsigned>(address & 0xFF));
    return buf;
}

std::string headsetIdFromAddress(std::uint64_t address) {
    const std::string text = formatAddress(address);
    std::uint8_t d[32];
    sha256(text.data(), text.size(), d);
    d[6] = static_cast<std::uint8_t>((d[6] & 0x0F) | 0x80);
    d[8] = static_cast<std::uint8_t>((d[8] & 0x3F) | 0x80);
    return uuidFromBytes(d);
}

std::string membershipDigest(const std::vector<std::string>& sortedMembers) {
    std::string canonical;
    for (const auto& m : sortedMembers) { canonical += m; canonical += '\n'; }
    return sha256Hex(canonical);
}

const LocalIdentity& localIdentity() {
    static std::once_flag once;
    static LocalIdentity identity;
    std::call_once(once, [] {
        const auto path = identityPath();
        std::string id;
        { std::ifstream in{path}; std::getline(in, id); }
        if (!isCanonicalUuid(id)) {
            id = randomUuid();
            std::ofstream out{path, std::ios::trunc};
            out << id << '\n';
        }
        identity.installationId = id;
        identity.sessionId = randomUuid();
    });
    return identity;
}

}  // namespace Propods::v2
