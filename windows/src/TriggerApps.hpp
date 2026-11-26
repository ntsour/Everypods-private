#pragma once

// Apps whose audio moves the AirPods to this PC. Teams and VLC are built in;
// the user adds more from the tray menu (saved in config.txt). There is no
// reliable catch-all for "a call started": any app may open the mic (voice
// typing, recorders, a browser tab) or play a sound, so only listed apps count.
//
//   Call  - takes the AirPods when the app opens the microphone, and holds them
//           while it stays open (phones are told not to take them for a call).
//   Media - takes the AirPods when the app starts playing sound.
//
// Pure logic, no Windows headers: unit-tested in tools/peer_v2_tests.cpp.

#include <algorithm>
#include <cwctype>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Propods {

enum class TriggerKind { Call, Media };

struct TriggerApp {
    std::wstring exe;   // normalized: lower-case file name ending in ".exe"
    TriggerKind  kind{TriggerKind::Media};
    bool         builtIn{false};
};

// "C:\\Apps\\Dots.EXE", " dots " -> "dots.exe". Empty if it can't be a file name.
inline std::wstring normalizeExeName(std::wstring_view raw) {
    std::size_t b = 0, e = raw.size();
    while (b < e && std::iswspace(raw[b])) ++b;
    while (e > b && std::iswspace(raw[e - 1])) --e;
    raw = raw.substr(b, e - b);
    if (const auto slash = raw.find_last_of(L"\\/"); slash != std::wstring_view::npos)
        raw = raw.substr(slash + 1);
    std::wstring s;
    s.reserve(raw.size() + 4);
    for (wchar_t c : raw) {
        if (c < 0x20 || std::wstring_view{L":*?\"<>|"}.find(c) != std::wstring_view::npos) return {};
        s.push_back(static_cast<wchar_t>(std::towlower(c)));
    }
    if (s.size() < 4 || s.compare(s.size() - 4, 4, L".exe") != 0) s += L".exe";
    if (s == L".exe") return {};
    return s;
}

inline const std::vector<TriggerApp>& builtInTriggerApps() {
    // Teams: any of its audio sessions starting counts (ringtone, meeting join),
    // as before user apps existed. VLC creates new sessions on seek/track
    // change; the watcher's per-process dedupe stops repeated triggers.
    static const std::vector<TriggerApp> k = {
        {L"ms-teams.exe", TriggerKind::Call,  true},
        {L"teams.exe",    TriggerKind::Call,  true},
        {L"vlc.exe",      TriggerKind::Media, true},
    };
    return k;
}

// Does an audio session of this app becoming active raise a takeover?
// User call apps: only the mic (capture). User media apps: only playback.
// Built-ins keep their original rule: any session.
constexpr bool triggersOn(const TriggerApp& app, bool capture) {
    if (app.builtIn) return true;
    return app.kind == TriggerKind::Call ? capture : !capture;
}

// An open mic of a call app is a call in progress (protects the AirPods).
constexpr bool isCallMic(const TriggerApp& app, bool capture) {
    return capture && app.kind == TriggerKind::Call;
}

// Thread-safe: read on WASAPI callback threads, changed from the tray.
class TriggerAppList {
public:
    // Normalizes, drops invalid names, built-ins and duplicates (first wins).
    void setUserApps(const std::vector<TriggerApp>& apps) {
        std::vector<TriggerApp> clean;
        for (const auto& a : apps) {
            TriggerApp t{normalizeExeName(a.exe), a.kind, false};
            if (t.exe.empty() || isBuiltIn(t.exe)) continue;
            if (std::any_of(clean.begin(), clean.end(), [&](const TriggerApp& c) { return c.exe == t.exe; }))
                continue;
            clean.push_back(std::move(t));
        }
        std::scoped_lock lk{m_mutex};
        m_user = std::move(clean);
    }

    std::vector<TriggerApp> userApps() const {
        std::scoped_lock lk{m_mutex};
        return m_user;
    }

    // Built-ins first, then the user's apps in the order added.
    std::vector<TriggerApp> all() const {
        std::vector<TriggerApp> out = builtInTriggerApps();
        std::scoped_lock lk{m_mutex};
        out.insert(out.end(), m_user.begin(), m_user.end());
        return out;
    }

    std::optional<TriggerApp> find(std::wstring_view exe) const {
        const std::wstring n = normalizeExeName(exe);
        if (n.empty()) return std::nullopt;
        for (const auto& a : builtInTriggerApps())
            if (a.exe == n) return a;
        std::scoped_lock lk{m_mutex};
        for (const auto& a : m_user)
            if (a.exe == n) return a;
        return std::nullopt;
    }

    // Adds the app, or changes its kind if already listed. False for an
    // invalid name, a built-in, or no change.
    bool add(std::wstring_view exe, TriggerKind kind) {
        const std::wstring n = normalizeExeName(exe);
        if (n.empty() || isBuiltIn(n)) return false;
        std::scoped_lock lk{m_mutex};
        for (auto& a : m_user) {
            if (a.exe != n) continue;
            if (a.kind == kind) return false;
            a.kind = kind;
            return true;
        }
        m_user.push_back({n, kind, false});
        return true;
    }

    bool remove(std::wstring_view exe) {
        const std::wstring n = normalizeExeName(exe);
        std::scoped_lock lk{m_mutex};
        return std::erase_if(m_user, [&](const TriggerApp& a) { return a.exe == n; }) > 0;
    }

    static bool isBuiltIn(std::wstring_view normalizedExe) {
        const auto& b = builtInTriggerApps();
        return std::any_of(b.begin(), b.end(), [&](const TriggerApp& a) { return a.exe == normalizedExe; });
    }

private:
    mutable std::mutex m_mutex;
    std::vector<TriggerApp> m_user;
};

}
