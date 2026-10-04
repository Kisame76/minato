import APKFormats
import APKKit
import CryptoKit
import Foundation
import Synchronization

public enum LibraryError: Error, Equatable, Sendable {
    /// The manifest's package name is not a valid Android package name (and could escape the library folders).
    case unsafePackageName(String)
    /// The new version's folder would be the one in use: another file whose SHA-256 begins like the
    /// installed version's (ADR-0034).
    case versionFolderInUse(String)
    /// A pick with nothing to take over: a folder without `.apk` or `.obb` files, or only links (ADR-0048, decision 1).
    case nothingToImport
}

extension LibraryError: LocalizedError {
    /// A reason for the app to show after "could not be added:" or "could not be imported:". An `.invalid` entry
    /// keeps the error's description instead, which the app reads.
    public var errorDescription: String? {
        switch self {
        case .unsafePackageName(let name): "“\(name)” is not a valid package name"
        case .versionFolderInUse(let folder): "its version folder \(folder) is still in use"
        case .nothingToImport: "it has no APK or OBB file"
        }
    }
}

/// The installed apps and the import pipeline: picked file → hidden copy → analysis → install directory → index.
public actor Library {
    public nonisolated let paths: LibraryPaths
    public private(set) var entries: [LibraryEntry]
    private let preferredLanguages: [String]
    /// What a set's splits are chosen for (ADR-0048, decision 4).
    private let device: DeviceProfile

    /// `device` defaults to an iPhone at 3x with `preferredLanguages`.
    public init(paths: LibraryPaths, preferredLanguages: [String] = Locale.preferredLanguages,
                device: DeviceProfile? = nil) throws {
        self.paths = paths
        self.preferredLanguages = preferredLanguages
        self.device = device ?? .iPhone(nativeScale: 3, languages: preferredLanguages)
        entries = try Self.entries(in: paths)
        Self.launched.withLock { launched in
            if launched.insert(paths.incoming.standardizedFileURL.path).inserted { Self.removeUnfinishedPicks(in: paths) }
        }
    }

    /// The incoming folders this process has cleaned up. Only its first Library does so, before any pick of the process
    /// starts and while later ones wait, so a pick another Library is still taking over stays: "Open with" during a
    /// session makes one for each file. iOS runs one process of the app at a time (ADR-0048, decision 1).
    private static let launched = Mutex<Set<String>>([])

    /// Removes the picks an earlier launch did not finish taking over, `incoming/<uuid>.partial/`.
    private static func removeUnfinishedPicks(in paths: LibraryPaths) {
        for name in (try? FileManager.default.contentsOfDirectory(atPath: paths.incoming.path)) ?? []
        where (name as NSString).pathExtension == "partial" {
            try? FileManager.default.removeItem(at: paths.incoming.appendingPathComponent(name))
        }
    }

    /// The entries the index holds, read without the actor: at launch, game mode or the library is chosen
    /// from them before anything else (ADR-0032, decision 1).
    public static func entries(in paths: LibraryPaths) throws -> [LibraryEntry] {
        guard FileManager.default.fileExists(atPath: paths.indexFile.path) else { return [] }
        return try decoder.decode(Index.self, from: Data(contentsOf: paths.indexFile)).entries
            .map { findingFolder(of: $0, in: paths) }
    }

    /// The picks that are not installed yet, one URL each and the oldest first, for `importUnit`: a unit's folder, or a
    /// file lying flat in incoming/ (ADR-0048, decision 1). Normally those `adopt` just took over, after an interrupted
    /// import also those left over from last time. A pick still being taken over is none yet, and one without an APK
    /// waits, its OBBs for ADR-0049. What such an import left in apps/ goes first.
    public func scanUnits() throws -> [URL] {
        try waitingUnits().map(\.url)
    }

    /// The APKs of the picks `scanUnits` gives, each pick's in name order.
    public func scan() throws -> [URL] {
        try waitingUnits().flatMap(\.apks)
    }

    private func waitingUnits() throws -> [(url: URL, apks: [URL])] {
        removeLeftovers()
        syncSignersFiles()
        let fileManager = FileManager.default
        try fileManager.createDirectory(at: paths.incoming, withIntermediateDirectories: true)
        // Resource values describe a link itself, so a link is neither a folder nor a regular file.
        let keys: Set<URLResourceKey> = [.isDirectoryKey, .isRegularFileKey, .creationDateKey]
        var units: [(created: Date, name: String, url: URL, apks: [URL])] = []
        for item in try fileManager.contentsOfDirectory(at: paths.incoming, includingPropertiesForKeys: Array(keys),
                                                        options: [.skipsHiddenFiles]) {
            let values = try item.resourceValues(forKeys: keys)
            let created = values.creationDate ?? .distantPast
            if values.isDirectory == true, item.pathExtension != "partial" {
                let files = try Self.files(inUnit: item)
                let apks = files.filter(Self.isAPK)
                if files.isEmpty {
                    rmdir(item.path) // its last file went, the folder did not
                } else if !apks.isEmpty {
                    units.append((created, item.lastPathComponent, item, apks))
                }
            } else if values.isRegularFile == true, Self.isAPK(item) {
                // Older builds took a file over flat, and tools/session-check.sh and tools/update-check.sh still drop
                // one there: a unit of its own.
                units.append((created, item.lastPathComponent, item, [item]))
            }
        }
        return units.sorted { ($0.created, $0.name) < ($1.created, $1.name) }.map { ($0.url, $0.apks) }
    }

    /// Takes over one pick (ADR-0048, decision 1): files picked together with +, a folder, or a file opened with "Open
    /// with". A folder gives its `.apk` and `.obb` files, or its only subfolder's (`ImportUnit.files`). They are cloned
    /// into `incoming/<uuid>.partial/`, which becomes `incoming/<uuid>/` once complete, so a scan never sees a pick half
    /// taken over: on APFS a clone takes no extra space, nothing new appears in the Files app, and the originals stay
    /// where they are. Only regular files are taken, never a link, so nothing in incoming/ points outside the pick, and
    /// only a copy iOS itself put into Documents/Inbox is removed. Returns the unit's folder. Callers own
    /// security-scoped access to the pick.
    public func adopt(_ pick: [URL]) async throws -> URL {
        let fileManager = FileManager.default
        var sources: [URL] = []
        for item in pick {
            sources += try Self.coordinatedRead(item) { readable in
                // Resource values describe a link itself, so a picked link is neither and gives nothing.
                let values = try readable.resourceValues(forKeys: [.isDirectoryKey, .isRegularFileKey])
                if values.isDirectory == true { return try ImportUnit.files(inFolder: readable) }
                return values.isRegularFile == true ? [readable] : []
            }
        }
        guard sources.count <= ImportUnit.maxFiles else { throw ImportUnitError.tooManyFiles(sources.count) }
        try fileManager.createDirectory(at: paths.incoming, withIntermediateDirectories: true)
        let unit = paths.incoming.appendingPathComponent(UUID().uuidString, isDirectory: true)
        let partial = unit.appendingPathExtension("partial")
        try fileManager.createDirectory(at: partial, withIntermediateDirectories: false)
        do {
            var taken = 0
            for source in sources {
                let target = Self.unusedURL(in: partial, name: source.lastPathComponent)
                try Self.coordinatedRead(source) { readable in
                    // A clone where the volume allows one, else a copy. A link that took the file's place in the
                    // meantime is copied as a link, and goes below.
                    let flags = copyfile_flags_t(COPYFILE_CLONE | COPYFILE_NOFOLLOW)
                    guard copyfile(readable.path, target.path, nil, flags) == 0 else {
                        throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
                    }
                }
                if Self.isRegularFile(target) { taken += 1 } else { try? fileManager.removeItem(at: target) }
                try await faultBetweenClones?()
            }
            guard taken > 0 else { throw LibraryError.nothingToImport }
            try fileManager.moveItem(at: partial, to: unit)
        } catch {
            try? fileManager.removeItem(at: partial)
            throw error
        }
        for item in pick where Self.isDirectChild(item, of: paths.inbox) {
            try? fileManager.removeItem(at: item)
        }
        return unit
    }

    /// Takes over one file, as "Open with" hands it over, and returns where it waits for `importFile`: in its unit.
    public func adopt(_ url: URL) async throws -> URL {
        let unit = try await adopt([url])
        let files = try Self.files(inUnit: unit)
        return files.count == 1 ? files[0] : unit
    }

    /// Runs `access` on `url` in a coordinated read, so that files in iCloud Drive or other file providers are
    /// materialised first.
    private static func coordinatedRead<Result>(_ url: URL, _ access: (URL) throws -> Result) throws -> Result {
        var coordinationError: NSError?
        var outcome: Swift.Result<Result, Error>?
        NSFileCoordinator().coordinate(readingItemAt: url, options: [], error: &coordinationError) { readable in
            outcome = Swift.Result { try access(readable) }
        }
        if let coordinationError { throw coordinationError }
        guard let outcome else { throw CocoaError(.fileReadUnknown) }
        return try outcome.get()
    }

    /// A unit's files: regular ones, never links or hidden files, in byte order of their names.
    static func files(inUnit unit: URL) throws -> [URL] {
        try FileManager.default.contentsOfDirectory(at: unit, includingPropertiesForKeys: [.isRegularFileKey],
                                                    options: [.skipsHiddenFiles])
            .filter { (try? $0.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true }
            .sorted { $0.lastPathComponent.utf8.lexicographicallyPrecedes($1.lastPathComponent.utf8) }
    }

    /// Analyses one APK and installs it, updates the installed app of its package, or leaves that app as
    /// it is (ADR-0034). When the policy asks, nothing changes and the file waits where it is, so the
    /// question comes again at the next scan; `replacing` is the answer "Replace", `keepInstalled` the
    /// other. A NativeActivity app's libraries are converted and signed with `signing`; without it only
    /// their originals are kept (ADR-0032). Never throws for a bad APK: that becomes an `.invalid` entry.
    @discardableResult
    public func importFile(_ url: URL, replacing: Bool = false, signing: ImportSigning? = nil,
                           progress: @Sendable (Double) -> Void = { _ in }) throws -> ImportResult {
        try importFile(url, answering: { _ in replacing }, signing: signing, progress: progress)
    }

    /// `importFile`, with "Replace" answered for the versions `answering` takes, and the file's SHA-256 where a unit has
    /// read it already.
    private func importFile(_ url: URL, answering: (AskingVersion) -> Bool, signing: ImportSigning?, sha256 known: String? = nil,
                            progress: @Sendable (Double) -> Void) throws -> ImportResult {
        var source = try Self.sourceFile(for: url)
        progress(0)

        let archive: ZipArchive
        let report: APKReport
        let icon: Data?
        do {
            archive = try ZipArchive(url: url)
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

        let digest = try known ?? hash(url, size: source.size) { progress(0.2 + 0.6 * $0) }
        source.sha256 = digest
        let signer = report.signature.signer ?? SignerIdentity(current: [])
        let new = NewVersion(
            report: report, digest: digest, source: source,
            files: [(url, PackageFile(fileName: "base.apk", splitName: nil, size: source.size, sha256: digest))],
            skippedSplits: nil, icon: icon, archives: [nil: archive],
            version: { state in
                AppVersion(state: state, versionCode: report.versionCode, sha256: digest, signer: signer,
                           signerProven: report.signature.check == .verified,
                           signatureFailed: report.signature.check?.isFailure == true)
            })
        return try install(new, answering: answering, signing: signing, discarding: url, progress: progress)
    }

    /// A version about to be installed: one APK, or a set's base and chosen splits (ADR-0048, decision 6).
    private struct NewVersion {
        var report: APKReport
        /// The file's SHA-256, or the set's digest: the version's folder is named after it.
        var digest: String
        var source: SourceFile
        /// The APKs the version holds, the base first, each with its name in the folder.
        var files: [(url: URL, file: PackageFile)]
        var skippedSplits: [SkippedSplit]?
        var icon: Data?
        /// The archives of the APKs it holds, by split name, nil for the base: the native code comes from them.
        var archives: [String?: ZipArchive]
        /// What the update policy compares, for a state.
        var version: (LibraryEntry.State) -> AppVersion
    }

    /// Installs a version, updates the installed app of its package with it, or leaves that app as it is (ADR-0034).
    /// When the policy asks, nothing changes. `pickedCopy` goes once the version is installed or turned away.
    private func install(_ new: NewVersion, answering: (AskingVersion) -> Bool, signing: ImportSigning?,
                         discarding pickedCopy: URL?, progress: @Sendable (Double) -> Void) throws -> ImportResult {
        let fileManager = FileManager.default
        var report = new.report
        let package = report.packageName
        var state: LibraryEntry.State = switch report.compatibility.status {
        case .ready: .ready
        case .needsSplits: .needsSplits
        case .incompatible: .incompatible
        }
        func finish(_ result: ImportResult) -> ImportResult {
            if let pickedCopy { discardPickedCopy(pickedCopy) }
            progress(1)
            return result
        }
        let digest = new.digest
        let installed = entries.first { $0.id == package }
        let installedVersion = installed.flatMap(version(of:))
        var decision = UpdatePolicy.decide(installed: installedVersion, new: new.version(state))
        // "Replace" answers the question as it was shown (F7): one that asks otherwise now asks again.
        let replacing = if case .ask(let warnings) = decision {
            answering(AskingVersion(package: package, versionCode: report.versionCode, warnings: warnings))
        } else {
            false
        }
        switch (decision, installed) {
        case (.unchanged, let installed?), (.reject, let installed?):
            return finish(ImportResult(entry: installed, decision: decision, report: report))
        case (.ask, let installed?) where !replacing:
            progress(1)
            return ImportResult(entry: installed, decision: decision, report: report,
                                uncheckedFiles: new.version(state).uncheckedFiles)
        case (.ask, nil) where !replacing:
            // A fresh install that asks (ADR-0048, step b3): nothing is stored, and the entry is the one it would make.
            progress(1)
            let entry = LibraryEntry(id: package, displayName: report.label, state: state, report: report, base: nil,
                                     splits: [], obbs: [], source: new.source, iconFile: nil, importedAt: Date())
            return ImportResult(entry: entry, decision: decision, report: report,
                                uncheckedFiles: new.version(state).uncheckedFiles)
        case (.ask, _):
            decision = installed == nil ? .install : .update
        default:
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

        // A ready NativeActivity app's native code goes into the version's lib/, converted and signed when a
        // certificate is at hand (ADR-0032, decision 4). Apps with Java code get theirs with M4.
        var signedWith: String?
        if state == .ready, case .nativeActivity = report.runtime {
            do {
                signedWith = try installNativeCode(of: report, from: new.archives, into: versionDirectory, signing: signing)
            } catch let refusal as RefusedLibrary {
                // This version cannot run, so it keeps no code and counts as incompatible: a ready
                // installed version then stays (ADR-0034, decision 1).
                state = .incompatible
                report.compatibility.status = .incompatible
                report.compatibility.issues.append(.conversionFailed(library: refusal.library, reason: refusal.reason))
                try? fileManager.removeItem(at: versionDirectory.appendingPathComponent("lib"))
                decision = UpdatePolicy.decide(installed: installedVersion, new: new.version(state))
                if case .reject = decision, let installed {
                    try? fileManager.removeItem(at: versionDirectory)
                    return finish(ImportResult(entry: installed, decision: decision, report: report))
                }
            }
        }
        var base: PackageFile?
        var splits: [PackageFile] = []
        if state != .incompatible {
            for (url, file) in new.files {
                // On APFS the copy is a clone and takes no extra space.
                try fileManager.copyItem(at: url, to: versionDirectory.appendingPathComponent(file.fileName))
            }
            base = new.files.first?.file
            splits = new.files.dropFirst().map(\.file)
            // Who signed it, for the guest's PackageManager (ADR-0034 decision 2): what the check proved, so an
            // unsigned or unverified APK has none.
            if report.signature.check == .verified, let proof = report.signature.proof {
                try SignersFile.content(of: proof)
                    .write(to: versionDirectory.appendingPathComponent(SignersFile.fileName), options: .atomic)
            }
            try paths.prepareData(for: package)
        }
        progress(0.85)

        var iconFile: String?
        if let icon = new.icon, let iconPath = report.iconPath {
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
            splits: [], obbs: [], source: new.source, iconFile: iconFile, importedAt: Date())
        entry.displayName = report.label
        entry.state = state
        entry.report = report
        entry.base = base
        entry.splits = splits
        entry.skippedSplits = new.skippedSplits
        entry.source = new.source
        entry.iconFile = iconFile
        entry.folder = folder
        entry.signedWith = signedWith
        entry.updatedAt = installed == nil ? nil : Date()
        try store(entry)
        removeEverything(in: appDirectory, except: folder)
        return finish(ImportResult(entry: entry, decision: decision, report: report))
    }

    /// Imports a unit (ADR-0048, decisions 1 to 7): its APKs grouped into sets by package and version, where splits
    /// without their base join the installed version. A set is checked as Android's installer checks a session, its
    /// splits are chosen for the device, and it is analysed over the APKs Minato installs, then installed, updated or
    /// asked about as one APK is (ADR-0034); a set Android refuses becomes an `.invalid` entry. One APK, and a file
    /// that is no APK, go through `importFile`, as before. The unit stays while a set asks, so the question comes again
    /// at the next scan. `replacing` answers "Replace" for the versions the question listed, as it listed them, and for
    /// no other, which keeps asking (F7, the lead's reviewer); one that asks otherwise now asks again. The first half of
    /// `progress` reads each APK once, by its size, and the second imports the sets and files, a share each. Returns a
    /// result for each set and each such file.
    @discardableResult
    public func importUnit(_ unit: URL, replacing answered: Set<AskingVersion> = [], signing: ImportSigning? = nil,
                           progress: @Sendable (Double) -> Void = { _ in }) throws -> [ImportResult] {
        var isFolder: ObjCBool = false
        _ = FileManager.default.fileExists(atPath: unit.path, isDirectory: &isFolder)
        let urls = isFolder.boolValue ? try Self.files(inUnit: unit).filter(Self.isAPK) : [unit]
        let sizes = try urls.map { try Self.sourceFile(for: $0).size }
        let total = Double(max(sizes.reduce(0, +), 1))
        var read: UInt64 = 0
        var digests: [URL: String] = [:]
        var unreadable: [URL] = []
        var members: [SetMember] = []
        var files: [String: (url: URL, archive: ZipArchive)] = [:]
        progress(0)
        for (url, size) in zip(urls, sizes) {
            let sha256 = try hash(url, size: size) { progress(0.5 * (Double(read) + $0 * Double(size)) / total) }
            read += size
            digests[url] = sha256
            guard let archive = try? ZipArchive(url: url),
                  let member = try? SetMember.read(archive, fileName: url.lastPathComponent, sha256: sha256) else {
                unreadable.append(url)
                continue
            }
            members.append(member)
            files[member.fileName] = (url, archive)
        }
        let groups = SetGrouping.byPackageAndVersion(members)
        let parts = Double(max(unreadable.count + groups.count, 1))
        var results: [ImportResult] = []
        for url in unreadable {
            let part = Double(results.count)
            results.append(try importFile(url, answering: answered.contains, signing: signing, sha256: digests[url]) {
                progress(0.5 + 0.5 * (part + $0) / parts)
            })
        }
        for group in groups {
            if group.count == 1, group[0].isBase, let url = files[group[0].fileName]?.url {
                let part = Double(results.count)
                results.append(try importFile(url, answering: answered.contains, signing: signing, sha256: digests[url]) {
                    progress(0.5 + 0.5 * (part + $0) / parts)
                })
            } else {
                results.append(try importSet(group, files: files, answering: answered.contains, signing: signing))
                progress(0.5 + 0.5 * Double(results.count) / parts)
            }
        }
        // The unit goes once nothing in it waits for an answer; an OBB waits for ADR-0049.
        if !results.contains(where: { if case .ask = $0.decision { true } else { false } }) {
            for url in urls { try? FileManager.default.removeItem(at: url) }
            rmdir(unit.path)
        }
        progress(1)
        return results
    }

    /// One set of a unit (ADR-0048, decisions 3 to 7): the base with its chosen splits, or splits without their base,
    /// which join the installed version.
    private func importSet(_ group: [SetMember], files: [String: (url: URL, archive: ZipArchive)],
                           answering: (AskingVersion) -> Bool, signing: ImportSigning?) throws -> ImportResult {
        /// An `.invalid` entry of `file` for a set that is not installed. The app shows `reason` as it is, as the entry's
        /// `refusal`, unless it is an error's description.
        func refuse(_ reason: String, file: SetMember, isDescription: Bool = false) throws -> ImportResult {
            var entry = LibraryEntry(
                id: "file:\(file.fileName)", displayName: file.fileName, state: .invalid(reason), report: nil, base: nil,
                splits: [], obbs: [], source: try Self.sourceFile(for: files[file.fileName]!.url), iconFile: nil,
                importedAt: Date())
            if !isDescription { entry.refusal = reason }
            try store(entry)
            return ImportResult(entry: entry, decision: .install, report: nil)
        }
        var set: CheckedSet
        switch SetChecks.check(group) {
        case .success(let checked): set = checked
        case .failure(let refusal): return try refuse(refusal.reason, file: group[0])
        }
        // The unit's APKs Minato cannot verify, by the names the user picked them under.
        let unchecked = group.filter { if case .unverified = $0.signature.check { true } else { false } }.map(\.fileName)
        // Each APK of the set by its split name, unique once checked: the unit's, then the installed version's.
        var apks = Dictionary(uniqueKeysWithValues: group.map { ($0.splitName, files[$0.fileName]!) })
        var installedSource: SourceFile?
        if set.base == nil, let installed = entries.first(where: { $0.id == group[0].packageName }) {
            // Splits without their base join the installed version, as Android's inherit mode adds them, and the
            // joined set is checked again (decision 7).
            let installedAPKs = installedAPKs(of: installed)
            if let joined = SetChecks.joining(set.splits, installed: installedAPKs.map(\.member)),
               let installedBase = joined.first(where: \.isBase) {
                let first = set.splits[0]
                switch SetChecks.check(joined) {
                case .success(let checked): set = checked
                case .failure(let refusal): return try refuse(refusal.reason, file: first)
                }
                // The session's signer comes from its first APK, here a split (Minato's first by name, as SetChecks
                // orders them), and Android holds it to the scheme the package's targetSdkVersion asks for
                // (PackageInstallerSession.java:3501-3503, InstallPackageHelper.java:1330-1331, ScanPackageUtils.java:
                // 740-751), while SetMember verified the split for no targetSdkVersion.
                if installedBase.targetSdk >= 30,
                   case .failed(let failure) = SignatureVerifier.verify(files[first.fileName]!.archive,
                                                                        targetSdk: installedBase.targetSdk).check,
                   failure.kind == .noV2Signature {
                    return try refuse("No signature found in package of version 2 or newer for package \(first.packageName)",
                                      file: first)
                }
                for apk in installedAPKs where apks[apk.member.splitName] == nil {
                    apks[apk.member.splitName] = (apk.url, apk.archive)
                }
                installedSource = installed.source
            }
        }
        guard let base = set.base else {
            let split = set.splits[0]
            return try refuse("Split APK “\(split.fileName)” needs its base APK. Import it together with its base APK.",
                              file: split)
        }
        guard Self.isSafePackageName(base.packageName) else {
            // The unit's first file: after a join, the base is the installed one.
            return try refuse(String(describing: LibraryError.unsafePackageName(base.packageName)), file: group[0],
                              isDescription: true)
        }
        let choice = SplitSelector.choose(set, for: device)
        var (report, icon) = try SetAnalysis.analyze(set, choice: choice, preferredLanguages: preferredLanguages) {
            apks[$0.splitName]!.archive
        }
        if let reason = choice.incompatibility {
            report.compatibility.issues.append(.splitChoice(reason))
            report.compatibility.status = .incompatible
        }
        let installing = [base] + choice.chosen
        let digest = choice.chosen.isEmpty ? base.sha256 : try SetDigest.of(installing.map { ($0.splitName, $0.sha256) })
        var source = try installedSource ?? Self.sourceFile(for: apks[nil]!.url)
        source.sha256 = base.sha256
        let new = NewVersion(
            report: report, digest: digest, source: source,
            files: try installing.map { member in
                let url = apks[member.splitName]!.url
                return (url, PackageFile(fileName: SetDigest.installedName(of: member.splitName), splitName: member.splitName,
                                         size: try Self.sourceFile(for: url).size, sha256: member.sha256,
                                         kind: Self.kind(of: member.role), configForSplit: member.configForSplit))
            },
            skippedSplits: choice.skipped.map {
                SkippedSplit(fileName: $0.member.fileName, splitName: $0.member.splitName ?? "", reason: $0.reason)
            },
            icon: icon,
            archives: Dictionary(uniqueKeysWithValues: installing.map { ($0.splitName, apks[$0.splitName]!.archive) }),
            version: { [set] state in
                // A join takes along installed APKs that were asked about at their import and whose bytes are checked
                // again (installedAPKs), so the question names only the unit's own APKs Minato cannot check.
                var version = AppVersion(set: set, state: state, sha256: digest)
                if !version.uncheckedFiles.isEmpty { version.uncheckedFiles = unchecked }
                return version
            })
        return try install(new, answering: answering, signing: signing, discarding: nil, progress: { _ in })
    }

    /// The APKs of an installed version as members of a set, under their names in its folder: none where it keeps no
    /// base, where one of them cannot be read or has other bytes than its entry names, or where they no longer pass
    /// as a set, which Android, taking them unverified, would not notice (PackageInstallerSession.java:3680-3681).
    private func installedAPKs(of entry: LibraryEntry) -> [(member: SetMember, url: URL, archive: ZipArchive)] {
        guard let base = entry.base else { return [] }
        let folder = paths.versionDirectory(for: entry)
        var apks: [(member: SetMember, url: URL, archive: ZipArchive)] = []
        for file in [base] + entry.splits {
            let url = folder.appendingPathComponent(file.fileName)
            guard let size = try? Self.sourceFile(for: url).size,
                  let sha256 = try? Self.sha256(of: url, size: size, progress: { _ in }), sha256 == file.sha256,
                  let archive = try? ZipArchive(url: url),
                  let member = try? SetMember.read(archive, fileName: file.fileName, sha256: sha256) else { return [] }
            apks.append((member, url, archive))
        }
        guard case .success = SetChecks.check(apks.map(\.member)) else { return [] }
        return apks
    }

    static func kind(of role: SplitRole) -> PackageFile.Kind {
        switch role {
        case .base: .base
        case .feature: .feature
        case .config: .config
        case .other: .other
        }
    }

    /// The answer "Keep Installed" to a question: the waiting file goes, or a unit's APKs, while its OBBs wait for
    /// ADR-0049 as at the end of `importUnit`; the installed version stays.
    public func keepInstalled(discarding url: URL) {
        var isFolder: ObjCBool = false
        guard Self.isDirectChild(url, of: paths.incoming),
              FileManager.default.fileExists(atPath: url.path, isDirectory: &isFolder), isFolder.boolValue else {
            discardPickedCopy(url)
            return
        }
        for apk in (try? Self.files(inUnit: url))?.filter(Self.isAPK) ?? [] {
            try? FileManager.default.removeItem(at: apk)
        }
        rmdir(url.path) // only once nothing else waits in it
    }

    /// The version an entry holds. A report from before the signature check (ADR-0034, step d), and so
    /// from before the signer or its lineage's capabilities were recorded, has its base.apk checked now.
    private func version(of entry: LibraryEntry) -> AppVersion? {
        guard var version = AppVersion(entry: entry) else { return nil }
        // A set is its digest, so the same set changes nothing (ADR-0048, decision 6).
        if let digest = Self.setDigest(of: entry) { version.sha256 = digest }
        let signature = entry.report?.signature
        if signature?.check == nil || signature?.signer?.knowsCapabilities != true, let base = entry.base,
           let archive = try? ZipArchive(url: paths.versionDirectory(for: entry).appendingPathComponent(base.fileName)) {
            let verification = SignatureVerifier.verify(archive, targetSdk: entry.report?.targetSdk ?? 0)
            version.signerProven = verification.check == .verified
            if let signer = verification.signer ?? APKSignatures.read(archive).signer { version.signer = signer }
        }
        return version
    }

    /// Runs right before an import's switch; a test throws from it to stand for an interruption.
    private var faultBeforeTheSwitch: (@Sendable () throws -> Void)?

    func interruptBeforeTheSwitch(with fault: @escaping @Sendable () throws -> Void) {
        faultBeforeTheSwitch = fault
    }

    /// Runs after each file `adopt` clones for a pick; a test scans from it, or throws to stand for an interruption.
    private var faultBetweenClones: (@Sendable () async throws -> Void)?

    func runBetweenClones(_ hook: @escaping @Sendable () async throws -> Void) {
        faultBetweenClones = hook
    }

    /// Runs right before a scan writes a version's signers.json; a test throws from it to stand for a write that fails.
    private var faultWritingSignersFile: (@Sendable () throws -> Void)?

    func failWritingSignersFile(with fault: @escaping @Sendable () throws -> Void) {
        faultWritingSignersFile = fault
    }

    /// Runs before an import reads a picked file to hash it; a test counts the reads.
    private var hashing: (@Sendable (URL) -> Void)?

    func observeHashing(_ hook: @escaping @Sendable (URL) -> Void) {
        hashing = hook
    }

    /// SHA-256 of a picked file, for an import.
    private func hash(_ url: URL, size: UInt64, progress: (Double) -> Void) throws -> String {
        hashing?(url)
        return try Self.sha256(of: url, size: size, progress: progress)
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

    /// The hidden copy `adopt` made is not needed once the app is installed or recorded as invalid. A unit goes with its
    /// last file; a file flat in incoming/ is one of its own.
    private func discardPickedCopy(_ url: URL) {
        if Self.isDirectChild(url, of: paths.incoming) {
            try? FileManager.default.removeItem(at: url)
        } else if Self.isDirectChild(url.deletingLastPathComponent(), of: paths.incoming) {
            try? FileManager.default.removeItem(at: url)
            rmdir(url.deletingLastPathComponent().path) // only once the folder is empty
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

    /// Makes each version's signers.json what its check proved (ADR-0034 decision 2): written when it is missing
    /// or differs, removed when the check did not verify. Only a regular file with the proof's bytes stays, so a
    /// folder or a link in its place goes, and so does a wrong file before its rewrite: a write that fails leaves
    /// none, unless the removal failed too (a logged gap). A report from before the proof was recorded has its
    /// base.apk checked once, and keeps the result.
    private func syncSignersFiles() {
        var checked: [String: SignatureInfo] = [:]
        for entry in entries {
            guard let base = entry.base, let report = entry.report else { continue }
            let directory = paths.versionDirectory(for: entry)
            var signature = report.signature
            if signature.proof == nil, signature.check == nil || signature.check == .verified,
               let archive = try? ZipArchive(url: directory.appendingPathComponent(base.fileName)) {
                let verification = SignatureVerifier.verify(archive, targetSdk: report.targetSdk)
                signature.check = verification.check
                signature.proof = verification.proof
                if let signer = verification.signer { signature.signer = signer }
                checked[entry.id] = signature
            }
            let target = directory.appendingPathComponent(SignersFile.fileName)
            guard signature.check == .verified, let proof = signature.proof,
                  let expected = try? SignersFile.content(of: proof) else {
                try? FileManager.default.removeItem(at: target)
                continue
            }
            if !Self.isRegularFile(target) || (try? Data(contentsOf: target)) != expected {
                try? FileManager.default.removeItem(at: target)
                do {
                    try faultWritingSignersFile?()
                    try expected.write(to: target, options: .atomic)
                } catch {
                    // Nothing is left in its place once the removal worked: the guest sees the version unsigned until
                    // a scan writes it.
                }
            }
        }
        if !checked.isEmpty {
            try? recordAll { entry in
                if let signature = checked[entry.id] { entry.report?.signature = signature }
            }
        }
    }

    /// Whether `url` is a regular file itself, not a folder or a link.
    private static func isRegularFile(_ url: URL) -> Bool {
        (try? FileManager.default.attributesOfItem(atPath: url.path)[.type] as? FileAttributeType) == .typeRegular
    }

    /// A set's digest from the files its entry names (ADR-0048, decision 6); nil for one APK.
    static func setDigest(of entry: LibraryEntry) -> String? {
        guard let base = entry.base, !entry.splits.isEmpty else { return nil }
        return try? SetDigest.of(([base] + entry.splits).map { ($0.splitName, $0.sha256) })
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
        let candidates = [setDigest(of: entry).map { String($0.prefix(16)) }, entry.base.map { String($0.sha256.prefix(16)) },
                          iconFolder]
        for case let folder? in candidates where isVersionFolder(folder) {
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
        // One autorelease pool per chunk: FileHandle's buffers would otherwise pile up to the file's size.
        while let chunk = try autoreleasepool(invoking: { try handle.read(upToCount: 4 << 20) }), !chunk.isEmpty {
            hasher.update(data: chunk)
            done += UInt64(chunk.count)
            progress(Double(done) / Double(max(size, 1)))
        }
        return hasher.finalize().map { String(format: "%02x", $0) }.joined()
    }
}
