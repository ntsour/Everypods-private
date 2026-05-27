#pragma once

#include <functional>
#include <windows.h>

namespace Propods {

// Intercepts AirPods stem single-press (VK_MEDIA_PLAY_PAUSE) via a
// low-level keyboard hook (WH_KEYBOARD_LL).
//
// Must be installed on a thread that pumps messages — in practice,
// call start() before TrayIcon::runMessageLoop() so the hook fires
// on the same thread that runs the message loop.
//
// The callback runs on the main thread (inside the hook proc), so it
// must not block for long. MicMuter::toggle() is ~1 ms — safe.
//
// The press-through behavior (CallNextHookEx) is preserved, so
// play/pause still works in addition to mic mute.
class StemPressWatcher {
public:
    using Callback = std::function<void()>;

    StemPressWatcher() = default;
    ~StemPressWatcher() { stop(); }

    StemPressWatcher(const StemPressWatcher&) = delete;
    StemPressWatcher& operator=(const StemPressWatcher&) = delete;

    // Install the hook. Returns false if SetWindowsHookEx fails.
    bool start(Callback onSinglePress);
    void stop();

private:
    static LRESULT CALLBACK hookProc(int nCode, WPARAM wParam, LPARAM lParam);

    static StemPressWatcher* s_instance;
    HHOOK    m_hook{};
    Callback m_callback;
};

} // namespace Propods
