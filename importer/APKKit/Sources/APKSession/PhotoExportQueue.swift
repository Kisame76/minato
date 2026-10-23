import Foundation
import Synchronization
import os

private let log = Logger(subsystem: "io.github.kisame76.minato", category: "photos")

/// The exports to Photos after the take (ADR-0057 step e): each job is copied from its descriptor into a folder of the
/// host's own on a queue of its own, off the guest's thread and Swift's cooperative threads (a copy that is not a clone
/// reads up to 1 GiB), then handed to Photos one at a time (`add`, the app's PhotoKit), and its copy goes once Photos
/// added or refused it. Once Photos says the user declined, `declined` stays true, and the host takes nothing more.
public final class PhotoExportQueue: Sendable {
    /// What Photos did with a copy.
    public enum Outcome: Sendable {
        case added
        /// Not a picture or video Photos takes: the kind is the app's word only.
        case refused
        /// The user declined Minato's access (or a profile restricts it).
        case declined
    }

    public typealias Add = @Sendable (MediaExportJob.Kind, URL, String) async -> Outcome

    private struct Waiting {
        let kind: MediaExportJob.Kind
        let copy: URL
        let name: String
    }

    private struct State {
        var waiting: [Waiting] = []
        var adding = false
        var declined: Bool
        /// Taken and not yet added or refused.
        var pending = 0
    }

    private static let copying = DispatchQueue(label: "io.github.kisame76.minato.photo-exports", qos: .utility)

    private let folder: URL
    private let add: Add
    private let state: Mutex<State>

    /// `folder` is the host's for the copies, made when missing and never emptied here: SwiftUI may make a session's
    /// state more than once, and a second queue must not empty it under the first (removeEarlierCopies does, at the
    /// launch). `declined` is the access iOS reports at the start.
    public init(folder: URL, declined: Bool, add: @escaping Add) {
        self.folder = folder
        self.add = add
        state = Mutex(State(declined: declined))
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
    }

    public var declined: Bool { state.withLock { $0.declined } }

    /// From the request handler: the guest's thread returns at once; the job's descriptor is closed once the copy is
    /// made.
    public func export(_ job: MediaExportJob) {
        state.withLock { $0.pending += 1 }
        Self.copying.async { [self] in
            guard let copy = job.copy(into: folder) else {
                log.error("export: the copy failed")
                state.withLock { $0.pending -= 1 }
                return
            }
            let next = Waiting(kind: job.kind, copy: copy, name: job.shownName)
            Task { await self.enqueue(next) }
        }
    }

    /// Waits up to `seconds` for the exports under way, before the session ends the process (apk-run's model waits
    /// 5 s too): a photo saved just before the app finishes still reaches Photos.
    public func settle(within seconds: Double) async {
        let clock = ContinuousClock()
        let deadline = clock.now + .seconds(seconds)
        while state.withLock({ $0.pending }) > 0 && clock.now < deadline {
            try? await Task.sleep(for: .milliseconds(50))
        }
    }

    /// The copies an earlier process left (a kill, or a settle that ran out): the folder is renamed at once, so that
    /// this process's copies never meet it, and removed in the background, with any such folder an earlier removal
    /// left.
    public static func removeEarlierCopies(_ folder: URL) {
        let parent = folder.deletingLastPathComponent()
        let prefix = folder.lastPathComponent + "-earlier-"
        try? FileManager.default.moveItem(at: folder, to: parent.appending(path: prefix + UUID().uuidString))
        DispatchQueue.global(qos: .utility).async {
            for name in (try? FileManager.default.contentsOfDirectory(atPath: parent.path)) ?? []
            where name.hasPrefix(prefix) {
                try? FileManager.default.removeItem(at: parent.appending(path: name))
            }
        }
    }

    /// Adds the copy after those before it; the first caller adds them all, until none waits.
    private func enqueue(_ export: Waiting) async {
        let first = state.withLock { state in
            state.waiting.append(export)
            defer { state.adding = true }
            return !state.adding
        }
        guard first else { return }
        while true {
            let next = state.withLock { state -> Waiting? in
                guard !state.waiting.isEmpty else {
                    state.adding = false
                    return nil
                }
                return state.waiting.removeFirst()
            }
            guard let next else { return }
            let outcome = declined ? .declined : await add(next.kind, next.copy, next.name)
            try? FileManager.default.removeItem(at: next.copy.deletingLastPathComponent())
            state.withLock { state in
                if outcome == .declined { state.declined = true }
                state.pending -= 1
            }
        }
    }
}
