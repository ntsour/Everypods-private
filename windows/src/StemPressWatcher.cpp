#include "StemPressWatcher.hpp"
#include "Logger.hpp"

namespace Propods {

StemPressWatcher* StemPressWatcher::s_instance = nullptr;

bool StemPressWatcher::start(Callback onSinglePress) {
    m_callback = std::move(onSinglePress);
    s_instance = this;

    // WH_KEYBOARD_LL is a global hook that does NOT require a DLL — the
    // callback runs on the installing thread (main thread here), which
    // must pump messages for the hook to fire.
    m_hook = SetWindowsHookExW(WH_KEYBOARD_LL, &StemPressWatcher::hookProc, nullptr, 0);
    if (!m_hook) {
        log::error("StemPressWatcher: SetWindowsHookEx failed ({})", GetLastError());
        return false;
    }
    log::info("StemPressWatcher: armed — listening for VK_MEDIA_PLAY_PAUSE (AirPods single press)");
    return true;
}

void StemPressWatcher::stop() {
    if (m_hook) {
        UnhookWindowsHookEx(m_hook);
        m_hook = nullptr;
        log::info("StemPressWatcher: hook removed");
    }
    if (s_instance == this) s_instance = nullptr;
}

// Static hook callback — called on the main thread during message pumping.
LRESULT CALLBACK StemPressWatcher::hookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && wParam == WM_KEYDOWN) {
        const auto* kbd = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if (kbd->vkCode == VK_MEDIA_PLAY_PAUSE) {
            log::info("StemPressWatcher: AirPods single press detected");
            if (s_instance && s_instance->m_callback) {
                s_instance->m_callback();
            }
            // Fall through to CallNextHookEx so the system still handles
            // play/pause (media keeps responding as usual).
        }
    }
    // Always pass the event to the next hook — never swallow.
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

} // namespace Propods
