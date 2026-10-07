import APKFormats
import APKKit
import Foundation

/// One app in the library. Laid out for split sets from the start: base + splits + OBBs (M5).
public struct LibraryEntry: Sendable, Equatable, Codable, Identifiable {
    public enum State: Sendable, Equatable, Codable {
        case ready
        case needsSplits
        case incompatible
        /// Not a readable APK; the associated value says why.
        case invalid(String)
    }

    /// Package name, or "file:<name>" for files that could not be read as an APK.
    public var id: String
    public var displayName: String
    public var state: State
    public var report: APKReport?
    public var base: PackageFile?
    public var splits: [PackageFile]
    public var obbs: [PackageFile]
    /// The dropped file this entry was imported from.
    public var source: SourceFile
    /// Icon image, relative to Application Support.
    public var iconFile: String?
    /// When the app was first imported; an update keeps it, so the app keeps its place (ADR-0034).
    public var importedAt: Date
    /// When an import last replaced the installed version; nil until then.
    public var updatedAt: Date? = nil
    /// SHA-256 of the certificate the converted libraries are signed with (ADR-0023); nil while
    /// nothing is converted or signed.
    public var signedWith: String?
    /// The installed version's folder in apps/<package>/, the first 16 hex digits of its APK's SHA-256
    /// (ADR-0034), or of a split set's digest (ADR-0048, decision 6); nil for an entry from before version folders,
    /// whose files lie in apps/<package>/.
    public var folder: String? = nil
    /// The split APKs of a set that the choice left out, with why (ADR-0048, decision 4); nil for one APK.
    public var skippedSplits: [SkippedSplit]? = nil
    /// Why the set this `.invalid` entry stands for is not installed, in words to show: Android's where Android refuses
    /// it (ADR-0048, decisions 3 and 7). nil for any other entry, and for a file that is no APK, whose state holds an
    /// error's description.
    public var refusal: String? = nil
    /// What the import of this version left out of its container, with why (ADR-0049 c2): files an XAPK holds besides
    /// its APKs and OBBs, and OBBs Minato does not install. nil where nothing was left out, so an import without a
    /// container clears it.
    public var ignoredFiles: [IgnoredFile]? = nil
    /// The asset packs Play fetches after the install, fast-follow and on-demand, kept in the version folder's packs/
    /// under bundletool's names (ADR-0049 decision 2, format 6); nil where the version has none.
    public var packs: [PackageFile]? = nil
}

public struct PackageFile: Sendable, Equatable, Codable {
    /// How an APK takes part in its set (ADR-0048, decision 2).
    public enum Kind: String, Sendable, Codable {
        case base, feature, config, other
    }

    /// File name inside the app's install directory.
    public var fileName: String
    public var splitName: String?
    public var size: UInt64
    public var sha256: String
    /// nil for one APK, and in entries from before split sets.
    public var kind: Kind? = nil
    /// The module a configuration split configures, as its manifest's `configForSplit` names it.
    public var configForSplit: String? = nil
}

/// A split APK of a set that Minato does not install, and why (ADR-0048, decision 4).
public struct SkippedSplit: Sendable, Equatable, Codable {
    public var fileName: String
    public var splitName: String
    public var reason: String
}

/// Identity of a dropped file, used to recognise files that were already imported.
public struct SourceFile: Sendable, Hashable, Codable {
    public var fileName: String
    public var size: UInt64
    public var modifiedMilliseconds: Int64
    /// SHA-256 of the file, recorded for every APK since ADR-0034 (also one that keeps no base.apk);
    /// nil for an entry from before and for a file that was not a readable APK.
    public var sha256: String? = nil
}
