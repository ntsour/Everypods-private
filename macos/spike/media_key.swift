// media_key.swift: send the system play/pause media key, and report whether the AirPods output
// stops running afterwards. Needs the calling app (Terminal) to have Accessibility permission.
// Build: swiftc -O -framework AppKit -framework ApplicationServices -framework CoreAudio media_key.swift -o media_key

import AppKit
import ApplicationServices
import CoreAudio

func airPodsRunning() -> Bool {
    var address = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDefaultOutputDevice,
        mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
    var device = AudioObjectID(0)
    var size = UInt32(MemoryLayout<AudioObjectID>.size)
    guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &device) == noErr else { return false }
    var running: UInt32 = 0
    size = UInt32(MemoryLayout<UInt32>.size)
    address.mSelector = kAudioDevicePropertyDeviceIsRunningSomewhere
    _ = AudioObjectGetPropertyData(device, &address, 0, nil, &size, &running)
    return running != 0
}

func post(keyDown: Bool) {
    let NX_KEYTYPE_PLAY = 16
    let flags: UInt = keyDown ? 0xA00 : 0xB00
    let data1 = (NX_KEYTYPE_PLAY << 16) | (Int(flags) & 0xFF00)
    let event = NSEvent.otherEvent(with: .systemDefined, location: .zero, modifierFlags: NSEvent.ModifierFlags(rawValue: flags),
        timestamp: 0, windowNumber: 0, context: nil, subtype: 8, data1: data1, data2: -1)
    event?.cgEvent?.post(tap: .cghidEventTap)
}

func now() -> String { let f = DateFormatter(); f.dateFormat = "HH:mm:ss.SSS"; return f.string(from: Date()) }

let trusted = AXIsProcessTrusted()
print("[\(now())] Accessibility trusted: \(trusted)")
print("[\(now())] before: output running = \(airPodsRunning())")
post(keyDown: true)
post(keyDown: false)
print("[\(now())] sent play/pause key")
for second in 1...12 {
    Thread.sleep(forTimeInterval: 1)
    print("[\(now())] +\(second)s: output running = \(airPodsRunning())")
}
