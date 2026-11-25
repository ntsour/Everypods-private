// PlaybackLevelProbe.swift
//
// After this Mac gives the AirPods to a phone it pauses the player, but a paused browser keeps its audio
// output stream open for up to a minute (seen with Chrome on 2026-11-25: ~40 s). Core Audio still reports that
// process as "running output", so MediaWatcher sees no new start when the user presses play again, and the
// AirPods stay on the phone until the stream finally closes.
//
// The probe listens to the samples of the processes that were playing, through a Core Audio process tap
// (macOS 14.2+): silence followed by sound means the player resumed. A player the pause key did not stop
// stays loud and never fires.
//
// The tap needs the "System Audio Recording Only" permission (NSAudioCaptureUsageDescription). Without it
// Core Audio delivers silence, the probe never fires, and the Mac behaves as before.

import CoreAudio
import Foundation

/// Pure decision logic: fires once on sound that follows enough silence.
struct ResumeDetector {
    /// Peak below this is silence (about -66 dBFS); above `loudPeak` (about -50 dBFS) is playback.
    static let silentPeak: Float = 0.0005
    static let loudPeak: Float = 0.003
    /// The paused stream must be silent this long before sound counts as a resume.
    static let silenceNeeded: TimeInterval = 0.5
    /// Sound must last this long (a click or a notification blip is shorter).
    static let soundNeeded: TimeInterval = 0.25

    private var silentSince: TimeInterval?
    private var armed = false
    private var loudSince: TimeInterval?
    private(set) var fired = false

    /// Feeds one buffer's peak at time `now` (seconds). Returns true exactly once, when playback resumes.
    mutating func feed(peak: Float, at now: TimeInterval) -> Bool {
        if fired { return false }
        if peak < Self.silentPeak {
            loudSince = nil
            if silentSince == nil { silentSince = now }
            if let since = silentSince, now - since >= Self.silenceNeeded { armed = true }
            return false
        }
        silentSince = nil
        guard armed, peak >= Self.loudPeak else {
            if peak >= Self.loudPeak { loudSince = nil }  // loud before any silence: the pause did not take
            return false
        }
        if loudSince == nil { loudSince = now }
        if let since = loudSince, now - since >= Self.soundNeeded {
            fired = true
            return true
        }
        return false
    }
}

@available(macOS 14.2, *)
final class PlaybackLevelProbe {
    private var tapID = AudioObjectID(kAudioObjectUnknown)
    private var aggregateID = AudioObjectID(kAudioObjectUnknown)
    private var procID: AudioDeviceIOProcID?
    private let queue = DispatchQueue(label: "everypods.levelprobe", qos: .userInitiated)
    /// Setup and teardown run here, in order: creating the tap can block for as long as the permission
    /// prompt is up, which must not hold the main thread (it delayed a release by 6.7 s).
    private let setup = DispatchQueue(label: "everypods.levelprobe.setup", qos: .userInitiated)
    private var stopped = false
    private var detector = ResumeDetector()
    private var heardAnything = false

    /// Starts listening to `processes` (Core Audio process objects) off the main thread. `onResume` runs once on
    /// the main queue; `completion` reports on the main queue whether the tap is running.
    func start(processes: [AudioObjectID], onResume: @escaping () -> Void, completion: @escaping (Bool) -> Void) {
        setup.async {
            let ok = !self.stopped && self.setUp(processes: processes, onResume: onResume)
            DispatchQueue.main.async { completion(ok) }
        }
    }

    private func setUp(processes: [AudioObjectID], onResume: @escaping () -> Void) -> Bool {
        guard !processes.isEmpty else { return false }
        let description = CATapDescription(stereoMixdownOfProcesses: processes)
        description.uuid = UUID()
        description.muteBehavior = .unmuted
        description.isPrivate = true
        description.name = "EveryPods playback probe"
        var tap = AudioObjectID(kAudioObjectUnknown)
        var status = AudioHardwareCreateProcessTap(description, &tap)
        guard status == noErr else {
            Log.write("Probe: process tap failed (\(status))")
            return false
        }
        tapID = tap

        var aggregate: [String: Any] = [
            kAudioAggregateDeviceNameKey: "EveryPods playback probe",
            kAudioAggregateDeviceUIDKey: UUID().uuidString,
            kAudioAggregateDeviceIsPrivateKey: true,
            kAudioAggregateDeviceIsStackedKey: false,
            kAudioAggregateDeviceTapAutoStartKey: true,
            kAudioAggregateDeviceTapListKey: [[
                kAudioSubTapUIDKey: description.uuid.uuidString,
                kAudioSubTapDriftCompensationKey: true,
            ]],
        ]
        if let output = Self.defaultOutputUID() {
            aggregate[kAudioAggregateDeviceMainSubDeviceKey] = output
            aggregate[kAudioAggregateDeviceSubDeviceListKey] = [[kAudioSubDeviceUIDKey: output]]
        }
        var device = AudioObjectID(kAudioObjectUnknown)
        status = AudioHardwareCreateAggregateDevice(aggregate as CFDictionary, &device)
        guard status == noErr else {
            Log.write("Probe: aggregate device failed (\(status))")
            tearDown()
            return false
        }
        aggregateID = device

        let started = ProcessInfo.processInfo.systemUptime
        status = AudioDeviceCreateIOProcIDWithBlock(&procID, device, queue) { [weak self] _, input, _, _, _ in
            guard let self else { return }
            var peak: Float = 0
            for buffer in UnsafeMutableAudioBufferListPointer(UnsafeMutablePointer(mutating: input)) {
                guard let data = buffer.mData else { continue }
                let samples = data.assumingMemoryBound(to: Float.self)
                for i in 0..<Int(buffer.mDataByteSize) / MemoryLayout<Float>.size { peak = max(peak, abs(samples[i])) }
            }
            if peak > 0 { self.heardAnything = true }
            let now = ProcessInfo.processInfo.systemUptime - started
            if self.detector.feed(peak: peak, at: now) { DispatchQueue.main.async(execute: onResume) }
        }
        guard status == noErr, let procID else {
            Log.write("Probe: IO proc failed (\(status))")
            tearDown()
            return false
        }
        status = AudioDeviceStart(device, procID)
        guard status == noErr else {
            Log.write("Probe: start failed (\(status))")
            tearDown()
            return false
        }
        return true
    }

    /// True once any non-zero sample arrived (false all along usually means the permission is missing).
    var receivedAudio: Bool { queue.sync { heardAnything } }

    func stop() {
        setup.async {
            self.stopped = true
            self.tearDown()
        }
    }

    private func tearDown() {
        if aggregateID != kAudioObjectUnknown {
            if let procID {
                AudioDeviceStop(aggregateID, procID)
                AudioDeviceDestroyIOProcID(aggregateID, procID)
            }
            AudioHardwareDestroyAggregateDevice(aggregateID)
        }
        if tapID != kAudioObjectUnknown { AudioHardwareDestroyProcessTap(tapID) }
        procID = nil
        aggregateID = AudioObjectID(kAudioObjectUnknown)
        tapID = AudioObjectID(kAudioObjectUnknown)
    }


    private static func defaultOutputUID() -> String? {
        var address = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDefaultOutputDevice,
                                                 mScope: kAudioObjectPropertyScopeGlobal,
                                                 mElement: kAudioObjectPropertyElementMain)
        var device = AudioObjectID(kAudioObjectUnknown)
        var size = UInt32(MemoryLayout<AudioObjectID>.size)
        guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &device) == noErr
        else { return nil }
        address.mSelector = kAudioDevicePropertyDeviceUID
        var uid: Unmanaged<CFString>?
        size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        guard AudioObjectGetPropertyData(device, &address, 0, nil, &size, &uid) == noErr, let uid else { return nil }
        return uid.takeRetainedValue() as String
    }
}
