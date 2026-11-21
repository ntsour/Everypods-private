// media_probe.swift: what can a Mac app learn about playback using only public CoreAudio?
// Prints once a second: output devices that are running, and processes with active audio output/input.
// Build: swiftc -O -framework CoreAudio -framework Foundation -framework AppKit media_probe.swift -o media_probe

import AppKit
import CoreAudio
import Foundation

func property<T>(_ object: AudioObjectID, _ selector: AudioObjectPropertySelector,
                 scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal, default value: T) -> T {
    var address = AudioObjectPropertyAddress(mSelector: selector, mScope: scope, mElement: kAudioObjectPropertyElementMain)
    var result = value
    var size = UInt32(MemoryLayout<T>.size)
    return AudioObjectGetPropertyData(object, &address, 0, nil, &size, &result) == noErr ? result : value
}

func objectList(_ object: AudioObjectID, _ selector: AudioObjectPropertySelector) -> [AudioObjectID] {
    var address = AudioObjectPropertyAddress(mSelector: selector, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
    var size: UInt32 = 0
    guard AudioObjectGetPropertyDataSize(object, &address, 0, nil, &size) == noErr, size > 0 else { return [] }
    var ids = [AudioObjectID](repeating: 0, count: Int(size) / MemoryLayout<AudioObjectID>.size)
    guard AudioObjectGetPropertyData(object, &address, 0, nil, &size, &ids) == noErr else { return [] }
    return ids
}

func string(_ object: AudioObjectID, _ selector: AudioObjectPropertySelector) -> String {
    var address = AudioObjectPropertyAddress(mSelector: selector, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
    var value: Unmanaged<CFString>?
    var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
    guard AudioObjectGetPropertyData(object, &address, 0, nil, &size, &value) == noErr, let value else { return "?" }
    return value.takeRetainedValue() as String
}

func hasOutput(_ device: AudioObjectID) -> Bool {
    var address = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyStreams, mScope: kAudioObjectPropertyScopeOutput, mElement: kAudioObjectPropertyElementMain)
    var size: UInt32 = 0
    return AudioObjectGetPropertyDataSize(device, &address, 0, nil, &size) == noErr && size > 0
}

var last = ""
print("Polling every second. Ctrl-C to stop.")
while true {
    var lines: [String] = []
    let defaultOut: AudioObjectID = property(AudioObjectID(kAudioObjectSystemObject), kAudioHardwarePropertyDefaultOutputDevice, default: 0)
    lines.append("default output: \(string(defaultOut, kAudioObjectPropertyName))")
    for device in objectList(AudioObjectID(kAudioObjectSystemObject), kAudioHardwarePropertyDevices) where hasOutput(device) {
        let running: UInt32 = property(device, kAudioDevicePropertyDeviceIsRunningSomewhere, default: 0)
        lines.append("  out \(string(device, kAudioObjectPropertyName)): running=\(running)")
    }
    for process in objectList(AudioObjectID(kAudioObjectSystemObject), kAudioHardwarePropertyProcessObjectList) {
        let out: UInt32 = property(process, kAudioProcessPropertyIsRunningOutput, default: 0)
        let input: UInt32 = property(process, kAudioProcessPropertyIsRunningInput, default: 0)
        guard out != 0 || input != 0 else { continue }
        let bundle = string(process, kAudioProcessPropertyBundleID)
        lines.append("  proc \(bundle): output=\(out) input=\(input)")
    }
    let text = lines.joined(separator: "\n")
    if text != last {
        let f = DateFormatter(); f.dateFormat = "HH:mm:ss"
        print("[\(f.string(from: Date()))]\n\(text)")
        fflush(stdout)
        last = text
    }
    Thread.sleep(forTimeInterval: 1)
}
