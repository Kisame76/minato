import APKLibrary
import Foundation

/// What makes the next launch game mode (ADR-0032, decision 1): the app to start, and when it was asked for; or compile
/// mode, which compiles the app's code (ADR-0053, M6 c), a guest start of its own since ART allows one runtime per
/// process.
public struct LaunchRequest: Codable, Equatable, Sendable {
    public enum Kind: String, Codable, Sendable {
        case run
        case compile
    }

    public var package: String
    public var time: Date
    public var kind: Kind
    /// A notification's checked content intent (NotificationTap.startIntent; ADR-0074 decision 4), which the app starts
    /// with in place of its launcher; nil for a plain start.
    public var startIntent: String?

    public init(package: String, time: Date, kind: Kind = .run, startIntent: String? = nil) {
        self.package = package
        self.time = time
        self.kind = kind
        self.startIntent = startIntent
    }

    /// A request an earlier build wrote has no kind: it runs the app.
    public init(from decoder: any Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        package = try container.decode(String.self, forKey: .package)
        time = try container.decode(Date.self, forKey: .time)
        kind = try container.decodeIfPresent(Kind.self, forKey: .kind) ?? .run
        startIntent = try container.decodeIfPresent(String.self, forKey: .startIntent)
    }
}

/// How game mode's session ended. Only a clean end asks for compile mode (ADR-0053, M6 c, E5): a crash's or a kill's
/// notice comes at the library's next start, never after an "Optimizing" screen.
public enum SessionEnd: Equatable, Sendable {
    /// The activities ended: the user's Quit, or the app's own finish.
    case lifecycleEnded
    /// The program ended with this status, after onDestroy or by itself.
    case exited(Int32)
    /// The session's channel broke.
    case channelBroke
    /// The app could not start.
    case startFailed

    public var asksForCompile: Bool {
        switch self {
        case .lifecycleEnded: true
        case .exited(let status): status == 0
        case .channelBroke, .startFailed: false
        }
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
    /// Compiles the app's Java code (ADR-0053, M6 c), then back to the library.
    case compileMode(LibraryEntry)

    /// Game mode for a request that names a ready app, a NativeActivity app or one with Java code (ADR-0039 c5),
    /// compile mode for a compile request of a ready app with Java code; the library otherwise, with the reason when
    /// the request's app is gone or not ready.
    public init(request: LaunchRequest?, entries: [LibraryEntry]) {
        guard let request else {
            self = .library(nil)
            return
        }
        guard let entry = entries.first(where: { $0.id == request.package }) else {
            self = .library(.appNotFound(request.package))
            return
        }
        guard entry.state == .ready, entry.report != nil else {
            self = .library(.appNotReady(entry))
            return
        }
        switch request.kind {
        case .run:
            self = .gameMode(entry)
        case .compile:
            // A NativeActivity app on activity-host has no Java code that ART would run compiled.
            self = entry.report?.startsOnJavaPath == true ? .compileMode(entry) : .library(nil)
        }
    }
}
