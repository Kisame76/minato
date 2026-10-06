import APKKit
import Foundation

/// An OBB's name as Android's APK expansion files have it: `[main|patch].<expansion-version>.<package>.obb`
/// (developer.android.com, "APK expansion files"), which an app builds itself to find its file.
public struct OBBName: Sendable, Hashable {
    public enum Kind: String, Sendable {
        case main, patch
    }

    public var kind: Kind
    /// The expansion version, the versionCode of the APK the file first came with, in ASCII digits as written.
    public var version: String
    public var package: String

    public init(kind: Kind, version: String, package: String) {
        self.kind = kind
        self.version = version
        self.package = package
    }

    /// `name` read as such a name: a plain file name, its version ASCII digits, its package one Android takes
    /// (`FrameworkParsingPackageUtils.validateName` with a separator, as a file name). nil for any other.
    public init?(_ name: String) {
        guard Container.isPlainFileName(name), name.hasSuffix(".obb") else { return nil }
        let parts = name.dropLast(4).split(separator: ".", maxSplits: 2, omittingEmptySubsequences: false)
        guard parts.count == 3, let kind = Kind(rawValue: String(parts[0])), !parts[1].isEmpty,
              parts[1].unicodeScalars.allSatisfy({ ("0"..."9").contains($0) }),
              InstallerReading.validateName(String(parts[2]), requireSeparator: true, requireFilename: true) == nil
        else { return nil }
        self.init(kind: kind, version: String(parts[1]), package: String(parts[2]))
    }

    public var fileName: String { "\(kind.rawValue).\(version).\(package).obb" }
}
