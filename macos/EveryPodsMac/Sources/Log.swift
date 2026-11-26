// Log.swift
//
// Tiny logger: timestamped lines to stdout and ~/Library/Logs/EveryPodsMac.log.

import Foundation

enum Log {
    private static let formatter: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss.SSS"
        return f
    }()
    private static let fileURL = FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("Logs/EveryPodsMac.log")
    private static let queue = DispatchQueue(label: "everypods.log")

    static func write(_ message: String) {
        let line = "[\(formatter.string(from: Date()))] \(message)\n"
        queue.async {
            print(line, terminator: "")
            fflush(stdout)
            guard let data = line.data(using: .utf8) else { return }
            if let handle = try? FileHandle(forWritingTo: fileURL) {
                handle.seekToEndOfFile()
                handle.write(data)
                try? handle.close()
            } else {
                try? data.write(to: fileURL)
            }
        }
    }
}

extension Log {
    /// Writes at once (for the last line before the app exits; `write` is asynchronous).
    static func writeNow(_ message: String) {
        queue.sync {}
        let line = "[\(formatter.string(from: Date()))] \(message)\n"
        guard let data = line.data(using: .utf8), let handle = try? FileHandle(forWritingTo: fileURL) else { return }
        handle.seekToEndOfFile()
        handle.write(data)
        try? handle.close()
    }

    /// Records why the app ends: an uncaught exception, or a fatal signal (a Swift trap is SIGTRAP), so a silent
    /// exit leaves a line in the log. The signal is then re-raised, so macOS still writes its crash report.
    static func installExitLogging() {
        crashLogPath = strdup(fileURL.path)
        NSSetUncaughtExceptionHandler { exception in
            Log.writeNow("Exit: uncaught exception \(exception.name.rawValue): \(exception.reason ?? "")")
        }
        for sig in [SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGABRT, SIGFPE, SIGTERM] { signal(sig, logSignalAndDie) }
    }
}

private var crashLogPath: UnsafeMutablePointer<CChar>?

/// Async-signal-safe: open, write and raise only.
private func logSignalAndDie(_ sig: Int32) {
    if let path = crashLogPath {
        let fd = open(path, O_WRONLY | O_APPEND)
        if fd >= 0 {
            var text: [UInt8] = Array("[exit] fatal signal ".utf8)
            var n = Int(sig), digits: [UInt8] = []
            repeat { digits.insert(UInt8(48 + n % 10), at: 0); n /= 10 } while n > 0
            text += digits + [10]
            _ = text.withUnsafeBufferPointer { Foundation.write(fd, $0.baseAddress, $0.count) }
            close(fd)
        }
    }
    signal(sig, SIG_DFL)
    raise(sig)
}

func hex(_ bytes: [UInt8]) -> String {
    bytes.map { String(format: "%02X", $0) }.joined(separator: " ")
}
