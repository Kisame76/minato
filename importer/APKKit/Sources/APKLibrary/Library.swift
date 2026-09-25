import APKKit
import CryptoKit
import Foundation

public enum LibraryError: Error, Equatable, Sendable {
    /// The manifest's package name is not a valid Android package name (and could escape the library folders).
    case unsafePackageName(String)
    /// The new version's folder would be the one in use: another file whose SHA-256 begins like the
    /// installed version's (ADR-0034).
    case versionFolderInUse(String)
}

/// The installed apps and the import pipeline: picked file → hidden copy → analysis → install directory → index.
public actor Library {
    public nonisolated let paths: LibraryPaths
    public private(set) var entries: [LibraryEntry]
    private let preferredLanguages: [String]

    public init(paths: LibraryPaths, preferredLanguages: [String] = Locale.preferredLanguages) throws {
        self.paths = paths
        self.preferredLanguages = preferredLanguages
        if FileManager.default.fileExists(atPath: paths.indexFile.path) {
            entries = try Self.decoder.decode(Index.self, from: Data(contentsOf: paths.indexFile)).entries
                .map { Self.findingFolder(of: $0, in: paths) }
        } else {
            entries = []
        }
    }

    /// Picked files that are not installed yet: normally the ones `adopt` just took over, after an
    /// interrupted import also those left over from last time. What such an import left in apps/ goes first.
    public func scan() throws -> [URL] {
        removeLeftovers()
        let fileManager = FileManager.default
        try fileManager.createDirectory(at: paths.incoming, withIntermediateDirectories: true)
        return try fileManager.contentsOfDirectory(at: paths.incoming, includingPropertiesForKeys: nil)
            .filter(Self.isAPK)
            .sorted { $0.lastPathComponent < $1.lastPathComponent }
    }

    /// Takes over a file picked with + or opened with "Open with" and returns where it waits for `importFile`.
    /// The file is cloned into a hidden folder: on APFS that takes no extra space, nothing new appears in the
    /// Files app, and the original stays where it is. Only a copy iOS itself put into Documents/Inbox is removed.
    /// Callers own security-scoped access to `url`.
    public func adopt(_ url: URL) throws -> URL {
        let fileManager = FileManager.default
        try fileManager.createDirectory(at: paths.incoming, withIntermediateDirectories: true)
        let target = Self.unusedURL(in: paths.incoming, name: url.lastPathComponent)
        var coordinationError: NSError?
        var copyError: Error?
        // Coordinated read, so that files in iCloud Drive or other file providers are materialised first.
        NSFileCoordinator().coordinate(readingItemAt: url, options: [], error: &coordinationError) { readableURL in
            do { try fileManager.copyItem(at: readableURL, to: target) } catch { copyError = error }
        }
        if let error = coordinationError ?? copyError { throw error }
        if Self.isDirectChild(url, of: paths.inbox) {
            try? fileManager.removeItem(at: url)
        }
        return target
    }

    /// Analyses one APK and installs it, updates the installed app of its package, or leaves that app as
    /// it is (ADR-0034). Never throws for a bad APK: that becomes an `.invalid` entry.
    @discardableResult
    public func importFile(_ url: URL, progress: @Sendable (Double) -> Void = { _ in }) throws -> ImportResult {
        let fileManager = FileManager.default
        var source = try Self.sourceFile(for: url)
        progress(0)

        let report: APKReport
        let icon: Data?
        do {
            let archive = try ZipArchive(url: url)
            report = try APKAnalyzer.analyze(archive, preferredLanguages: preferredLanguages)
            guard Self.isSafePackageName(report.packageName) else { throw LibraryError.unsafePackageName(report.packageName) }
            icon = try? APKAnalyzer.iconData(for: report, in: archive)
        } catch {
            let entry = LibraryEntry(
                id: "file:\(url.lastPathComponent)", displayName: url.lastPathComponent, state: .invalid(String(describing: error)),
                report: nil, base: nil, splits: [], obbs: [], source: source, iconFile: nil, importedAt: Date())
            try store(entry)
            discardPickedCopy(url)
            progress(1)
            return ImportResult(entry: entry, decision: .install, report: nil)
        }
        progress(0.2)

        let package = report.packageName
        let state: LibraryEntry.State = switch report.compatibility.status {
        case .ready: .ready
        case .needsSplits: .needsSplits
        case .incompatible: .incompatible
        }
        let digest = try Self.sha256(of: url, size: source.size) { progress(0.2 + 0.6 * $0) }
        source.sha256 = digest
        let installed = entries.first { $0.id == package }
        let decision = UpdatePolicy.decide(
            installed: installed.flatMap(AppVersion.init(entry:)),
            new: AppVersion(state: state, versionCode: report.versionCode, sha256: digest))
        switch (decision, installed) {
        case (.unchanged, let installed?), (.reject, let installed?):
            discardPickedCopy(url)
            progress(1)
            return ImportResult(entry: installed, decision: decision, report: report)
        default:
            // Until step b) asks first, an older version replaces the installed one, as before ADR-0034.
            break
        }

        // The new version's folder is built completely while the installed version stays in use.
        let folder = String(digest.prefix(16))
        let appDirectory = paths.installDirectory(for: package)
        let versionDirectory = appDirectory.appendingPathComponent(folder, isDirectory: true)
        // The same bytes changed nothing above, so only another file whose SHA-256 begins like the
        // installed version's gets here with its folder: that one stays untouched.
        guard installed?.folder != folder else { throw LibraryError.versionFolderInUse(folder) }
        try? fileManager.removeItem(at: versionDirectory) // left over from an interrupted import of this file
        try fileManager.createDirectory(at: versionDirectory, withIntermediateDirectories: true)
        var base: PackageFile?
        if state != .incompatible {
            // On APFS the copy is a clone and takes no extra space.
            try fileManager.copyItem(at: url, to: versionDirectory.appendingPathComponent("base.apk"))
            base = PackageFile(fileName: "base.apk", splitName: nil, size: source.size, sha256: digest)
            for name in ["data", "external", "obb"] {
                try fileManager.createDirectory(at: paths.dataDirectory(for: package).appendingPathComponent(name), withIntermediateDirectories: true)
            }
        }
        progress(0.85)

        var iconFile: String?
        if let icon, let iconPath = report.iconPath {
            let fileName = "icon." + (iconPath.split(separator: ".").last.map { $0.lowercased() } ?? "png")
            try icon.write(to: versionDirectory.appendingPathComponent(fileName), options: .atomic)
            iconFile = "apps/\(package)/\(folder)/\(fileName)"
        }
        try faultBeforeTheSwitch?()

        // The switch: one atomic write of the index names the new folder; then the old version goes. An
        // update changes what belongs to the version and keeps the rest of the entry: the ID, the place,
        // importedAt, the OBBs and the data (ADR-0034, decisions 4 and 6).
        var entry = installed ?? LibraryEntry(
            id: package, displayName: report.label, state: state, report: report, base: base,
            splits: [], obbs: [], source: source, iconFile: iconFile, importedAt: Date())
        entry.displayName = report.label
        entry.state = state
        entry.report = report
        entry.base = base
        entry.splits = []
        entry.source = source
        entry.iconFile = iconFile
        entry.folder = folder
        entry.signedWith = nil
        entry.updatedAt = installed == nil ? nil : Date()
        try store(entry)
        removeEverything(in: appDirectory, except: folder)
        discardPickedCopy(url)
        progress(1)
        return ImportResult(entry: entry, decision: decision, report: report)
    }

    /// Runs right before an import's switch; a test throws from it to stand for an interruption.
    private var faultBeforeTheSwitch: (@Sendable () throws -> Void)?

    func interruptBeforeTheSwitch(with fault: @escaping @Sendable () throws -> Void) {
        faultBeforeTheSwitch = fault
    }

    /// Removes an app with its data. The file it was installed from is not touched.
    public func remove(id: String) throws {
        guard entries.contains(where: { $0.id == id }) else { return }
        if !id.hasPrefix("file:"), Self.isSafePackageName(id) {
            try? FileManager.default.removeItem(at: paths.installDirectory(for: id))
            try? FileManager.default.removeItem(at: paths.dataDirectory(for: id))
        }
        try save(entries.filter { $0.id != id })
        entries.removeAll { $0.id == id }
    }

    /// The hidden copy `adopt` made is not needed once the app is installed or recorded as invalid.
    private func discardPickedCopy(_ url: URL) {
        if Self.isDirectChild(url, of: paths.incoming) {
            try? FileManager.default.removeItem(at: url)
        }
    }

    // MARK: - Version folders (ADR-0034)

    /// Removes everything in an app's folder but the folder of the version its entry names.
    private func removeEverything(in appDirectory: URL, except folder: String) {
        for name in (try? FileManager.default.contentsOfDirectory(atPath: appDirectory.path)) ?? [] where name != folder {
            try? FileManager.default.removeItem(at: appDirectory.appendingPathComponent(name))
        }
    }

    /// Removes from apps/ what no entry names after an interrupted import: the folder of a package without
    /// an entry, and in an app's folder everything but its version's folder. An entry from before version
    /// folders keeps its files in the app's folder itself, so only version folders next to them go.
    private func removeLeftovers() {
        let fileManager = FileManager.default
        for package in (try? fileManager.contentsOfDirectory(atPath: paths.appsDirectory.path)) ?? [] {
            let appDirectory = paths.appsDirectory.appendingPathComponent(package, isDirectory: true)
            guard let entry = entries.first(where: { $0.id == package }) else {
                try? fileManager.removeItem(at: appDirectory)
                continue
            }
            if let folder = entry.folder {
                removeEverything(in: appDirectory, except: folder)
            } else {
                for name in (try? fileManager.contentsOfDirectory(atPath: appDirectory.path)) ?? [] where Self.isVersionFolder(name) {
                    try? fileManager.removeItem(at: appDirectory.appendingPathComponent(name))
                }
            }
        }
    }

    /// A version folder's name: 16 hex digits.
    static func isVersionFolder(_ name: String) -> Bool {
        name.count == 16 && name.allSatisfy(\.isHexDigit)
    }

    /// The entry with its version's folder. An older Minato build that wrote the index again dropped
    /// `folder`, but the folder is still there, named after the base.apk's SHA-256 or found in the
    /// icon's path; without it, the next scan would remove the installed version as a leftover.
    static func findingFolder(of entry: LibraryEntry, in paths: LibraryPaths) -> LibraryEntry {
        guard entry.folder == nil, isSafePackageName(entry.id) else { return entry }
        let iconParts = entry.iconFile?.split(separator: "/") ?? []
        let iconFolder = iconParts.count == 4 && iconParts[0] == "apps" && iconParts[1] == entry.id ? String(iconParts[2]) : nil
        for case let folder? in [entry.base.map { String($0.sha256.prefix(16)) }, iconFolder] where isVersionFolder(folder) {
            var isDirectory: ObjCBool = false
            let path = paths.installDirectory(for: entry.id).appendingPathComponent(folder).path
            if FileManager.default.fileExists(atPath: path, isDirectory: &isDirectory), isDirectory.boolValue {
                var found = entry
                found.folder = folder
                return found
            }
        }
        return entry
    }

    // MARK: - Index

    private struct Index: Codable {
        var version = 1
        var entries: [LibraryEntry]
    }

    /// Changes every entry in place and saves the index once.
    func recordAll(_ change: (inout LibraryEntry) -> Void) throws {
        var changed = entries
        for position in changed.indices { change(&changed[position]) }
        try save(changed)
        entries = changed
    }

    /// Changes the entry `id` in place and saves the index; nothing when there is no such entry.
    func record(id: String, _ change: (inout LibraryEntry) -> Void) throws {
        guard let position = entries.firstIndex(where: { $0.id == id }) else { return }
        var changed = entries
        change(&changed[position])
        try save(changed)
        entries = changed
    }

    /// Adds or replaces the entry. The entries change only once the index is written, so the library
    /// always holds what the index names: after a failed switch, the old version (ADR-0034).
    private func store(_ entry: LibraryEntry) throws {
        var changed = entries
        if let position = changed.firstIndex(where: { $0.id == entry.id }) {
            changed[position] = entry
        } else {
            changed.append(entry)
        }
        try save(changed)
        entries = changed
    }

    /// Writes the index with these entries; the callers take them over only once it is written.
    private func save(_ entries: [LibraryEntry]) throws {
        try FileManager.default.createDirectory(at: paths.applicationSupport, withIntermediateDirectories: true)
        try Self.encoder.encode(Index(entries: entries)).write(to: paths.indexFile, options: .atomic)
    }

    private static let encoder: JSONEncoder = {
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        return encoder
    }()

    private static let decoder: JSONDecoder = {
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        return decoder
    }()

    // MARK: - Files

    static func isAPK(_ url: URL) -> Bool {
        url.pathExtension.lowercased() == "apk"
    }

    /// Android package names: dot-separated segments of letters, digits and underscores.
    /// Anything else could turn into a path outside the library.
    static func isSafePackageName(_ name: String) -> Bool {
        guard !name.isEmpty, name.count <= 255 else { return false }
        return name.split(separator: ".", omittingEmptySubsequences: false).allSatisfy { segment in
            guard let first = segment.first, first.isASCII, first.isLetter || first == "_" else { return false }
            return segment.allSatisfy { $0.isASCII && ($0.isLetter || $0.isNumber || $0 == "_") }
        }
    }

    /// Whether `url` lies directly in `folder`, comparing resolved paths (/var is a symlink to /private/var).
    static func isDirectChild(_ url: URL, of folder: URL) -> Bool {
        url.deletingLastPathComponent().resolvingSymlinksInPath().standardizedFileURL.path
            == folder.resolvingSymlinksInPath().standardizedFileURL.path
    }

    static func sourceFile(for url: URL) throws -> SourceFile {
        let values = try url.resourceValues(forKeys: [.fileSizeKey, .contentModificationDateKey])
        return SourceFile(
            fileName: url.lastPathComponent,
            size: UInt64(values.fileSize ?? 0),
            modifiedMilliseconds: Int64(((values.contentModificationDate ?? .distantPast).timeIntervalSince1970 * 1000).rounded()))
    }

    private static func unusedURL(in folder: URL, name: String) -> URL {
        var candidate = folder.appendingPathComponent(name)
        let base = (name as NSString).deletingPathExtension
        let pathExtension = (name as NSString).pathExtension
        var counter = 2
        while FileManager.default.fileExists(atPath: candidate.path) {
            candidate = folder.appendingPathComponent("\(base) \(counter).\(pathExtension)")
            counter += 1
        }
        return candidate
    }

    private static func sha256(of url: URL, size: UInt64, progress: (Double) -> Void) throws -> String {
        let handle = try FileHandle(forReadingFrom: url)
        defer { try? handle.close() }
        var hasher = SHA256()
        var done: UInt64 = 0
        while let chunk = try handle.read(upToCount: 4 << 20), !chunk.isEmpty {
            hasher.update(data: chunk)
            done += UInt64(chunk.count)
            progress(Double(done) / Double(max(size, 1)))
        }
        return hasher.finalize().map { String(format: "%02x", $0) }.joined()
    }
}
