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
    public var importedAt: Date
    /// SHA-256 of the certificate the converted libraries are signed with (ADR-0023); nil while
    /// nothing is converted or signed.
    public var signedWith: String?
}

public struct PackageFile: Sendable, Equatable, Codable {
    /// File name inside the app's install directory.
    public var fileName: String
    public var splitName: String?
    public var size: UInt64
    public var sha256: String
}

/// Identity of a dropped file, used to recognise files that were already imported.
public struct SourceFile: Sendable, Hashable, Codable {
    public var fileName: String
    public var size: UInt64
    public var modifiedMilliseconds: Int64
}
