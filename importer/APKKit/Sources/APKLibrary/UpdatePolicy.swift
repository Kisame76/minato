import APKKit
import Foundation

/// What the update policy compares of a version, installed or about to be imported (ADR-0034).
public struct AppVersion: Sendable, Equatable {
    public var state: LibraryEntry.State
    public var versionCode: Int64
    /// SHA-256 of the APK; nil for an installed version from before it was recorded that keeps no copy.
    public var sha256: String?

    public init(state: LibraryEntry.State, versionCode: Int64, sha256: String?) {
        self.state = state
        self.versionCode = versionCode
        self.sha256 = sha256
    }

    /// The version an entry holds; nil for a file that was not a readable APK.
    public init?(entry: LibraryEntry) {
        guard let report = entry.report else { return nil }
        self.init(state: entry.state, versionCode: report.versionCode, sha256: entry.source.sha256 ?? entry.base?.sha256)
    }
}

/// Why the library asks before an import (ADR-0034, decision 1).
public enum UpdateWarning: Sendable, Hashable {
    /// The file is an older version, which may not be able to read the newer one's data.
    case olderVersion
}

/// What importing a file does to the installed app of the same package (ADR-0034, decision 1).
public enum UpdateDecision: Sendable, Equatable {
    /// No app of this package is installed yet.
    case install
    /// A newer version replaces the installed one, which keeps its data and its place.
    case update
    /// The same version with other bytes is installed again, as Android does.
    case reinstall
    /// The same bytes: nothing changes, and the file is discarded.
    case unchanged
    /// The installed version works and the file, in this state, would not: nothing changes.
    case reject(LibraryEntry.State)
    /// Nothing changes until the user answers.
    case ask(Set<UpdateWarning>)
}

public enum UpdatePolicy {
    /// Step a) decides by the versions and the bytes; the signer comes with step b).
    public static func decide(installed: AppVersion?, new: AppVersion) -> UpdateDecision {
        guard let installed else { return .install }
        // An unusable file never replaces a working version, whatever its version.
        if installed.state == .ready, new.state == .incompatible || new.state == .needsSplits {
            return .reject(new.state)
        }
        if new.versionCode > installed.versionCode { return .update }
        if new.versionCode < installed.versionCode { return .ask([.olderVersion]) }
        if let bytes = installed.sha256, bytes == new.sha256 { return .unchanged }
        return .reinstall
    }
}

/// What `Library.importFile` did.
public struct ImportResult: Sendable, Equatable {
    /// The package's entry after the import: the new one, or the installed one that stays.
    public var entry: LibraryEntry
    public var decision: UpdateDecision
    /// The imported file's report; nil for a file that is not a readable APK.
    public var report: APKReport?
}
