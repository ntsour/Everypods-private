#pragma once

// Apps whose audio moves the AirPods to this PC, or keeps them here. Teams,
// VLC and the common meeting apps are built in; the user adds more from the
// tray menu (saved in config.txt). There is no reliable catch-all for "a call
// started": any app may open the mic (voice typing, recorders, a browser tab)
// or play a sound, so only listed apps count.
//
// Each app has three independent settings, in any combination:
//   takeOnMic  - take the AirPods when the app opens the microphone
//   takeOnPlay - take the AirPods when the app starts playing sound
//   keepOnMic  - keep the AirPods while the app has the microphone open: it is
//                a call, so phones and other computers are told not to take them
// or, exclusive of those three:
//   neverTake  - none of the app's audio or mic takes the AirPods (not even a
//                media-controls start) and its open mic is not a call. An app
//                with none of the four set is not listed and follows the
//                defaults (a media-controls start takes the AirPods).
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

enum class TriggerRule { TakeOnMic, TakeOnPlay, KeepOnMic, NeverTake };

struct TriggerApp {
    std::wstring exe;          // normalized: lower-case file name ending in ".exe"
    bool takeOnMic{false};
    bool takeOnPlay{false};
    bool keepOnMic{false};
    bool neverTake{false};     // exclusive of the three above
    bool builtIn{false};
    bool anySessionIsCall{false};  // built-in Teams: any of its sessions starting is a call

    bool any() const { return takeOnMic || takeOnPlay || keepOnMic || neverTake; }
    bool get(TriggerRule r) const {
        switch (r) {
            case TriggerRule::TakeOnMic:  return takeOnMic;
            case TriggerRule::TakeOnPlay: return takeOnPlay;
            case TriggerRule::KeepOnMic:  return keepOnMic;
            case TriggerRule::NeverTake:  return neverTake;
        }
        return false;
    }
    void set(TriggerRule r, bool on) {
        switch (r) {
            case TriggerRule::TakeOnMic:  takeOnMic = on; break;
            case TriggerRule::TakeOnPlay: takeOnPlay = on; break;
            case TriggerRule::KeepOnMic:  keepOnMic = on; break;
            case TriggerRule::NeverTake:  neverTake = on; break;
        }
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
    // Teams: any of its audio sessions starting counts as a call (ringtone,
    // meeting join), as before user apps existed. VLC creates new sessions on
    // seek/track change; the watcher's per-process dedupe stops repeated
    // triggers. Meeting apps only keep: Windows already refused peer requests
    // while they held the AirPods.
    //                  exe              mic    play   keep   never  builtIn anySessionIsCall
    static const std::vector<TriggerApp> k = {
        {L"ms-teams.exe", true,  true,  true,  false, true, true},
        {L"teams.exe",    true,  true,  true,  false, true, true},
        {L"vlc.exe",      false, true,  false, false, true, false},
        {L"zoom.exe",     false, false, true,  false, true, false},
        {L"webex.exe",    false, false, true,  false, true, false},
        {L"slack.exe",    false, false, true,  false, true, false},
        {L"discord.exe",  false, false, true,  false, true, false},
    };
    return k;
}

// What an audio session of this app becoming active raises.
enum class TriggerEvent { None, Call, Media };
constexpr TriggerEvent triggerFor(const TriggerApp& app, bool capture) {
    if (app.neverTake) return TriggerEvent::None;
    if (app.anySessionIsCall) return TriggerEvent::Call;
    if (capture) return app.takeOnMic ? TriggerEvent::Call : TriggerEvent::None;
    return app.takeOnPlay ? TriggerEvent::Media : TriggerEvent::None;
}

// An open mic of a keep app is a call in progress (protects the AirPods).
constexpr bool isCallMic(const TriggerApp& app, bool capture) {
    return capture && app.keepOnMic && !app.neverTake;
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

// For the log.
inline std::string triggerRulesText(const TriggerApp& app) {
    std::string s;
    auto add = [&s](const char* t) { if (!s.empty()) s += ", "; s += t; };
    if (app.takeOnMic)  add("takes the AirPods when it opens the mic");
    if (app.takeOnPlay) add("takes the AirPods when it starts playing");
    if (app.keepOnMic)  add("keeps the AirPods while it uses the mic");
    if (app.neverTake)  add("never takes the AirPods");
    return s.empty() ? "nothing" : s;
}

// Thread-safe: read on WASAPI callback threads, changed from the tray.
class TriggerAppList {
public:
    // Normalizes, drops invalid names, built-ins and apps with no setting;
    // entries for the same app are merged.
    void setUserApps(const std::vector<TriggerApp>& apps) {
        std::vector<TriggerApp> clean;
        for (const auto& a : apps) {
            const std::wstring n = normalizeExeName(a.exe);
            if (n.empty() || isBuiltIn(n) || !a.any()) continue;
            auto it = std::find_if(clean.begin(), clean.end(), [&](const TriggerApp& c) { return c.exe == n; });
            if (it == clean.end()) {
                clean.push_back({n, a.takeOnMic, a.takeOnPlay, a.keepOnMic, a.neverTake, false, false});
            } else {
                it->takeOnMic = it->takeOnMic || a.takeOnMic;
                it->takeOnPlay = it->takeOnPlay || a.takeOnPlay;
                it->keepOnMic = it->keepOnMic || a.keepOnMic;
                it->neverTake = it->neverTake || a.neverTake;
            }
        }
        // Never is exclusive: a setting to take or keep wins over it.
        for (auto& c : clean)
            if (c.takeOnMic || c.takeOnPlay || c.keepOnMic) c.neverTake = false;
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

    // Turns one setting on or off. Adds an unlisted app; an app with no
    // setting left is removed (back to the defaults). Never is exclusive:
    // turning it on clears the other three, turning any of them on clears it.
    // False for an invalid name, a built-in, or no change.
    bool setRule(std::wstring_view exe, TriggerRule rule, bool on) {
        const std::wstring n = normalizeExeName(exe);
        if (n.empty() || isBuiltIn(n)) return false;
        std::scoped_lock lk{m_mutex};
        auto it = std::find_if(m_user.begin(), m_user.end(), [&](const TriggerApp& a) { return a.exe == n; });
        if (it == m_user.end()) {
            if (!on) return false;
            TriggerApp a;
            a.exe = n;
            a.set(rule, true);
            m_user.push_back(std::move(a));
            return true;
        }
        if (it->get(rule) == on) return false;
        if (on && rule == TriggerRule::NeverTake) {
            it->takeOnMic = it->takeOnPlay = it->keepOnMic = false;
        } else if (on) {
            it->neverTake = false;
        }
        it->set(rule, on);
        if (!it->any()) m_user.erase(it);
        return true;
    }

    // The media-controls session `gsmtcAppId` belongs to an app set to never
    // take the AirPods.
    bool neverTakesMedia(std::wstring_view gsmtcAppId) const {
        if (gsmtcAppId.empty()) return false;
        std::scoped_lock lk{m_mutex};
        return std::any_of(m_user.begin(), m_user.end(), [&](const TriggerApp& a) {
            return a.neverTake && gsmtcAppMatchesExe(gsmtcAppId, a.exe);
        });
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
