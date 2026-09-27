import APKLibrary
import Foundation

/// What makes the next launch game mode (ADR-0032, decision 1): the app to start, and when it was asked for.
public struct LaunchRequest: Codable, Equatable, Sendable {
    public var package: String
    public var time: Date

    public init(package: String, time: Date) {
        self.package = package
        self.time = time
    }
}

/// The request as a file in Application Support: written atomically, read once.
public struct LaunchRequestStore: Sendable {
    public let file: URL

    public init(folder: URL) {
        file = folder.appendingPathComponent("launch-request.json")
    }

    /// Writes the request to a temporary file and renames it, so it is complete on disk before the
    /// process ends for the restart.
    public func write(_ request: LaunchRequest) throws {
        try FileManager.default.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
        try Self.encoder.encode(request).write(to: file, options: .atomic)
    }

    /// The request, removed before anything else: a crash in game mode never leads into game mode again.
    /// Nil without one, or when it cannot be read or removed.
    public func take() -> LaunchRequest? {
        guard let data = try? Data(contentsOf: file), (try? FileManager.default.removeItem(at: file)) != nil else { return nil }
        return try? Self.decoder.decode(LaunchRequest.self, from: data)
    }

    // Milliseconds: the time from the tap to game mode's process is about half a second.
    private static let encoder: JSONEncoder = {
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .millisecondsSince1970
        return encoder
    }()

    private static let decoder: JSONDecoder = {
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .millisecondsSince1970
        return decoder
    }()
}

/// How the process runs: the library, or game mode for one app (ADR-0032, decision 1).
public enum LaunchMode: Equatable, Sendable {
    /// Why a request led to the library instead, or how game mode's last session went wrong.
    public enum Problem: Equatable, Sendable {
        case appNotFound(String)
        case appNotReady(LibraryEntry)
        /// Only without a request (ADR-0032, decision 8).
        case quitUnexpectedly(LastSessionNotice)
    }

    case library(Problem?)
    case gameMode(LibraryEntry)

    /// Game mode for a request that names a ready NativeActivity app; the library otherwise, with the
    /// reason when there was a request.
    public init(request: LaunchRequest?, entries: [LibraryEntry]) {
        guard let request else {
            self = .library(nil)
            return
        }
        guard let entry = entries.first(where: { $0.id == request.package }) else {
            self = .library(.appNotFound(request.package))
            return
        }
        guard entry.state == .ready, case .nativeActivity = entry.report?.runtime else {
            self = .library(.appNotReady(entry))
            return
        }
        self = .gameMode(entry)
    }
}
