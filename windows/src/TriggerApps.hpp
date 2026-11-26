#pragma once

// Which apps move the AirPods to this PC, or keep them here. Every app, built
// in or not, has the same three independent settings:
//   takeOnMic  - take the AirPods when the app opens the microphone
//   takeOnPlay - take the AirPods when the app starts playing; only a start in
//                the Windows media controls counts, so notification dings
//                never do (VLC, outside the media controls, is the exception)
//   keepOnMic  - keep the AirPods while the app has the microphone open: it is
//                a call, so phones and other computers are told not to take them
// Nothing ticked: the app never takes or keeps the AirPods.
//
// Where an app's settings come from, first match wins:
//   1. what the user saved (any app, built-ins included),
//   2. the built-in catalog (meeting apps, browsers, media players),
//   3. all three, for an app first seen using the mic and sound together,
//   4. take on play.
// Rule 3 is applied by saving the app's settings when it is first seen.
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

enum class TriggerRule { TakeOnMic, TakeOnPlay, KeepOnMic };

struct TriggerApp {
    std::wstring exe;          // normalized: lower-case file name ending in ".exe"
    bool takeOnMic{false};
    bool takeOnPlay{false};
    bool keepOnMic{false};
    bool builtIn{false};       // in the built-in catalog
    bool changed{false};       // a built-in with settings the user saved
    bool saved{false};         // the user saved settings for it (rule 1)
    // VLC: its playback is outside the media controls, so its audio session
    // starting counts as a start (built-in only).
    bool playWithoutMediaControls{false};

    bool get(TriggerRule r) const {
        switch (r) {
            case TriggerRule::TakeOnMic:  return takeOnMic;
            case TriggerRule::TakeOnPlay: return takeOnPlay;
            case TriggerRule::KeepOnMic:  return keepOnMic;
        }
        return false;
    }
    void set(TriggerRule r, bool on) {
        switch (r) {
            case TriggerRule::TakeOnMic:  takeOnMic = on; break;
            case TriggerRule::TakeOnPlay: takeOnPlay = on; break;
            case TriggerRule::KeepOnMic:  keepOnMic = on; break;
        }
    }
    bool sameSettings(const TriggerApp& o) const {
        return takeOnMic == o.takeOnMic && takeOnPlay == o.takeOnPlay && keepOnMic == o.keepOnMic;
    }
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
    // Pre-ticked to match the behaviour before per-app settings:
    // - Teams: a call opens the mic; its other sounds (chat pings) don't count.
    // - Zoom, Webex, Slack, Discord: an open mic is a call (they never took the
    //   AirPods by themselves).
    // - Browsers: media-controls starts take the AirPods (YouTube...), and a
    //   meeting tab's open mic keeps them.
    // - Media players: playback takes them. VLC 3 is outside the media controls.
    auto app = [](const wchar_t* exe, bool mic, bool play, bool keep, bool outsideMc = false) {
        TriggerApp a;
        a.exe = exe;
        a.takeOnMic = mic;
        a.takeOnPlay = play;
        a.keepOnMic = keep;
        a.builtIn = true;
        a.playWithoutMediaControls = outsideMc;
        return a;
    };
    //                          mic    play   keep
    static const std::vector<TriggerApp> k = {
        app(L"ms-teams.exe",    true,  false, true),
        app(L"teams.exe",       true,  false, true),
        app(L"zoom.exe",        false, false, true),
        app(L"webex.exe",       false, false, true),
        app(L"slack.exe",       false, false, true),
        app(L"discord.exe",     false, false, true),
        app(L"chrome.exe",      false, true,  true),
        app(L"msedge.exe",      false, true,  true),
        app(L"firefox.exe",     false, true,  true),
        app(L"brave.exe",       false, true,  true),
        app(L"spotify.exe",     false, true,  false),
        app(L"vlc.exe",         false, true,  false, /*outsideMc=*/true),
    };
    return k;
}

// What an audio session of this app becoming active raises. Playback only
// counts for apps outside the media controls (VLC); every other start comes
// from the media controls (see takesOnMediaStart).
enum class TriggerEvent { None, Call, Media };
constexpr TriggerEvent triggerFor(const TriggerApp& app, bool capture) {
    if (capture) return app.takeOnMic ? TriggerEvent::Call : TriggerEvent::None;
    return app.takeOnPlay && app.playWithoutMediaControls ? TriggerEvent::Media : TriggerEvent::None;
}

// An open mic of a keep app is a call in progress (protects the AirPods).
constexpr bool isCallMic(const TriggerApp& app, bool capture) {
    return capture && app.keepOnMic;
}

// Does the media-controls (GSMTC) session `appId` (SourceAppUserModelId)
// belong to the process image `exe`? Heuristic: unpackaged apps use their exe
// name ("Spotify.exe", "Chrome", "MSEdge"); Firefox uses a 16-hex-digit
// install hash; the new Media Player is packaged.
inline bool gsmtcAppMatchesExe(std::wstring_view appId, std::wstring_view exe) {
    std::wstring a, b;
    for (wchar_t c : appId) a.push_back(static_cast<wchar_t>(std::towlower(c)));
    for (wchar_t c : exe) b.push_back(static_cast<wchar_t>(std::towlower(c)));
    if (b.size() > 4 && b.compare(b.size() - 4, 4, L".exe") == 0) b.resize(b.size() - 4);
    if (b.size() >= 3 && a.find(b) != std::wstring::npos) return true;
    const bool hex16 = a.size() == 16 && std::all_of(a.begin(), a.end(), [](wchar_t c) { return std::iswxdigit(c) != 0; });
    if (b == L"firefox" && hex16) return true;
    if (b == L"microsoft.media.player" && a.find(L"zunemusic") != std::wstring::npos) return true;
    return false;
}

// Menu line and log text: "takes the AirPods when it opens the mic; ...".
inline std::string triggerRulesText(const TriggerApp& app) {
    std::string s;
    auto add = [&s](const char* t) { if (!s.empty()) s += "; "; s += t; };
    if (app.takeOnMic)  add("takes the AirPods when it opens the mic");
    if (app.takeOnPlay) add("takes the AirPods when it starts playing");
    if (app.keepOnMic)  add("keeps the AirPods while it uses the mic");
    return s.empty() ? "never takes or keeps the AirPods" : s;
}

// Thread-safe: read on WASAPI and media callback threads, changed from the tray.
class TriggerAppList {
public:
    // The user's saved settings (config.txt). Entries for the same app are
    // merged; an entry with nothing ticked is kept (it means "never").
    void setUserApps(const std::vector<TriggerApp>& apps) {
        std::vector<TriggerApp> clean;
        for (const auto& a : apps) {
            const std::wstring n = normalizeExeName(a.exe);
            if (n.empty()) continue;
            auto it = std::find_if(clean.begin(), clean.end(), [&](const TriggerApp& c) { return c.exe == n; });
            if (it == clean.end()) {
                TriggerApp c;
                c.exe = n;
                c.takeOnMic = a.takeOnMic;
                c.takeOnPlay = a.takeOnPlay;
                c.keepOnMic = a.keepOnMic;
                clean.push_back(std::move(c));
            } else {
                it->takeOnMic = it->takeOnMic || a.takeOnMic;
                it->takeOnPlay = it->takeOnPlay || a.takeOnPlay;
                it->keepOnMic = it->keepOnMic || a.keepOnMic;
            }
        }
        std::scoped_lock lk{m_mutex};
        m_user = std::move(clean);
    }

    // What config.txt stores: the saved settings only.
    std::vector<TriggerApp> userApps() const {
        std::scoped_lock lk{m_mutex};
        return m_user;
    }

    // The settings in force for any app (rules 1, 2, 4; rule 3 is saved).
    TriggerApp resolve(std::wstring_view exe) const {
        const std::wstring n = normalizeExeName(exe);
        const TriggerApp* builtIn = findBuiltIn(n);
        TriggerApp out;
        if (builtIn) {
            out = *builtIn;
        } else {
            out.exe = n;
            out.takeOnPlay = true;  // rule 4
        }
        std::scoped_lock lk{m_mutex};
        if (const TriggerApp* user = findUser(n)) {
            out.takeOnMic = user->takeOnMic;
            out.takeOnPlay = user->takeOnPlay;
            out.keepOnMic = user->keepOnMic;
            out.saved = true;
            out.changed = builtIn != nullptr;
        }
        return out;
    }

    // The built-ins (with any saved changes) and every other saved app.
    std::vector<TriggerApp> listed() const {
        std::vector<TriggerApp> out;
        for (const auto& b : builtInTriggerApps()) out.push_back(resolve(b.exe));
        std::scoped_lock lk{m_mutex};
        for (const auto& u : m_user) {
            if (findBuiltIn(u.exe)) continue;
            TriggerApp a = u;
            a.saved = true;
            out.push_back(std::move(a));
        }
        return out;
    }

    // Turns one setting on or off and saves the app's settings (from what is
    // in force now). False for an invalid name or no change.
    bool setRule(std::wstring_view exe, TriggerRule rule, bool on) {
        const std::wstring n = normalizeExeName(exe);
        if (n.empty()) return false;
        TriggerApp now = resolve(n);
        if (now.get(rule) == on) return false;
        now.set(rule, on);
        TriggerApp entry;
        entry.exe = n;
        entry.takeOnMic = now.takeOnMic;
        entry.takeOnPlay = now.takeOnPlay;
        entry.keepOnMic = now.keepOnMic;
        std::scoped_lock lk{m_mutex};
        if (TriggerApp* user = findUser(n)) *user = entry;
        else m_user.push_back(std::move(entry));
        return true;
    }

    // Rule 3: an app first seen using the mic and sound together starts with
    // all three. Only for an app with no saved settings and not built in.
    bool adoptCallApp(std::wstring_view exe) {
        const std::wstring n = normalizeExeName(exe);
        if (n.empty() || findBuiltIn(n)) return false;
        std::scoped_lock lk{m_mutex};
        if (findUser(n)) return false;
        TriggerApp a;
        a.exe = n;
        a.takeOnMic = a.takeOnPlay = a.keepOnMic = true;
        m_user.push_back(std::move(a));
        return true;
    }

    // "Reset to built-in": drop the saved settings of a built-in.
    bool resetToBuiltIn(std::wstring_view exe) {
        const std::wstring n = normalizeExeName(exe);
        if (!findBuiltIn(n)) return false;
        std::scoped_lock lk{m_mutex};
        return std::erase_if(m_user, [&](const TriggerApp& a) { return a.exe == n; }) > 0;
    }

    // A start in the media controls by session `gsmtcAppId`: does its app
    // take the AirPods on play? An app the session can't be matched to
    // follows rule 4 (it does).
    bool takesOnMediaStart(std::wstring_view gsmtcAppId) const {
        if (gsmtcAppId.empty()) return true;
        for (const auto& b : builtInTriggerApps())
            if (gsmtcAppMatchesExe(gsmtcAppId, b.exe)) return resolve(b.exe).takeOnPlay;
        std::scoped_lock lk{m_mutex};
        for (const auto& u : m_user)
            if (gsmtcAppMatchesExe(gsmtcAppId, u.exe)) return u.takeOnPlay;
        return true;
    }

    static bool isBuiltIn(std::wstring_view normalizedExe) { return findBuiltIn(normalizedExe) != nullptr; }

private:
    static const TriggerApp* findBuiltIn(std::wstring_view n) {
        for (const auto& b : builtInTriggerApps())
            if (b.exe == n) return &b;
        return nullptr;
    }
    TriggerApp* findUser(std::wstring_view n) {
        for (auto& u : m_user) if (u.exe == n) return &u;
        return nullptr;
    }
    const TriggerApp* findUser(std::wstring_view n) const {
        for (const auto& u : m_user) if (u.exe == n) return &u;
        return nullptr;
    }

    mutable std::mutex m_mutex;
    std::vector<TriggerApp> m_user;
};

}
