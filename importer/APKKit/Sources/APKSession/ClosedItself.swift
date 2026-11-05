import Foundation

/// Apps that closed themselves soon after they opened, by package, in `closed-itself.json` in Library/Application
/// Support: Get Info says so in its footer until the app's next start, and Report a Problem names it. Neither Android
/// nor iOS shows a word when an app ends itself, and the library coming back at once looks like a crash (the lead,
/// 2026-11-04). No alert and no badge: apps with their own Quit end themselves too.
public struct ClosedItselfStore: Sendable {
    /// How soon after the start an end counts.
    public static let window: Double = 10

    public let folder: URL

    public init(folder: URL) {
        self.folder = folder
    }

    var file: URL { folder.appending(path: "closed-itself.json") }

    /// The whole seconds to show, at least 1, for the session's `end` `seconds` after its start; nil for an end the
    /// user asked for (`byUser`, the menu's Quit), a later one, and one with a notice or reason of its own: an exit
    /// code from 1 to 128 (LastSessionStore), a failed start, a broken channel.
    public static func seconds(after end: SessionEnd, byUser: Bool, seconds: Double) -> Int? {
        guard !byUser, seconds >= 0, seconds <= window else { return nil }
        switch end {
        case .lifecycleEnded: break
        // 0, or a kill of itself's 137 through the end handler (ADR-0028, decision 5).
        case .exited(let status) where !(1...128).contains(status): break
        case .exited, .channelBroke, .startFailed: return nil
        }
        return max(1, Int(seconds.rounded()))
    }

    /// The seconds of the app `package`'s last start, nil when it did not close itself then.
    public func seconds(of package: String) -> Int? {
        read()[package]
    }

    public func record(package: String, seconds: Int) {
        var records = read()
        records[package] = seconds
        write(records)
    }

    /// At the app's next start, and when its data is cleared or it is deleted.
    public func forget(package: String) {
        var records = read()
        guard records.removeValue(forKey: package) != nil else { return }
        write(records)
    }

    private func read() -> [String: Int] {
        (try? Data(contentsOf: file)).flatMap { try? JSONDecoder().decode([String: Int].self, from: $0) } ?? [:]
    }

    private func write(_ records: [String: Int]) {
        guard !records.isEmpty else {
            try? FileManager.default.removeItem(at: file)
            return
        }
        try? JSONEncoder().encode(records).write(to: file, options: .atomic)
    }
}
