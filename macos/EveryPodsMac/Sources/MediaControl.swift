// MediaControl.swift
//
// Pauses playback by posting the system play/pause media key. The key goes to whichever app owns
// the system media session. Needs the Accessibility permission for this app.

import AppKit
import ApplicationServices

enum MediaControl {
    static var hasPermission: Bool { AXIsProcessTrusted() }

    /// Shows the system prompt that sends the user to Privacy & Security > Accessibility.
    static func requestPermission() {
        let key = kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String
        _ = AXIsProcessTrustedWithOptions([key: true] as CFDictionary)
    }

    /// Posts play/pause. Only call this while something is playing, or it will start playback.
    @discardableResult
    static func togglePlayPause() -> Bool {
        guard hasPermission else {
            Log.write("Media: Accessibility permission missing; cannot pause")
            return false
        }
        post(keyDown: true)
        post(keyDown: false)
        Log.write("Media: sent play/pause key")
        return true
    }

    private static func post(keyDown: Bool) {
        let keyPlay = 16  // NX_KEYTYPE_PLAY
        let flags: UInt = keyDown ? 0xA00 : 0xB00
        let data1 = (keyPlay << 16) | (Int(flags) & 0xFF00)
        NSEvent.otherEvent(with: .systemDefined, location: .zero,
                           modifierFlags: NSEvent.ModifierFlags(rawValue: flags), timestamp: 0,
                           windowNumber: 0, context: nil, subtype: 8, data1: data1, data2: -1)?
            .cgEvent?.post(tap: .cghidEventTap)
    }
}
