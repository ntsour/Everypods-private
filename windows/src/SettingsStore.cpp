#include "SettingsStore.hpp"

#include "Logger.hpp"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>
#include <string>

namespace Propods {

namespace {

std::filesystem::path resolveConfigPath() {
    PWSTR roaming = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) {
        std::filesystem::path p = roaming;
        CoTaskMemFree(roaming);
        p /= L"Propods";
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        return p / L"config.txt";
    }
    return std::filesystem::current_path() / L"config.txt";
}

std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

}

SettingsStore::SettingsStore() : m_path(resolveConfigPath()) {}

std::optional<std::uint64_t> SettingsStore::parseBluetoothAddress(std::string_view s) {
    std::string compact;
    compact.reserve(12);
    for (char c : s) {
        if (c == ':' || c == '-' || c == ' ') continue;
        compact.push_back(c);
    }
    if (compact.size() != 12) return std::nullopt;
    std::uint64_t value = 0;
    auto [ptr, ec] = std::from_chars(
        compact.data(), compact.data() + compact.size(), value, 16);
    if (ec != std::errc{} || ptr != compact.data() + compact.size()) return std::nullopt;
    return value;
}

std::string SettingsStore::formatBluetoothAddress(std::uint64_t addr) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
        (unsigned)((addr >> 40) & 0xFF),
        (unsigned)((addr >> 32) & 0xFF),
        (unsigned)((addr >> 24) & 0xFF),
        (unsigned)((addr >> 16) & 0xFF),
        (unsigned)((addr >>  8) & 0xFF),
        (unsigned)((addr >>  0) & 0xFF));
    return buf;
}

Settings SettingsStore::load() const {
    log::debug("Loading settings from {}", m_path.string());
    Settings s;
    std::ifstream f(m_path);
    if (!f) {
        log::debug("Settings file not found, using defaults");
        return s;
    }
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        // "android" is the historical key for every peer; "peer" is read too.
        // "computer" lines come from a test build (2026-11-21) and are ignored:
        // the Mac links over Bluetooth LE only, so dialling it never succeeds
        // and only keeps the radio busy. The next save drops them.
        if (key == "computer") {
            log::warn("  computer = {} -> ignored (left by an earlier test build)", val);
        } else if (key == "android" || key == "peer") {
            if (auto addr = parseBluetoothAddress(val)) {
                if (std::find(s.peerAddresses.begin(), s.peerAddresses.end(), *addr) == s.peerAddresses.end())
                    s.peerAddresses.push_back(*addr);
                log::debug("  {} = {} -> {}", key, val, formatBluetoothAddress(*addr));
            } else {
                log::warn("  {} = {} -> (parse failed)", key, val);
            }
        } else if (key == "airpods") {
            s.airpodsAddress = parseBluetoothAddress(val);
            log::debug("  airpods = {} -> {}",
                val, s.airpodsAddress ? formatBluetoothAddress(*s.airpodsAddress) : "(parse failed)");
        } else if (key == "autoroute") {
            s.autoRoute = (val != "0");
            log::debug("  autoroute = {}", s.autoRoute);
        } else if (key == "pauseonrelease") {
            s.pauseOnRelease = (val != "0");
            log::debug("  pauseonrelease = {}", s.pauseOnRelease);
        } else if (key == "takeonmedia") {
            s.takeOnMediaStart = (val != "0");
            log::debug("  takeonmedia = {}", s.takeOnMediaStart);
        } else if (key == "takeoncall") {
            s.takeOnCall = (val != "0");
            log::debug("  takeoncall = {}", s.takeOnCall);
        } else if (key == "holdplayback") {
            s.holdPlayback = (val != "0");
            log::debug("  holdplayback = {}", s.holdPlayback);
        } else if (key == "lidopenconnect") {
            s.lidOpenConnect = (val != "0");
            log::debug("  lidopenconnect = {}", s.lidOpenConnect);
        } else if (key == "lidlease") {
            s.lidLease = (val != "0");
            log::debug("  lidlease = {}", s.lidLease);
        } else if (key == "callapp" || key == "mediaapp") {
            // Cleaned (normalized, de-duplicated) when handed to TriggerAppList.
            const std::wstring exe = normalizeExeName(fromUtf8(val));
            if (exe.empty()) {
                log::warn("  {} = {} -> (not a program name)", key, val);
            } else {
                s.triggerApps.push_back({exe, key == "callapp" ? TriggerKind::Call : TriggerKind::Media, false});
                log::debug("  {} = {}", key, toUtf8(exe));
            }
        }
    }
    return s;
}

void SettingsStore::save(const Settings& s) const {
    std::ofstream f(m_path, std::ios::trunc);
    if (!f) {
        log::warn("Failed to open settings file for write: {}", m_path.string());
        return;
    }
    // Written as "android=" lines (historical key name) so older builds still
    // find the peers.
    for (auto addr : s.peerAddresses)
        f << "android=" << formatBluetoothAddress(addr) << '\n';
    if (s.airpodsAddress) f << "airpods=" << formatBluetoothAddress(*s.airpodsAddress) << '\n';
    f << "autoroute=" << (s.autoRoute ? "1" : "0") << '\n';
    f << "pauseonrelease=" << (s.pauseOnRelease ? "1" : "0") << '\n';
    f << "takeonmedia=" << (s.takeOnMediaStart ? "1" : "0") << '\n';
    f << "takeoncall=" << (s.takeOnCall ? "1" : "0") << '\n';
    f << "holdplayback=" << (s.holdPlayback ? "1" : "0") << '\n';
    f << "lidopenconnect=" << (s.lidOpenConnect ? "1" : "0") << '\n';
    f << "lidlease=" << (s.lidLease ? "1" : "0") << '\n';
    for (const auto& app : s.triggerApps)
        f << (app.kind == TriggerKind::Call ? "callapp=" : "mediaapp=") << toUtf8(app.exe) << '\n';
}

}
