#include "PlaybackHold.hpp"

#include "Logger.hpp"
#include "MediaPlaybackWatcher.hpp"

#include <windows.h>

#include <winrt/base.h>

#include <algorithm>
#include <condition_variable>
#include <cwctype>
#include <mutex>
#include <thread>
#include <vector>

namespace Propods {

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

// Timings, as on Android / Mac except where noted.
// Each Windows connect attempt waits a full page (~6 s) and the controller
// retries back to back, so one attempt window is longer than Android's 10 s.
constexpr milliseconds kAttemptTimeout{15000};
constexpr milliseconds kFailedChoiceTimeout{12000};
constexpr milliseconds kPeerReleaseWait{1500};   // RequestingRelease -> Connecting without an ack
constexpr milliseconds kRoutePollInterval{150};
// Windows can take a few seconds after the link is up to open A2DP and make
// the endpoint ACTIVE; after this we resume anyway (the AirPods are connected).
constexpr milliseconds kRoutePollMax{6000};
constexpr milliseconds kConnectedLinger{1500};
constexpr milliseconds kMuteCap{1500};           // no session stays muted longer than this
constexpr milliseconds kResumeDelay{300};        // let apps reopen their stream on the AirPods
constexpr milliseconds kSafetyReplay{800};
constexpr milliseconds kExpectPlaying{3000};     // Playing events after our own play are ours
constexpr milliseconds kSuppressAfterPlayHere{5000};

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

// "Chrome.exe" -> "chrome"
std::wstring exeBase(const std::wstring& exe) {
    std::wstring b = lower(exe);
    if (b.size() > 4 && b.compare(b.size() - 4, 4, L".exe") == 0) b.resize(b.size() - 4);
    return b;
}

bool isCommsExe(const std::wstring& base) {
    static const wchar_t* const kComms[] = {
        L"ms-teams", L"teams", L"zoom", L"slack", L"discord", L"webex", L"ciscocollabhost",
        L"skype", L"lync", L"msedgewebview2",
    };
    for (auto* c : kComms) if (base == c) return true;
    return false;
}

bool isHex16(const std::wstring& s) {
    if (s.size() != 16) return false;
    return std::all_of(s.begin(), s.end(), [](wchar_t c) { return std::iswxdigit(c) != 0; });
}

// Does the GSMTC session `appId` (SourceAppUserModelId) belong to the process
// image `exe`? Heuristic: unpackaged apps use their exe name ("Spotify.exe",
// "Chrome", "MSEdge"); Firefox uses a 16-hex-digit install hash; the new
// Media Player is packaged.
bool appMatchesExe(const std::wstring& appId, const std::wstring& exe) {
    const std::wstring a = lower(appId);
    const std::wstring b = exeBase(exe);
    if (b.size() >= 3 && a.find(b) != std::wstring::npos) return true;
    if (b == L"firefox" && isHex16(a)) return true;
    if (b == L"microsoft.media.player" && a.find(L"zunemusic") != std::wstring::npos) return true;
    return false;
}

// Apps worth a pre-mute: media players and browsers. Anything else (games,
// notification sounds of other apps) is left alone.
bool isMediaCapable(const std::wstring& base, const std::vector<std::wstring>& gsmtcIds) {
    static const wchar_t* const kKnown[] = {
        L"chrome", L"msedge", L"firefox", L"brave", L"opera", L"vivaldi", L"arc",
        L"spotify", L"microsoft.media.player", L"applemusic", L"itunes", L"tidal", L"deezer",
    };
    for (auto* k : kKnown) if (base == k) return true;
    for (const auto& id : gsmtcIds) if (appMatchesExe(id, base + L".exe")) return true;
    return false;
}

const char* phaseName(HoldPhase p) {
    switch (p) {
        case HoldPhase::RequestingRelease: return "requestingRelease";
        case HoldPhase::Connecting:        return "connecting";
        case HoldPhase::Routing:           return "routing";
        case HoldPhase::Connected:         return "connected";
        case HoldPhase::Failed:            return "failed";
    }
    return "?";
}

std::int64_t nowMs() {
    return std::chrono::duration_cast<milliseconds>(Clock::now().time_since_epoch()).count();
}

// A single worker thread running posted and delayed tasks in order. A named
// task replaces an earlier pending task with the same name; cancel(name)
// drops it. The thread is an MTA (WinRT media calls and WASAPI are made here).
class Worker {
public:
    Worker() : m_thread([this] { run(); }) {}
    ~Worker() { stop(); }

    void post(std::function<void()> fn) { schedule({}, milliseconds{0}, std::move(fn)); }

    void schedule(const std::string& name, milliseconds delay, std::function<void()> fn) {
        std::scoped_lock lk{m_mutex};
        if (m_stopping) return;
        if (!name.empty()) eraseLocked(name);
        m_items.push_back({Clock::now() + delay, ++m_seq, name, std::move(fn)});
        m_cv.notify_one();
    }

    void cancel(const std::string& name) {
        std::scoped_lock lk{m_mutex};
        eraseLocked(name);
    }

    // Drops every named (timer) task; plain posts still run.
    void cancelTimers() {
        std::scoped_lock lk{m_mutex};
        std::erase_if(m_items, [](const Item& i) { return !i.name.empty(); });
    }

    void stop() {
        {
            std::scoped_lock lk{m_mutex};
            if (m_stopping) return;
            m_stopping = true;
            m_items.clear();
        }
        m_cv.notify_all();
        if (m_thread.joinable()) {
            if (m_thread.get_id() == std::this_thread::get_id()) m_thread.detach();
            else m_thread.join();
        }
    }

private:
    struct Item {
        Clock::time_point due;
        std::uint64_t seq;
        std::string name;
        std::function<void()> fn;
    };

    void eraseLocked(const std::string& name) {
        std::erase_if(m_items, [&](const Item& i) { return i.name == name; });
    }

    void run() {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        } catch (...) {
            log::warn("Hold: worker could not initialise its apartment; continuing");
        }
        std::unique_lock lk{m_mutex};
        while (!m_stopping) {
            if (m_items.empty()) { m_cv.wait(lk); continue; }
            auto it = std::min_element(m_items.begin(), m_items.end(), [](const Item& a, const Item& b) {
                return a.due != b.due ? a.due < b.due : a.seq < b.seq;
            });
            if (it->due > Clock::now()) {
                const auto due = it->due;
                m_cv.wait_until(lk, due);
                continue;
            }
            auto fn = std::move(it->fn);
            m_items.erase(it);
            lk.unlock();
            try {
                fn();
            } catch (const winrt::hresult_error& e) {
                log::warn("Hold: task failed 0x{:08X} {}", static_cast<std::uint32_t>(e.code().value),
                          winrt::to_string(e.message()));
            } catch (const std::exception& e) {
                log::warn("Hold: task failed: {}", e.what());
            } catch (...) {
                log::warn("Hold: task failed (unknown exception)");
            }
            lk.lock();
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<Item> m_items;
    std::uint64_t m_seq = 0;
    bool m_stopping = false;
    std::thread m_thread;  // last: started after the members above exist
};

}  // namespace

// Everything below runs on the worker thread unless noted.
class PlaybackHold::Impl {
public:
    Impl(PlaybackHold& owner, MediaPlaybackWatcher& media, SessionMute& mute)
        : m_owner(owner), m_media(media), m_mute(mute) {}

    PlaybackHold& m_owner;
    MediaPlaybackWatcher& m_media;
    SessionMute& m_mute;
    Worker m_worker;

    std::mutex m_configMutex;  // guards m_hooks / m_onStatus (set from the main thread)
    Hooks m_hooks;
    StatusCallback m_onStatus;

    std::atomic<std::int64_t> m_suppressUntilMs{0};

    std::optional<HoldStatus> m_status;
    bool m_wePaused = false;
    bool m_sawPaused = false;
    std::wstring m_heldAppId;
    Clock::time_point m_expectPlayingUntil{};
    Clock::time_point m_routePollStarted{};

    Hooks hooks() {
        std::scoped_lock lk{m_configMutex};
        return m_hooks;
    }

    bool active() const { return m_status && m_status->phase != HoldPhase::Connected; }

    void publish() {
        m_owner.m_active.store(active());
        StatusCallback cb;
        {
            std::scoped_lock lk{m_configMutex};
            cb = m_onStatus;
        }
        if (cb) cb(m_status);
    }

    void schedule(const std::string& name, milliseconds delay, std::function<void()> fn) {
        m_worker.schedule(name, delay, std::move(fn));
    }
    void cancel(const std::string& name) { m_worker.cancel(name); }

    void setPhase(HoldPhase phase) {
        if (!m_status || m_status->phase == phase) return;
        log::handover("HOLD    phase {} -> {}", phaseName(m_status->phase), phaseName(phase));
        m_status->phase = phase;
        m_status->phaseStartedAt = Clock::now();
        publish();
    }

    void suppressTriggers(const char* why) {
        m_suppressUntilMs.store(nowMs() + kSuppressAfterPlayHere.count());
        log::handover("HOLD    media triggers suppressed for {} ms ({})", kSuppressAfterPlayHere.count(), why);
    }

    // ---- pre-mute ----

    void onRenderSessionActive(const AudioSessionRef& s) {
        if (!m_owner.enabled() || active()) return;
        const std::wstring base = exeBase(s.exe);
        if (base.empty() || isCommsExe(base)) return;
        if (!isMediaCapable(base, m_media.sessionAppIds())) return;
        auto h = hooks();
        if (!h.preMuteAllowed || !h.preMuteAllowed()) return;
        if (m_mute.mute(s, "pre-mute: playback seen, takeover likely")) {
            schedule("preMuteCap", kMuteCap, [this] {
                if (!active()) m_mute.restoreAll("no takeover followed the pre-mute");
            });
        }
    }

    // ---- hold lifecycle ----

    void start(HoldKind kind, bool peerAsked, const std::wstring& appId, bool holdAudio) {
        cancel("preMuteCap");
        if (active()) {
            if (m_status->kind == kind) {
                log::handover("HOLD    already active ({}), keeping it", phaseName(m_status->phase));
                return;
            }
            // A call during a media hold: the media stays paused, the call owns the panel.
            endStayPaused("a call takes over", /*cancelTakeover=*/false);
        }
        cancel("linger");
        cancel("resume");
        cancel("safetyReplay");

        m_wePaused = false;
        m_sawPaused = false;
        m_heldAppId.clear();

        if (kind == HoldKind::Call) {
            // Calls are never held; undo a pre-mute that may have caught a web call.
            m_mute.restoreAll("call: nothing is held");
        } else if (!holdAudio || !m_owner.enabled()) {
            // Progress only: the hold is off, or no peer is using the AirPods.
            m_mute.restoreAll("media: progress only");
            log::handover("HOLD    media progress only ({})",
                          m_owner.enabled() ? "AirPods not in use on a peer" : "hold setting off");
        } else {
            m_heldAppId = appId;
            if (!appId.empty()) {
                const std::wstring id = appId;
                m_mute.muteActiveSessionsOf([&id](const std::wstring& exe) { return appMatchesExe(id, exe); },
                                            "hold start");
                if (m_media.isSessionPlaying(appId).value_or(false) && m_media.pauseSession(appId)) {
                    m_wePaused = true;
                }
            }
            log::handover("HOLD    media hold for '{}': {}", log::toUtf8(appId),
                          m_wePaused ? "paused" : "nothing paused (outside media controls or refused); progress only");
            schedule("muteCap", kMuteCap, [this] { m_mute.restoreAll("mute cap reached"); });
        }

        const auto now = Clock::now();
        HoldStatus st;
        st.phase = peerAsked ? HoldPhase::RequestingRelease : HoldPhase::Connecting;
        st.kind = kind;
        auto h = hooks();
        st.deviceName = h.deviceName ? h.deviceName() : std::wstring{};
        if (st.deviceName.empty()) st.deviceName = L"AirPods";
        st.attempt = 1;
        st.phaseStartedAt = now;
        st.attemptStartedAt = now;
        st.attemptTimeout = kAttemptTimeout;
        m_status = st;
        log::handover("HOLD    start {} for '{}' (phase {})", kind == HoldKind::Media ? "media" : "call",
                      log::toUtf8(st.deviceName), phaseName(st.phase));
        publish();
        armAttemptTimeout();
        if (peerAsked) {
            schedule("peerWait", kPeerReleaseWait, [this] {
                if (active() && m_status->phase == HoldPhase::RequestingRelease) setPhase(HoldPhase::Connecting);
            });
        }
    }

    void onSessionPlaybackChanged(const std::wstring& appId, bool playing) {
        if (!active() || m_status->kind != HoldKind::Media || appId != m_heldAppId || !m_wePaused) return;
        if (!playing) {
            if (!m_sawPaused) {
                m_sawPaused = true;
                cancel("muteCap");
                m_mute.restoreAll("app paused");
            }
            return;
        }
        // Playing again during the hold, not from our own play: the user took
        // playback back. End the hold; the connection carries on.
        if (m_sawPaused && Clock::now() >= m_expectPlayingUntil) {
            log::handover("HOLD    user resumed '{}' during the hold — ending it", log::toUtf8(appId));
            m_wePaused = false;
            suppressTriggers("user resumed during the hold");
            const bool wasFailed = m_status->phase == HoldPhase::Failed;
            if (wasFailed) { auto h = hooks(); if (h.cancelTakeover) h.cancelTakeover(); }
            m_worker.cancelTimers();
            m_mute.restoreAll("user resumed");
            clear("user resumed");
        }
    }

    void onPeerReleased() {
        if (!active() || m_status->phase != HoldPhase::RequestingRelease) return;
        cancel("peerWait");
        setPhase(HoldPhase::Connecting);
    }

    void onConnected() {
        if (!active() || m_status->phase == HoldPhase::Routing) return;
        cancel("failedChoice");
        cancel("peerWait");
        cancel("attempt");  // routing has its own bound
        setPhase(HoldPhase::Routing);
        if (m_status->kind == HoldKind::Call) return connected();
        m_routePollStarted = Clock::now();
        pollRoute();
    }

    void pollRoute() {
        if (!active() || m_status->phase != HoldPhase::Routing) return;
        const auto elapsed = std::chrono::duration_cast<milliseconds>(Clock::now() - m_routePollStarted);
        auto h = hooks();
        if (h.airPodsAreDefaultOutput && h.airPodsAreDefaultOutput()) {
            log::handover("HOLD    AirPods are the default output after {} ms", elapsed.count());
            return connected();
        }
        if (elapsed >= kRoutePollMax) {
            log::handover("HOLD    AirPods not the default output after {} ms; releasing anyway", elapsed.count());
            return connected();
        }
        schedule("routePoll", kRoutePollInterval, [this] { pollRoute(); });
    }

    void connected() {
        m_worker.cancelTimers();
        setPhase(HoldPhase::Connected);
        if (m_status->kind == HoldKind::Media) {
            if (m_wePaused) {
                const std::wstring app = m_heldAppId;
                schedule("resume", kResumeDelay, [this, app] {
                    m_mute.restoreAll("connected");
                    m_expectPlayingUntil = Clock::now() + kExpectPlaying;
                    const bool ok = m_media.playSession(app);
                    log::handover("HOLD    resume '{}' on the AirPods: {}", log::toUtf8(app), ok ? "ok" : "refused");
                    schedule("safetyReplay", kSafetyReplay, [this, app] {
                        if (m_media.isSessionPlaying(app) == std::optional<bool>{false}) {
                            m_expectPlayingUntil = Clock::now() + kExpectPlaying;
                            log::handover("HOLD    '{}' still paused; replaying once", log::toUtf8(app));
                            m_media.playSession(app);
                        }
                    });
                });
            } else {
                m_mute.restoreAll("connected");
            }
        }
        m_wePaused = false;
        schedule("linger", kConnectedLinger, [this] {
            if (m_status && m_status->phase == HoldPhase::Connected) {
                m_status.reset();
                publish();
            }
        });
    }

    void armAttemptTimeout() {
        if (!m_status) return;
        const auto left = std::chrono::duration_cast<milliseconds>(
            m_status->attemptStartedAt + m_status->attemptTimeout - Clock::now());
        schedule("attempt", (std::max)(left, milliseconds{10}), [this] {
            if (!active()) return;
            const auto p = m_status->phase;
            if (p == HoldPhase::RequestingRelease || p == HoldPhase::Connecting) fail("attempt timed out");
        });
    }

    void fail(const char* reason) {
        log::handover("HOLD    failed ({}); waiting for Retry / Play on PC", reason);
        cancel("attempt");
        cancel("routePoll");
        cancel("peerWait");
        setPhase(HoldPhase::Failed);
        schedule("failedChoice", kFailedChoiceTimeout, [this] {
            endStayPaused("no choice within 12 s", /*cancelTakeover=*/true);
        });
    }

    void onConnectFailed() {
        if (!active()) return;
        const auto p = m_status->phase;
        if (p == HoldPhase::RequestingRelease || p == HoldPhase::Connecting) fail("every connect attempt failed");
    }

    void onLost(const char* reason) {
        if (active()) endStayPaused(reason, /*cancelTakeover=*/false);
    }

    void onCallEnded() {
        if (m_status && m_status->kind == HoldKind::Call) clear("call ended");
    }

    // ---- actions ----

    void retry() {
        if (!active() || m_status->phase != HoldPhase::Failed) return;
        cancel("failedChoice");
        const auto now = Clock::now();
        m_status->attempt += 1;
        m_status->phase = HoldPhase::Connecting;
        m_status->phaseStartedAt = now;
        m_status->attemptStartedAt = now;
        log::handover("HOLD    retry (attempt {})", m_status->attempt);
        publish();
        armAttemptTimeout();
        auto h = hooks();
        if (h.retryTakeover) h.retryTakeover(m_status->kind);
    }

    void playHere() {
        if (!m_status) return;
        log::handover("HOLD    play here");
        auto h = hooks();
        if (h.cancelTakeover) h.cancelTakeover();
        m_worker.cancelTimers();
        m_mute.restoreAll("play here");
        if (m_status->kind == HoldKind::Media) {
            suppressTriggers("play on PC");
            if (m_wePaused && !m_heldAppId.empty()) {
                m_expectPlayingUntil = Clock::now() + kExpectPlaying;
                const bool ok = m_media.playSession(m_heldAppId);
                log::handover("HOLD    resume '{}' on the PC: {}", log::toUtf8(m_heldAppId), ok ? "ok" : "refused");
            }
        }
        m_wePaused = false;
        clear("play here");
    }

    void dismiss() {
        if (m_status) endStayPaused("dismissed", /*cancelTakeover=*/true);
    }

    // ---- endings ----

    void endStayPaused(const char* reason, bool cancelTakeover) {
        log::handover("HOLD    end, media stays paused ({})", reason);
        if (cancelTakeover) {
            auto h = hooks();
            if (h.cancelTakeover) h.cancelTakeover();
        }
        m_worker.cancelTimers();
        m_mute.restoreAll(reason);
        m_wePaused = false;
        clear(reason);
    }

    void clear(const char* reason) {
        if (!m_status) return;
        log::handover("HOLD    cleared ({})", reason);
        m_status.reset();
        m_heldAppId.clear();
        publish();
    }
};

// ---- PlaybackHold: every call hops onto the worker thread ----

PlaybackHold::PlaybackHold(MediaPlaybackWatcher& media, SessionMute& mute)
    : m_impl(std::make_unique<Impl>(*this, media, mute)) {}

PlaybackHold::~PlaybackHold() { shutdown(); }

void PlaybackHold::setHooks(Hooks hooks) {
    std::scoped_lock lk{m_impl->m_configMutex};
    m_impl->m_hooks = std::move(hooks);
}

void PlaybackHold::setOnStatusChange(StatusCallback cb) {
    std::scoped_lock lk{m_impl->m_configMutex};
    m_impl->m_onStatus = std::move(cb);
}

void PlaybackHold::onRenderSessionActive(const AudioSessionRef& session) {
    if (!enabled()) return;  // cheap early out on the WASAPI callback thread
    m_impl->m_worker.post([this, session] { m_impl->onRenderSessionActive(session); });
}

void PlaybackHold::onSessionPlaybackChanged(const std::wstring& appId, bool playing) {
    m_impl->m_worker.post([this, appId, playing] { m_impl->onSessionPlaybackChanged(appId, playing); });
}

void PlaybackHold::start(HoldKind kind, bool peerAsked, const std::wstring& appId) {
    // Mark active at once so a racing trigger sees the hold before the worker runs.
    m_active.store(true);
    m_impl->m_worker.post([this, kind, peerAsked, appId, holdAudio] { m_impl->start(kind, peerAsked, appId, holdAudio); });
}

void PlaybackHold::onPeerReleased()  { m_impl->m_worker.post([this] { m_impl->onPeerReleased(); }); }
void PlaybackHold::onConnected()     { m_impl->m_worker.post([this] { m_impl->onConnected(); }); }
void PlaybackHold::onConnectFailed() { m_impl->m_worker.post([this] { m_impl->onConnectFailed(); }); }
void PlaybackHold::onCallEnded()     { m_impl->m_worker.post([this] { m_impl->onCallEnded(); }); }

void PlaybackHold::onLost(const char* reason) {
    std::string why = reason ? reason : "";
    m_impl->m_worker.post([this, why] { m_impl->onLost(why.c_str()); });
}

bool PlaybackHold::suppressesMediaTrigger() const {
    return nowMs() < m_impl->m_suppressUntilMs.load();
}

void PlaybackHold::shutdown() {
    if (!m_impl) return;
    m_impl->m_worker.stop();
    m_active.store(false);
    m_impl->m_mute.restoreAll("quitting");
}

void PlaybackHold::retry()    { m_impl->m_worker.post([this] { m_impl->retry(); }); }
void PlaybackHold::playHere() { m_impl->m_worker.post([this] { m_impl->playHere(); }); }
void PlaybackHold::dismiss()  { m_impl->m_worker.post([this] { m_impl->dismiss(); }); }

}
