import APKKit
import Foundation

/// What a picked file holds, told by its content, never by its extension (ADR-0049 decision 1).
public enum ContainerKind: Sendable, Equatable {
    /// `AndroidManifest.xml` at the root: one APK, which the import takes as it is.
    case apk
    /// `manifest.json` at the root: an XAPK (ADR-0049 step c).
    case xapk
    /// `info.json` at the root with `apkm_version` or `pname`: a plain APKM (step e).
    case apkm
    /// `toc.pb` or `toc.json` at the root: bundletool's `.apks`, chosen from its toc (step a2).
    case bundletool
    /// Anything else: SAI's or App Manager's `.apks` or a plain ZIP, whose APKs make a loose set (decision 3).
    case looseSet
}

/// Why a container is refused, with a reason for the app to show after "could not be added:".
public enum ContainerError: Error, Sendable, Equatable, LocalizedError {
    /// None of a ZIP's signatures in its first 4 bytes. `encryptedAPKM` when its extension is `.apkm`: an encrypted
    /// APKM is no ZIP (`UnApkm.java:190-198`).
    case notAZip(encryptedAPKM: Bool)
    /// The ZIP reader refused the archive or an entry, as libziparchive does.
    case unreadable(APKError)
    /// An entry with the ZIP encryption flag; Minato never decrypts.
    case encrypted(String)
    /// Over one of decision 7's caps.
    case overCap(String)
    /// A loose set without an APK.
    case noAPK
    /// An entry whose content does not match its CRC-32.
    case checksumMismatch(String)
    /// Less free space than the declared sizes and a tenth more.
    case notEnoughSpace(needed: UInt64, available: UInt64)
    /// A file name of ours that is no plain file name, or one given twice.
    case unsafeName(String)
    /// The folder to extract into exists already.
    case destinationExists

    public var errorDescription: String? {
        switch self {
        case .notAZip(let encryptedAPKM): encryptedAPKM ? "Encrypted APKM files aren’t supported" : "it is no ZIP archive"
        case .unreadable(let error): "it is damaged (\(error))"
        case .encrypted(let name): "“\(name)” in it is encrypted"
        case .overCap(let reason): reason
        case .noAPK: "it holds no APK"
        case .checksumMismatch(let name): "“\(name)” in it is damaged: its checksum does not match"
        case .notEnoughSpace(let needed, let available):
            "it needs \(Self.bytes(needed)) of free space, and \(Self.bytes(available)) are free"
        case .unsafeName(let name): "“\(name)” is no name for a file Minato takes out of it"
        case .destinationExists: "the folder for its files exists already"
        }
    }

    private static func bytes(_ count: UInt64) -> String {
        ByteCountFormatter.string(fromByteCount: Int64(clamping: count), countStyle: .file)
    }
}

/// A picked file opened as a ZIP, with its kind (ADR-0049).
public struct Container {
    /// [guess] Decision 7's caps, to tune with real files. A container's entries count once its kind is known: an
    /// APK keeps the reader's own cap (`ZipArchive.defaultMaxEntries`).
    public static let maxEntries = 4_096
    public static let maxAPKs = 512
    public static let maxJSONSize: UInt64 = 1 << 20

    /// `PK\3\4`, `PK\5\6` and `PK\7\8`, the first 4 bytes of a ZIP as InstallerX and App Manager tell one.
    static let zipSignatures: Set<Data> = [Data([0x50, 0x4B, 0x03, 0x04]), Data([0x50, 0x4B, 0x05, 0x06]),
                                           Data([0x50, 0x4B, 0x07, 0x08])]

    public let kind: ContainerKind
    public let archive: ZipArchive

    /// Opens `url` and tells its kind by its content, in InstallerX's order (`FileTypeDetector.kt:192-279`): no ZIP
    /// is refused, then the root's `manifest.json`, `info.json` with APKM's keys, `AndroidManifest.xml` and a toc
    /// decide, and anything else is a loose set. A container with an encrypted entry or over its entries is refused.
    public static func open(_ url: URL) throws -> Container {
        let source = try FileSource(url: url)
        let head = source.size >= 4 ? try source.read(at: 0, count: 4) : Data()
        guard zipSignatures.contains(head) else {
            throw ContainerError.notAZip(encryptedAPKM: url.pathExtension.lowercased() == "apkm")
        }
        let archive: ZipArchive
        do {
            archive = try ZipArchive(source: source)
        } catch let error as APKError {
            throw ContainerError.unreadable(error)
        }
        let kind = try kind(of: archive)
        if kind != .apk {
            if let entry = archive.entries.first(where: \.isEncrypted) { throw ContainerError.encrypted(entry.name) }
            guard archive.entries.count <= maxEntries else {
                throw ContainerError.overCap(
                    "it holds \(archive.entries.count) entries, more than the \(maxEntries) Minato takes from a container")
            }
        }
        return Container(kind: kind, archive: archive)
    }

    static func kind(of archive: ZipArchive) throws -> ContainerKind {
        if archive.entry(named: "manifest.json") != nil { return .xapk }
        if let info = archive.entry(named: "info.json"), try isAPKMInfo(info, in: archive) { return .apkm }
        if archive.entry(named: "AndroidManifest.xml") != nil { return .apk }
        if archive.entry(named: "toc.pb") != nil || archive.entry(named: "toc.json") != nil { return .bundletool }
        return .looseSet
    }

    /// Whether `info.json` is a JSON object with `apkm_version` or `pname`, APKMirror's keys. App Manager's own
    /// `info.json` has neither, and unreadable JSON decides nothing.
    static func isAPKMInfo(_ entry: ZipEntry, in archive: ZipArchive) throws -> Bool {
        guard !entry.isEncrypted else { throw ContainerError.encrypted(entry.name) }
        guard entry.uncompressedSize <= maxJSONSize else {
            throw ContainerError.overCap(
                "its info.json has \(entry.uncompressedSize) bytes, more than the \(maxJSONSize) Minato reads")
        }
        let data: Data
        do {
            data = try archive.data(for: entry, maxSize: maxJSONSize)
        } catch let error as APKError {
            throw ContainerError.unreadable(error)
        }
        guard let object = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] else { return false }
        return object["apkm_version"] != nil || object["pname"] != nil
    }

    /// A loose set's APKs (decision 3), in the archive's order: every `.apk` entry in any folder, its extension in
    /// any case, but hidden files and the AppleDouble copies macOS's Archive Utility adds under `__MACOSX/`. A folder's
    /// entry ends in a slash, so it has no file name.
    public func looseSetAPKs() throws -> [ZipEntry] {
        let apks = archive.entries.filter(Self.isLooseAPK)
        guard !apks.isEmpty else { throw ContainerError.noAPK }
        guard apks.count <= Self.maxAPKs else {
            throw ContainerError.overCap("it holds \(apks.count) APKs, more than the \(Self.maxAPKs) Minato takes from a container")
        }
        return apks
    }

    /// The loose set's APKs under names of ours, `1.apk` on, never an entry's path (decision 6); the set's import
    /// names them by their roles.
    public func looseSetItems() throws -> [Item] {
        try looseSetAPKs().enumerated().map { Item(entry: $1, fileName: "\($0 + 1).apk") }
    }

    static func isLooseAPK(_ entry: ZipEntry) -> Bool {
        guard !entry.name.hasPrefix("__MACOSX/") else { return false }
        let name = entry.name.split(separator: "/", omittingEmptySubsequences: false).last.map(String.init) ?? entry.name
        return !name.hasPrefix(".") && name.lowercased().hasSuffix(".apk")
    }
}
