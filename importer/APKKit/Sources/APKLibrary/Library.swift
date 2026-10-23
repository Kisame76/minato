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
    /// Something that is neither a folder nor a link stands where the app's OBB folder belongs, and stays there
    /// (ADR-0049 c2, the lead's reviewer).
    case obbFolderIsNoFolder(String)
    /// The same for the app's Download folder, the root of the guest's Download mount (ADR-0057 step b).
    case downloadFolderIsNoFolder(String)
}

extension LibraryError: LocalizedError {
    /// A reason for the app to show after "could not be added:" or "could not be imported:". An `.invalid` entry
    /// keeps the error's description instead, which the app reads.
    public var errorDescription: String? {
        switch self {
        case .unsafePackageName(let name): "“\(name)” is not a valid package name"
        case .versionFolderInUse(let folder): "its version folder \(folder) is still in use"
        case .nothingToImport: "it has no APK or OBB file"
        case .obbFolderIsNoFolder(let package): "the OBB folder of \(package) is not a folder"
        case .downloadFolderIsNoFolder(let package): "the Download folder of \(package) is not a folder"
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
    /// During one `importUnit`, each APK taken out of a container with the name it shows, "<container>/<file>", the
    /// container as the file the user picked (ADR-0049 a3), and the container itself, whose OBBs go with its sets (c2).
    private var fromContainers: [URL: (name: String, source: SourceFile, container: URL)] = [:]
    /// The converter's check mode, which the import asks about every native library, certificate or not (ADR-0055 a2).
    var checker: (any ELFRefusing)?
    /// The guest's public libraries (its public.libraries.txt), which the NEEDED names of a library are looked up in
    /// besides the app's own (ADR-0055 a3); without it NEEDED names are not checked.
    var publicLibraries: Set<String>?
    /// The guest's export table (ADR-0055 a4), against which the undefined symbols of every library are bound.
    var exportTable: ExportTable?
    /// The SHA-256 of the texts the two lists above were read from (ADR-0055 decision 5).
    var listDigests = Compatibility.Checks()

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
    /// import also those left over from last time. A pick still being taken over is none yet; one with nothing but OBBs
    /// is one too (ADR-0049 d); any other file may be a container, whatever its extension (ADR-0049 a3). What such an
    /// import left in apps/ goes first.
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
                } else {
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

    /// The SHA-256 of a text's bytes, in lowercase hex.
    private static func digest(_ text: String) -> String {
        SHA256.hash(data: Data(text.utf8)).map { String(format: "%02x", $0) }.joined()
    }

    /// Sets the converter's check mode that the import asks about every native library (ADR-0055 a2); without one,
    /// only the converter's own refusal at signing counts, as before.
    public func use(checker: (any ELFRefusing)?) {
        self.checker = checker
    }

    /// Sets the guest's public libraries, the content of its public.libraries.txt, so that the import checks the
    /// NEEDED names of every native library (ADR-0055 a3).
    public func use(publicLibraries text: String?) {
        publicLibraries = text.map(NativeFindings.publicLibraries(from:))
        listDigests.publicLibraries = text.map(Self.digest)
    }

    /// Sets the guest's export table, the content of its export-table.txt, so that the import binds the undefined
    /// symbols of every native library (ADR-0055 a4); throws for a text that is no table, and keeps the old one.
    /// Without it symbols are not checked.
    public func use(exportTable text: String?) throws {
        exportTable = try text.map { try ExportTable(text: $0) }
        listDigests.exportTable = text.map(Self.digest)
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

    /// `importFile`, with "Replace" answered for the versions `answering` takes, the file's SHA-256 where a unit has
    /// read it already, and what its container adds (ADR-0049 c2).
    private func importFile(_ url: URL, answering: (AskingVersion) -> Bool, signing: ImportSigning?, sha256 known: String? = nil,
                            extras: Extras = Extras(), progress: @Sendable (Double) -> Void) throws -> ImportResult {
        var source = try sourceFile(for: url)
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
                id: "file:\(displayName(of: url))", displayName: displayName(of: url), state: .invalid(String(describing: error)),
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
            },
            extras: extras)
        return try install(new, answering: answering, signing: signing, discarding: url, progress: progress)
    }

    /// What a container adds to the version of its APKs (ADR-0049 c2): an XAPK's OBBs of its package, each taken out
    /// to `url` with its digest, its checked name and its path in the container, and what it leaves out.
    private struct Extras {
        var obbs: [(url: URL, file: PackageFile, name: OBBName, path: String)] = []
        /// A bundletool set's later pack slices for the version's packs/, each checked against the set and named as
        /// bundletool names it (ADR-0049 decision 2).
        var packs: [(url: URL, file: PackageFile)] = []
        var leftOut = LeftOut()
    }

    /// What an import leaves out of a container, which the entries it makes name (ADR-0049 c2): the container's own
    /// list as its reader names it, already capped, and then the import's, capped the same way, so each last name
    /// counts the rest of its own list (the lead's reviewer).
    private struct LeftOut {
        var byContainer: [IgnoredFile] = []
        var byImport: [IgnoredFile] = []

        /// Both lists, for the entry; nil when nothing is left out.
        var named: [IgnoredFile]? {
            let named = byContainer + IgnoredFile.capped(byImport)
            return named.isEmpty ? nil : named
        }
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
        var extras = Extras()
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
        let first = UpdatePolicy.decide(installed: installedVersion, new: new.version(state))
        var decision = first
        // A container's OBB under an installed one's name with other bytes would change the app's content, so the import
        // asks first, and the default keeps the installed file and version (ADR-0049 c2, the lead's rule). A fresh
        // install has none to keep.
        let keepingVersion = installed?.report?.versionCode == report.versionCode
        var plan = planOBBs(new.extras, package: package, recorded: installed?.obbs ?? [], keepingVersion: keepingVersion,
                            replacing: installed == nil)
        let installsOBBs = switch first {
        case .reject: false
        case .unchanged: installed?.state != .incompatible
        default: state != .incompatible
        }
        if installsOBBs, !plan.conflicts.isEmpty {
            decision = if case .ask(let warnings) = first { .ask(warnings.union([.otherOBB])) } else { .ask([.otherOBB]) }
        }
        // "Replace" answers the question as it was shown (F7): one that asks otherwise now asks again.
        let replacing = if case .ask(let warnings) = decision {
            answering(AskingVersion(package: package, versionCode: report.versionCode, warnings: warnings))
        } else {
            false
        }
        if case .ask = decision {
            guard replacing else {
                progress(1)
                guard let installed else {
                    // A fresh install that asks (ADR-0048, step b3): nothing is stored, and the entry is the one it
                    // would make.
                    let entry = LibraryEntry(id: package, displayName: report.label, state: state, report: report, base: nil,
                                             splits: [], obbs: [], source: new.source, iconFile: nil, importedAt: Date())
                    return ImportResult(entry: entry, decision: decision, report: report,
                                        uncheckedFiles: new.version(state).uncheckedFiles)
                }
                return ImportResult(entry: installed, decision: decision, report: report,
                                    uncheckedFiles: new.version(state).uncheckedFiles, otherOBBs: plan.conflicts)
            }
            if !plan.conflicts.isEmpty {
                plan = planOBBs(new.extras, package: package, recorded: installed?.obbs ?? [],
                                keepingVersion: keepingVersion, replacing: true)
            }
            // The answer goes on as the first decision would have, and one that asked for itself installs or updates.
            decision = if case .ask = first { installed == nil ? .install : .update } else { first }
        }
        switch (decision, installed) {
        case (.unchanged, let installed?)
            where installed.state != .incompatible
                && (!new.extras.obbs.isEmpty || !new.extras.packs.isEmpty || new.extras.leftOut.named != nil):
            // The same version from a container: its OBBs go in through the installed version's folder (ADR-0049 c2),
            // and its later packs replace that folder's packs/ (decision 2), unless it cannot run, as an incompatible
            // version installs none. The OBB folder is checked first, so a refusal changes nothing.
            var entry = installed
            try paths.prepareData(for: package)
            let staging = try stageOBBs(plan, from: new.extras, in: paths.versionDirectory(for: installed))
            if let packs = try placePacks(new.extras.packs, in: paths.versionDirectory(for: installed)) { entry.packs = packs }
            entry.obbs = plan.obbs
            entry.ignoredFiles = plan.leftOut.named
            try store(entry)
            try moveOBBs(plan, of: package, from: staging)
            try? FileManager.default.removeItem(at: staging)
            return finish(ImportResult(entry: entry, decision: decision, report: report, installedOBBs: plan.moves))
        case (.unchanged, let installed?), (.reject, let installed?):
            return finish(ImportResult(entry: installed, decision: decision, report: report))
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

        // A ready app's native code goes into the version's lib/, converted and signed when a certificate is at hand
        // (ADR-0032, decision 4): a NativeActivity app's, and a Java app's JNI libraries (ADR-0039 c5).
        var signedWith: String?
        var containerVersion: UInt32?
        if state == .ready {
            do {
                let installed = try installNativeCode(of: report, from: new.archives, into: versionDirectory,
                                                      signing: signing)
                signedWith = installed.signedWith
                containerVersion = installed.containerVersion
                report.compatibility.issues += installed.findings
                report.compatibility.checks = installed.checks
            } catch let refusal as RefusedLibrary {
                // This version cannot run, so it keeps no code and counts as incompatible: a ready
                // installed version then stays (ADR-0034, decision 1).
                state = .incompatible
                report.compatibility.status = .incompatible
                report.compatibility.issues += refusal.issues
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
        var packs: [PackageFile]?
        // A container's OBBs are staged with the version and move in after the switch (ADR-0049 c2), and its later
        // packs stay in the version's packs/ (decision 2); an incompatible version installs neither.
        if state == .incompatible { plan = OBBPlan(leftOut: new.extras.leftOut, obbs: installed?.obbs ?? []) }
        var staging: URL?
        if state != .incompatible {
            staging = try stageOBBs(plan, from: new.extras, in: versionDirectory)
            for (url, file) in new.files {
                // On APFS the copy is a clone and takes no extra space. Read-only, so that the guest cannot write it
                // (LibraryPaths.makeReadOnly).
                let copy = versionDirectory.appendingPathComponent(file.fileName)
                try fileManager.copyItem(at: url, to: copy)
                try LibraryPaths.makeReadOnly(copy)
            }
            base = new.files.first?.file
            splits = new.files.dropFirst().map(\.file)
            packs = try placePacks(new.extras.packs, in: versionDirectory)
            // Who signed it, for the guest's PackageManager (ADR-0034 decision 2): what the check proved, so an
            // unsigned or unverified APK has none.
            if report.signature.check == .verified, let proof = report.signature.proof {
                let signers = versionDirectory.appendingPathComponent(SignersFile.fileName)
                try SignersFile.content(of: proof).write(to: signers, options: .atomic)
                try LibraryPaths.makeReadOnly(signers)
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
        // importedAt, the OBBs and the data (ADR-0034, decisions 4 and 6). The entry records the OBBs as they are once
        // they moved in, so a crash before that leaves recorded OBBs missing, which the unit's next import, the same
        // version, moves in.
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
        entry.containerVersion = containerVersion
        entry.updatedAt = installed == nil ? nil : Date()
        entry.obbs = plan.obbs
        entry.packs = packs
        entry.ignoredFiles = plan.leftOut.named
        try store(entry)
        removeEverything(in: appDirectory, except: folder)
        if let staging {
            try moveOBBs(plan, of: package, from: staging)
            try? FileManager.default.removeItem(at: staging)
        }
        return finish(ImportResult(entry: entry, decision: decision, report: report, installedOBBs: plan.moves))
    }

    /// What becomes of a container's OBBs in the app's OBB folder (ADR-0049 c2).
    private struct OBBPlan {
        /// The OBBs to move in, each over a file or link of its name.
        var moves: [PackageFile] = []
        /// The installed files of their kinds that go once the new ones are in.
        var removals: [String] = []
        /// The installed OBBs the import would replace with other bytes, by name: it asks first (the lead's rule).
        var conflicts: [String] = []
        /// What the import leaves out: the container's own list, then the OBBs that stay out.
        var leftOut = LeftOut()
        /// What the entry records afterwards.
        var obbs: [PackageFile] = []
    }

    /// Plans `extras`' OBBs against those `recorded` for the entry and the files in its OBB folder, read without following
    /// a link (decisions 4 and 5, the lead's rule). Where the versionCode stays (`keepingVersion`), an OBB of a kind the
    /// app has none of moves in, and an installed one never goes: another of its kind stays out. Where it changes, an
    /// OBB replaces an older one of its kind, and a newer one stays installed. Either way an OBB under an installed
    /// one's name with other bytes replaces it only `replacing`, as the user answered, and is a conflict otherwise; one
    /// whose bytes are there moves nothing, and one recorded but missing, as a crash after the switch leaves it, moves in.
    /// A name the folder holds a folder or anything else but a file or a link under stays the app's, so that OBB stays
    /// out and nothing of its kind goes (the lead's reviewer).
    private func planOBBs(_ extras: Extras, package: String, recorded: [PackageFile], keepingVersion: Bool,
                          replacing: Bool) -> OBBPlan {
        let folder = paths.dataDirectory(for: package).appendingPathComponent("obb", isDirectory: true)
        let present = Self.entries(in: folder)
        let files = Set(present.filter { $0.value == .typeRegular }.keys)
        var plan = OBBPlan(leftOut: extras.leftOut, obbs: recorded)
        for obb in extras.obbs {
            let name = obb.name, fileName = obb.file.fileName
            switch present[fileName] {
            case nil, .typeSymbolicLink?:
                break
            case .typeRegular?:
                if recorded.contains(obb.file) { continue }
                if holds(folder.appendingPathComponent(fileName), obb.file) {
                    plan.obbs = plan.obbs.filter { $0.fileName != fileName } + [obb.file]
                    continue
                }
                guard replacing else {
                    plan.conflicts.append(fileName)
                    continue
                }
            default:
                plan.leftOut.byImport.append(IgnoredFile(path: obb.path, reason: "the app’s OBB folder holds something else under its name"))
                continue
            }
            let others = files.union(recorded.map(\.fileName)).compactMap { OBBName($0) }
                .filter { $0.kind == name.kind && $0.package == package && $0 != name }
            if let newest = others.max(by: { Self.isNewer($1.version, than: $0.version) }) {
                if keepingVersion {
                    plan.leftOut.byImport.append(IgnoredFile(path: obb.path, reason: "the same version keeps the installed \(newest.fileName)"))
                    continue
                }
                if Self.isNewer(newest.version, than: name.version) {
                    plan.leftOut.byImport.append(IgnoredFile(path: obb.path, reason: "it is older than the installed \(newest.fileName)"))
                    continue
                }
            }
            let replaced = Set(others.map(\.fileName) + [fileName])
            plan.moves.append(obb.file)
            plan.removals += others.map(\.fileName).filter(files.contains)
            plan.obbs = plan.obbs.filter { !replaced.contains($0.fileName) } + [obb.file]
        }
        return plan
    }

    /// The names in `folder` with what each is, a link as itself; none where no folder stands there, as where a link
    /// stands in its place, which `prepareData` removes. Read with lstat, as `prepareData` reads it.
    private static func entries(in folder: URL) -> [String: FileAttributeType] {
        var status = stat()
        guard lstat(folder.path, &status) == 0, status.st_mode & S_IFMT == S_IFDIR,
              let names = try? FileManager.default.contentsOfDirectory(atPath: folder.path) else { return [:] }
        var entries: [String: FileAttributeType] = [:]
        for name in names {
            let attributes = try? FileManager.default.attributesOfItem(atPath: folder.appendingPathComponent(name).path)
            entries[name] = attributes?[.type] as? FileAttributeType ?? FileAttributeType.typeUnknown
        }
        return entries
    }

    /// Whether the file at `url` holds `file`'s bytes: its size, then its SHA-256.
    private func holds(_ url: URL, _ file: PackageFile) -> Bool {
        guard let size = try? Self.sourceFile(for: url).size, size == file.size else { return false }
        return (try? hash(url, size: size) { _ in }) == file.sha256
    }

    /// Puts a version's later pack slices into its packs/ under their names (ADR-0049 decision 2): APFS clones,
    /// read-only as the version's APKs are, in a folder left out of backups as the OBB folder is (decision 9). They are
    /// written into packs.partial first and come in whole, swapped with the packs/ there, so the packs an entry names
    /// never lie half written (App's review); the same version's packs are replaced by those its container brings.
    /// Returns what it put there, nil for none.
    private func placePacks(_ packs: [(url: URL, file: PackageFile)], in version: URL) throws -> [PackageFile]? {
        guard !packs.isEmpty else { return nil }
        let folder = version.appendingPathComponent("packs", isDirectory: true)
        var staging = version.appendingPathComponent("packs.partial", isDirectory: true)
        try? FileManager.default.removeItem(at: staging)
        try FileManager.default.createDirectory(at: staging, withIntermediateDirectories: true)
        var excluded = URLResourceValues()
        excluded.isExcludedFromBackup = true
        try staging.setResourceValues(excluded)
        for pack in packs {
            let copy = staging.appendingPathComponent(pack.file.fileName)
            try FileManager.default.copyItem(at: pack.url, to: copy)
            try LibraryPaths.makeReadOnly(copy)
        }
        if FileManager.default.fileExists(atPath: folder.path) {
            guard renamex_np(staging.path, folder.path, UInt32(RENAME_SWAP)) == 0 else {
                throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
            }
            try? FileManager.default.removeItem(at: staging) // the packs it replaced
        } else {
            try FileManager.default.moveItem(at: staging, to: folder)
        }
        return packs.map(\.file)
    }

    /// Stages the OBBs `plan` moves in as APFS clones in `version`'s obb/, after whatever a crash left there (App's
    /// review), excluded from backups as the OBB folder is (the lead's reviewer). Returns the staging folder.
    private func stageOBBs(_ plan: OBBPlan, from extras: Extras, in version: URL) throws -> URL {
        var staging = version.appendingPathComponent("obb", isDirectory: true)
        try? FileManager.default.removeItem(at: staging)
        if !plan.moves.isEmpty {
            try FileManager.default.createDirectory(at: staging, withIntermediateDirectories: true)
            var excluded = URLResourceValues()
            excluded.isExcludedFromBackup = true
            try staging.setResourceValues(excluded)
        }
        for obb in extras.obbs where plan.moves.contains(obb.file) {
            try FileManager.default.copyItem(at: obb.url, to: staging.appendingPathComponent(obb.file.fileName))
        }
        return staging
    }

    /// Carries out `plan` in `package`'s OBB folder, never through a link (the lead's reviewer): the folder and `source`
    /// are opened as themselves, the OBBs move in first, each from its name in `source` (`names`, else its own) by
    /// rename over the file or link of its name, and only then do the replaced files of their kinds go, so an
    /// interruption leaves both, never neither.
    private func moveOBBs(_ plan: OBBPlan, of package: String, from source: URL, names: [String: String] = [:]) throws {
        try paths.prepareData(for: package)
        let target = try Self.openFolder(paths.dataDirectory(for: package).appendingPathComponent("obb", isDirectory: true))
        defer { close(target) }
        if !plan.moves.isEmpty {
            let from = try Self.openFolder(source)
            defer { close(from) }
            for file in plan.moves {
                guard renameatx_np(from, names[file.fileName] ?? file.fileName, target, file.fileName, 0) == 0 else {
                    throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
                }
            }
        }
        try faultBetweenTheOBBMoves?()
        for name in plan.removals { _ = unlinkat(target, name, 0) }
    }

    /// `folder` opened as itself: a link in its place fails (ELOOP).
    private static func openFolder(_ folder: URL) throws -> Int32 {
        let descriptor = Darwin.open(folder.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard descriptor >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        return descriptor
    }

    /// Whether OBB version `a` is newer than `b`, each an int as Android writes one (`OBBName`).
    static func isNewer(_ a: String, than b: String) -> Bool {
        (Int(a) ?? -1) > (Int(b) ?? -1)
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
    ///
    /// A unit's file that is no APK by its content is a container (ADR-0049 a3): its APKs are taken out next to it, into
    /// ".<file>.contents/", and join the unit's own under names "<file>/<n>.apk", with the container as the file the
    /// user picked; a container Minato refuses becomes an entry with its reason. A file named .apk that opens as no
    /// ZIP stays an APK, so a damaged APK keeps its entry. The containers and their folders go with the unit.
    ///
    /// An XAPK's OBBs are read with the APKs, and each goes with the set of its package from the same XAPK (ADR-0049
    /// c2); one without such a set is named on the XAPK's entries, beside what the XAPK itself left out.
    @discardableResult
    public func importUnit(_ unit: URL, replacing answered: Set<AskingVersion> = [], assigning: [String: String] = [:],
                           signing: ImportSigning? = nil, progress: @Sendable (Double) -> Void = { _ in }) throws -> [ImportResult] {
        var isFolder: ObjCBool = false
        _ = FileManager.default.fileExists(atPath: unit.path, isDirectory: &isFolder)
        var urls: [URL] = []
        var obbs: [(url: URL, name: OBBName, path: String, container: URL)] = []
        var packs: [(url: URL, pack: String, path: String, container: URL)] = []
        var leftOut: [URL: LeftOut] = [:]
        var refused: [(file: URL, error: ContainerError)] = []
        var taken: [URL] = []
        var picked: [URL] = []
        fromContainers = [:]
        defer { fromContainers = [:] }
        if isFolder.boolValue {
            for file in try Self.files(inUnit: unit) {
                if Self.isOBB(file) {
                    picked.append(file)
                    continue
                }
                switch try expand(file, in: unit) {
                case .apk:
                    urls.append(file)
                case .container(let apks, let containerOBBs, let containerPacks, let named, let folders):
                    let source = try Self.sourceFile(for: file)
                    for (url, name) in apks {
                        urls.append(url)
                        fromContainers[url] = ("\(file.lastPathComponent)/\(name)", source, file)
                    }
                    obbs += containerOBBs.map { ($0.url, $0.name, $0.path, file) }
                    packs += containerPacks.map { ($0.url, $0.pack, $0.path, file) }
                    leftOut[file] = named
                    // The folders go before their container (App's review): a crash between them leaves a container
                    // to take out again, never a hidden folder no unit lists.
                    taken += folders + [file]
                case .refused(let error):
                    refused.append((file, error))
                    taken.append(file)
                }
            }
        } else {
            urls = [unit]
        }
        let sizes = try urls.map { try Self.sourceFile(for: $0).size }
        let obbSizes = try obbs.map { try Self.sourceFile(for: $0.url).size }
        let packSizes = try packs.map { try Self.sourceFile(for: $0.url).size }
        let pickedSizes = try picked.map { try Self.sourceFile(for: $0).size }
        let bytes: UInt64 = (sizes + obbSizes + packSizes + pickedSizes).reduce(0, +)
        let total = Double(max(bytes, 1))
        var read: UInt64 = 0
        var digests: [URL: String] = [:]
        var unreadable: [URL] = []
        var members: [SetMember] = []
        var files: [String: (url: URL, archive: ZipArchive)] = [:]
        var later: [(url: URL, member: SetMember, size: UInt64)] = []
        progress(0)
        for (url, size) in zip(urls, sizes) {
            let sha256 = try hash(url, size: size) { progress(0.5 * (Double(read) + $0 * Double(size)) / total) }
            read += size
            digests[url] = sha256
            guard let archive = try? ZipArchive(url: url),
                  let member = try? SetMember.read(archive, fileName: displayName(of: url), sha256: sha256) else {
                unreadable.append(url)
                continue
            }
            files[member.fileName] = (url, archive)
            // A later pack's slice, as bundletool writes it (no versionCode, the app's as versionName), may be a pack
            // of its set, not a split of it: a store's XAPK or APKM or a folder of APKs may carry one (ADR-0050,
            // step 5).
            if member.splitName != nil, member.versionCode == 0, member.versionName != nil {
                later.append((url, member, size))
                continue
            }
            members.append(member)
        }
        // An entry records each OBB with its SHA-256 (ADR-0049 c2).
        var obbFiles: [(url: URL, file: PackageFile, name: OBBName, path: String, container: URL)] = []
        for (obb, size) in zip(obbs, obbSizes) {
            let sha256 = try hash(obb.url, size: size) { progress(0.5 * (Double(read) + $0 * Double(size)) / total) }
            read += size
            obbFiles.append((obb.url, PackageFile(fileName: obb.name.fileName, splitName: nil, size: size, sha256: sha256),
                             obb.name, obb.path, obb.container))
        }
        // Each later pack slice is read as an APK, so that its set can check it (ADR-0049 decision 2).
        var packSlices: [(url: URL, slice: SetMember?, size: UInt64, pack: String, path: String, container: URL)] = []
        for (pack, size) in zip(packs, packSizes) {
            let sha256 = try hash(pack.url, size: size) { progress(0.5 * (Double(read) + $0 * Double(size)) / total) }
            read += size
            let slice = (try? ZipArchive(url: pack.url)).flatMap { try? SetMember.read($0, fileName: pack.path, sha256: sha256) }
            packSlices.append((pack.url, slice, size, pack.pack, pack.path, pack.container))
        }
        var pickedFiles: [(url: URL, size: UInt64, sha256: String)] = []
        for (url, size) in zip(picked, pickedSizes) {
            let sha256 = try hash(url, size: size) { progress(0.5 * (Double(read) + $0 * Double(size)) / total) }
            read += size
            pickedFiles.append((url, size, sha256))
        }
        // A later slice belongs to the base of its package whose versionCode, as bundletool writes it, is the slice's
        // versionName; a base without a versionCode never is one's, and a slice with no such base stays a member, as it
        // was before (a set that has no versionCode keeps its splits).
        // Where the package has versioned bases but none of the slice's version, the first is its set, and refuses it
        // by name.
        let baseOf = { (slice: SetMember) in
            let bases = members.filter { $0.isBase && $0.packageName == slice.packageName && $0.versionCode != 0 }
            return bases.first { String(Int32(truncatingIfNeeded: $0.versionCode)) == slice.versionName } ?? bases.first
        }
        let laterSlices = later.filter { baseOf($0.member) != nil }
        members += later.filter { baseOf($0.member) == nil }.map(\.member)
        let groups = SetGrouping.byPackageAndVersion(members)
        // Each OBB goes with the set of its package from its own container; one without is named there.
        var extras = [Extras](repeating: Extras(), count: groups.count)
        var containers: [URL?] = []
        for group in groups {
            var container: URL?
            for member in group where container == nil {
                if let url = files[member.fileName]?.url { container = fromContainers[url]?.container }
            }
            containers.append(container)
        }
        for obb in obbFiles {
            let package = obb.name.package
            if let index = groups.indices.first(where: { containers[$0] == obb.container && groups[$0][0].packageName == package }) {
                extras[index].obbs.append((obb.url, obb.file, obb.name, obb.path))
            } else {
                leftOut[obb.container, default: LeftOut()].byImport.append(IgnoredFile(path: obb.path, reason: Self.noAPK(for: package)))
            }
        }
        // Each later pack slice goes with the set from its own container, checked against it and named as bundletool
        // names it (ADR-0049 decision 2); one it refuses is named there.
        for slice in packSlices {
            guard let index = groups.indices.first(where: { containers[$0] == slice.container }) else { continue }
            let checked = Self.packFile(slice.slice, pack: slice.pack, size: slice.size, of: groups[index],
                                        taken: extras[index].packs.map(\.file.fileName))
            if let file = checked.file {
                extras[index].packs.append((slice.url, file))
            } else {
                leftOut[slice.container, default: LeftOut()].byImport.append(IgnoredFile(path: slice.path, reason: checked.reason))
            }
        }
        // The same for the later slices a set carries itself: kept in packs/ under their pack's name, or named as left
        // out, in the container they came in, else on the entry's own files (ADR-0050, step 5).
        var onEntry: [(index: Int, file: IgnoredFile)] = []
        for slice in laterSlices {
            let container = fromContainers[slice.url]?.container
            // Its set is the one of its base, which `later` kept only for a slice that has one.
            // The group is found by the base's own package and versionCode, which byPackageAndVersion keys it by.
            guard let base = baseOf(slice.member), let index = groups.firstIndex(where: {
                $0[0].packageName == base.packageName && $0[0].versionCode == base.versionCode
            }) else { continue }
            let split = slice.member.splitName ?? ""
            let pack = split.components(separatedBy: ".config.")[0]
            let checked = Self.packFile(slice.member, pack: pack, size: slice.size, of: groups[index],
                                        taken: extras[index].packs.map(\.file.fileName))
            if let file = checked.file {
                extras[index].packs.append((slice.url, file))
            } else if let container, containers[index] == container {
                leftOut[container, default: LeftOut()].byImport.append(
                    IgnoredFile(path: slice.member.fileName, reason: checked.reason))
            } else {
                // Its container's names go on that container's first entry, which is not this set's where a group
                // merged several containers: the set's own entry names it, after those are placed below.
                onEntry.append((index, IgnoredFile(path: slice.member.fileName, reason: checked.reason)))
            }
        }
        // What a container leaves out goes on the first of its entries only, never copied onto each (the lead's
        // reviewer); each set's own names go on its own.
        var named = Set<URL>()
        for index in groups.indices {
            if let container = containers[index], named.insert(container).inserted {
                extras[index].leftOut = leftOut[container] ?? LeftOut()
            }
        }
        for (index, file) in onEntry { extras[index].leftOut.byImport.append(file) }
        let parts = Double(max(unreadable.count + groups.count + picked.count, 1))
        var results = try refused.map { try refuse($0.file, $0.error) }
        for url in unreadable {
            let part = Double(results.count)
            results.append(try importFile(url, answering: answered.contains, signing: signing, sha256: digests[url]) {
                progress(0.5 + 0.5 * (part + $0) / parts)
            })
        }
        for (index, group) in groups.enumerated() {
            if group.count == 1, group[0].isBase, let url = files[group[0].fileName]?.url {
                let part = Double(results.count)
                results.append(try importFile(url, answering: answered.contains, signing: signing, sha256: digests[url],
                                              extras: extras[index]) {
                    progress(0.5 + 0.5 * (part + $0) / parts)
                })
            } else {
                results.append(try importSet(group, files: files, answering: answered.contains, signing: signing,
                                             extras: extras[index]))
                progress(0.5 + 0.5 * Double(results.count) / parts)
            }
        }
        // The unit's own OBBs, once none of its sets waits for an answer (ADR-0049 decision 5): each goes to the app its
        // name gives, or to the one the user named for it, and one without either waits for the user.
        let asks = results.contains { if case .ask = $0.decision { true } else { false } }
        var placed: [URL] = []
        if !asks {
            for obb in pickedFiles {
                guard let target = target(forPicked: obb.url.lastPathComponent, assigning: assigning) else { continue }
                results.append(try installPicked(obb, as: target.name, into: target.entry))
                placed.append(obb.url)
                progress(0.5 + 0.5 * Double(results.count) / parts)
            }
        }
        // The unit goes once nothing in it waits for an answer, its containers and their folders with it.
        if !asks {
            for url in urls + taken + placed { try? FileManager.default.removeItem(at: url) }
            rmdir(unit.path)
        }
        progress(1)
        return results
    }

    /// The unit's OBBs no app takes yet, by file name, for App's question (ADR-0049 decision 5); none while a set of the
    /// unit waits for its own answer, as its OBBs wait with it.
    public func obbsWaitingForAnApp(inUnit unit: URL) -> [String] {
        guard let files = try? Self.files(inUnit: unit), files.allSatisfy(Self.isOBB) else { return [] }
        return files.map(\.lastPathComponent).filter { target(forPicked: $0, assigning: [:]) == nil }
    }

    /// "Don’t Import" for one picked OBB of a unit (ADR-0049 d, App's review): that file goes, found among the unit's
    /// own files, so the name leads nowhere else, and the unit goes with its last file; its other OBBs keep waiting.
    public func discardPickedOBB(_ fileName: String, inUnit unit: URL) {
        guard Self.isDirectChild(unit, of: paths.incoming),
              let file = (try? Self.files(inUnit: unit))?.first(where: { $0.lastPathComponent == fileName && Self.isOBB($0) })
        else { return }
        try? FileManager.default.removeItem(at: file)
        rmdir(unit.path) // only once nothing else waits in it
    }

    /// The apps a picked OBB can go to, by their names as App's question lists them: those ready or needing their
    /// splits (the lead), never an incompatible one or a file's entry, which is invalid, nor one whose versionCode names
    /// no OBB (App's review), which could not take the file.
    public func appsForOBBs() -> [LibraryEntry] {
        appsTakingOBBs().filter { Self.obbName(for: $0, picked: "main.obb") != nil }
            .sorted { $0.displayName.localizedStandardCompare($1.displayName) == .orderedAscending }
    }

    /// The apps that take a picked OBB by its own name: those ready or needing their splits.
    private func appsTakingOBBs() -> [LibraryEntry] {
        entries.filter { $0.state == .ready || $0.state == .needsSplits }
    }

    /// The name a picked file takes in `entry`'s app (decision 5): `main.<its versionCode>.<package>.obb`, or `patch.…`
    /// where the picked name starts with "patch", with the int versionCode Android writes, the long one's lower half
    /// (`PackageInfo.versionCode`); nil where that gives no OBB name, as `OBBName` checks it.
    private static func obbName(for entry: LibraryEntry, picked fileName: String) -> OBBName? {
        let version = Int32(truncatingIfNeeded: entry.report?.versionCode ?? -1)
        return OBBName("\(fileName.hasPrefix("patch") ? "patch" : "main").\(version).\(entry.id).obb")
    }

    /// The app a picked OBB goes to and its name there (decision 5): the app the user named for it among those
    /// `appsForOBBs` lists, under the name `obbName(for:picked:)` makes, else the app its own name gives, ready or
    /// needing its splits, under that name. nil while it waits for the user.
    private func target(forPicked fileName: String, assigning: [String: String]) -> (entry: LibraryEntry, name: OBBName)? {
        if let package = assigning[fileName], let entry = appsForOBBs().first(where: { $0.id == package }),
           let name = Self.obbName(for: entry, picked: fileName) {
            return (entry, name)
        }
        if let name = OBBName(fileName), let entry = appsTakingOBBs().first(where: { $0.id == name.package }) {
            return (entry, name)
        }
        return nil
    }

    /// Installs a picked OBB for `entry`'s app under `name` (decision 5): it replaces the installed one of its kind,
    /// newer or older, and a file of its own name with other bytes too, as the pick is the user's own act (the lead);
    /// the same bytes move nothing. As in c2 the target name is read with lstat first, so something else there refuses
    /// the file, the entry records it before the unit's clone moves in by rename through no-follow descriptors, and the
    /// replaced files go after. The result is the app's entry, `.unchanged`, with what came in and what it replaced.
    private func installPicked(_ obb: (url: URL, size: UInt64, sha256: String), as name: OBBName,
                               into entry: LibraryEntry) throws -> ImportResult {
        let package = entry.id
        try paths.prepareData(for: package)
        let folder = paths.dataDirectory(for: package).appendingPathComponent("obb", isDirectory: true)
        let present = Self.entries(in: folder)
        let file = PackageFile(fileName: name.fileName, splitName: nil, size: obb.size, sha256: obb.sha256)
        var result = ImportResult(entry: entry, decision: .unchanged, report: entry.report)
        var replaced: [String] = []
        switch present[name.fileName] {
        case nil, .typeSymbolicLink?:
            break
        case .typeRegular?:
            if entry.obbs.contains(file) { return result }
            if holds(folder.appendingPathComponent(name.fileName), file) {
                result.entry.obbs = entry.obbs.filter { $0.fileName != file.fileName } + [file]
                try store(result.entry)
                return result
            }
            replaced.append(name.fileName)
        default:
            result.refusedOBBs = [IgnoredFile(path: obb.url.lastPathComponent,
                                              reason: "something else is in the app’s expansion file folder under that name")]
            return result
        }
        let files = Set(present.filter { $0.value == .typeRegular }.keys)
        let others = files.union(entry.obbs.map(\.fileName)).compactMap { OBBName($0) }
            .filter { $0.kind == name.kind && $0.package == package && $0 != name }
        let removals = others.map(\.fileName).filter(files.contains).sorted()
        let gone = Set(others.map(\.fileName) + [file.fileName])
        result.entry.obbs = entry.obbs.filter { !gone.contains($0.fileName) } + [file]
        try store(result.entry)
        try moveOBBs(OBBPlan(moves: [file], removals: removals), of: package, from: obb.url.deletingLastPathComponent(),
                     names: [file.fileName: obb.url.lastPathComponent])
        result.installedOBBs = [file]
        result.replacedOBBs = replaced + removals
        return result
    }

    /// What a unit's file gives (ADR-0049 a3 and c2).
    private enum Expanded {
        case apk
        /// The APKs taken out, each with its name, an XAPK's OBBs taken out, each with its checked name and its path in
        /// the container, a bundletool set's later pack slices taken out, each with its pack and its path, what the
        /// container leaves out, and the folders they went into.
        case container(apks: [(url: URL, name: String)], obbs: [(url: URL, name: OBBName, path: String)],
                       packs: [(url: URL, pack: String, path: String)], leftOut: LeftOut, folders: [URL])
        case refused(ContainerError)
    }

    /// A unit's file as an APK, as a container's APKs taken out into ".<file>.contents/" by ADR-0049's extraction, or
    /// as the reason Minato refuses it. Errors that are no container's, such as a full disk, stop the import, and the
    /// unit waits.
    ///
    /// An XAPK's OBBs are taken out after its APKs, into ".<file>.obbs/", and only those of a package one of its APKs
    /// names (the lead's reviewer): an XAPK never brings an OBB for another app, and the others are named before
    /// anything of theirs is written (ADR-0049 c2).
    private func expand(_ file: URL, in unit: URL) throws -> Expanded {
        let container: Container
        do {
            container = try Container.open(file)
        } catch let error as ContainerError {
            return Self.isAPK(file) ? .apk : .refused(error)
        }
        let folders = Self.takenOutFolders(of: file)
        do {
            let items: [Container.Item]
            var contents: XAPKContents?
            var packItems: [Container.PackItem] = []
            switch container.kind {
            case .apk: return .apk
            case .xapk:
                contents = try container.xapkContents()
                items = contents?.apks ?? []
            case .bundletool:
                let picked = try container.bundletoolItems(for: BundletoolDevice(device))
                items = picked.items
                packItems = try container.bundletoolPackItems(of: picked.choice)
            case .apkm, .looseSet: items = try container.looseSetItems()
            }
            let apks = try takeOut(items, of: container, into: folders.contents)
            let packs = try takeOut(packItems.map(\.item), of: container, into: folders.packs)
            guard let contents else {
                return .container(apks: apks, obbs: [], packs: zip(packs, packItems).map { ($0.url, $1.pack, $1.path) },
                                  leftOut: LeftOut(), folders: [folders.contents, folders.packs])
            }
            let packages = Set(apks.compactMap { Self.packageName(of: $0.url) })
            let kept = contents.obbs.filter { packages.contains($0.name.package) }
            var leftOut = LeftOut(byContainer: contents.ignored)
            for obb in contents.obbs where !packages.contains(obb.name.package) {
                leftOut.byImport.append(IgnoredFile(path: obb.item.entry.name, reason: Self.noAPK(for: obb.name.package)))
            }
            let obbs = try takeOut(kept.map(\.item), of: container, into: folders.obbs)
            return .container(apks: apks, obbs: zip(obbs, kept).map { ($0.url, $1.name, $1.item.entry.name) }, packs: [],
                              leftOut: leftOut, folders: [folders.contents, folders.obbs])
        } catch let error as ContainerError {
            // A container refused after its APKs were taken out, as for a damaged OBB, keeps none of them.
            try? FileManager.default.removeItem(at: folders.contents)
            return .refused(error)
        }
    }

    /// A later pack slice as its set keeps it in packs/ (ADR-0049 decision 2), under bundletool's name for it,
    /// `<pack>-master.apk` or `<pack>-<suffix>.apk` (`ApkPathManager.java:31-150`, 1.18.3), which Play's libraries read
    /// a slice's split from; or why the set leaves it out. It must be its app's, of the app's version and signers, and a
    /// slice of its pack whose name is a module's, as bundletool allows one, and not one the set took already. Its
    /// signers must be proven as the base's are: a slice Minato cannot verify passes only beside a base it cannot
    /// verify either, as the set then counts as one such APK (App's review), since a certificate's name is easy to copy.
    private static func packFile(_ slice: SetMember?, pack: String, size: UInt64, of group: [SetMember],
                                 taken: [String]) -> (file: PackageFile?, reason: String) {
        guard let slice, let app = group.first(where: \.isBase) ?? group.first else {
            return (nil, "it is no APK Minato can read")
        }
        if slice.packageName != app.packageName { return (nil, "it belongs to \(slice.packageName)") }
        // bundletool gives a later pack's slice no versionCode and the base's versionCode attribute, without its
        // versionCodeMajor, as its versionName, "since their version code is not used by Android"
        // (AssetSlicesGenerator.java:56-76, 1.18.3); its --asset-modules-version-override writes another one there.
        if slice.versionCode != app.versionCode,
           slice.versionCode != 0 || slice.versionName != String(Int32(truncatingIfNeeded: app.versionCode)) {
            if slice.versionCode != 0 {
                return (nil, "its versionCode \(slice.versionCode) is not the app’s \(app.versionCode)")
            }
            guard let name = slice.versionName else { return (nil, "it has no versionCode and no versionName") }
            return (nil, "its versionName \(name) is not the app’s versionCode \(Int32(truncatingIfNeeded: app.versionCode))")
        }
        if slice.certificates.isEmpty || slice.certificates != app.certificates || slice.signature.check.isFailure {
            return (nil, "it is signed by another key than the app")
        }
        if app.signature.check == .verified, slice.signature.check != .verified {
            return (nil, "its signature cannot be verified")
        }
        let isName = { (name: Substring) in
            !name.isEmpty && name.allSatisfy { $0.isASCII && ($0.isLetter || $0.isNumber || $0 == "_") }
        }
        let config = "\(pack).config."
        let name: String
        if isName(Substring(pack)), slice.splitName == pack {
            name = "\(pack)-master.apk"
        } else if isName(Substring(pack)), let split = slice.splitName, split.hasPrefix(config),
                  isName(split.dropFirst(config.count)) {
            name = "\(pack)-\(split.dropFirst(config.count)).apk"
        } else {
            return (nil, "it is no slice of the pack \(pack)")
        }
        // A pack's name is no split's of the set (an install-time pack is a split), and names that differ in case only
        // are one file on APFS, as SetChecks takes them.
        // The pack's name, in any case, is no split's either (a config split of it needs the split itself).
        let lowered = pack.lowercased()
        if let split = group.compactMap(\.splitName).first(where: {
            let name = $0.lowercased()
            return name == lowered || name == slice.splitName?.lowercased()
        }) {
            return (nil, "the set has a split \(split)")
        }
        guard !taken.contains(where: { $0.lowercased() == name.lowercased() }) else {
            return (nil, "the set has another \(name)")
        }
        return (PackageFile(fileName: name, splitName: slice.splitName, size: size, sha256: slice.sha256), "")
    }

    /// The folders next to a unit's container that its APKs, its OBBs and its later pack slices are taken out into
    /// (ADR-0049 a3, c2 and decision 2): hidden, so the unit's files never list them and no picked file shares their
    /// names.
    private static func takenOutFolders(of container: URL) -> (contents: URL, obbs: URL, packs: URL) {
        let unit = container.deletingLastPathComponent(), name = container.lastPathComponent
        return (unit.appendingPathComponent(".\(name).contents", isDirectory: true),
                unit.appendingPathComponent(".\(name).obbs", isDirectory: true),
                unit.appendingPathComponent(".\(name).packs", isDirectory: true))
    }

    /// `items` taken out of `container` into `folder` by ADR-0049's extraction, each with its name. A folder an earlier
    /// import took out whole, while a question waited, is taken as it is; a ".partial" one goes. No items need no folder.
    private func takeOut(_ items: [Container.Item], of container: Container, into folder: URL) throws -> [(url: URL, name: String)] {
        let files = items.map { (url: folder.appendingPathComponent($0.fileName), name: $0.fileName) }
        if !files.allSatisfy({ FileManager.default.fileExists(atPath: $0.url.path) }) {
            try? FileManager.default.removeItem(at: folder)
            try container.extract(items, into: folder)
        }
        return files
    }

    /// The package an APK names, read as `SetMember.read` reads it; nil for one Minato cannot read.
    private static func packageName(of apk: URL) -> String? {
        guard let archive = try? ZipArchive(url: apk),
              let xml = try? archive.data(named: "AndroidManifest.xml", maxSize: 16 << 20) else { return nil }
        return try? AndroidManifest(xml: BinaryXML.parse(xml)).packageName
    }

    /// Why an OBB stays out that no set of its package from its own container takes.
    private static func noAPK(for package: String) -> String {
        "it is for \(package), which has no APK here"
    }

    /// An `.invalid` entry of a container Minato refuses; the app shows its reason as it is (ADR-0049 a3).
    private func refuse(_ file: URL, _ error: ContainerError) throws -> ImportResult {
        var entry = LibraryEntry(
            id: "file:\(file.lastPathComponent)", displayName: file.lastPathComponent, state: .invalid(error.refusal),
            report: nil, base: nil, splits: [], obbs: [], source: try Self.sourceFile(for: file), iconFile: nil,
            importedAt: Date())
        entry.refusal = error.refusal
        try store(entry)
        return ImportResult(entry: entry, decision: .install, report: nil)
    }

    /// The name an APK shows: "<container>/<file>" for one taken out of a container, else its own.
    private func displayName(of url: URL) -> String {
        fromContainers[url]?.name ?? url.lastPathComponent
    }

    /// The file the user picked for an APK: its container for one taken out of a container, else the APK.
    private func sourceFile(for url: URL) throws -> SourceFile {
        try fromContainers[url]?.source ?? Self.sourceFile(for: url)
    }

    /// One set of a unit (ADR-0048, decisions 3 to 7): the base with its chosen splits, or splits without their base,
    /// which join the installed version.
    private func importSet(_ group: [SetMember], files: [String: (url: URL, archive: ZipArchive)],
                           answering: (AskingVersion) -> Bool, signing: ImportSigning?, extras: Extras) throws -> ImportResult {
        /// An `.invalid` entry of `file` for a set that is not installed. The app shows `reason` as it is, as the entry's
        /// `refusal`, unless it is an error's description.
        func refuse(_ reason: String, file: SetMember, isDescription: Bool = false) throws -> ImportResult {
            var entry = LibraryEntry(
                id: "file:\(file.fileName)", displayName: file.fileName, state: .invalid(reason), report: nil, base: nil,
                splits: [], obbs: [], source: try sourceFile(for: files[file.fileName]!.url), iconFile: nil,
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
        var source = try installedSource ?? sourceFile(for: apks[nil]!.url)
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
            },
            extras: extras)
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

    /// The answer "Keep Installed" or "Don’t Import" to a question: the waiting file goes, or a unit's files, each
    /// container's folders before it, as at the end of `importUnit` (ADR-0049 a3 and c2, found by c2's set-check), and
    /// its OBBs with them, as the original stays where the user has it (ADR-0049 d, the lead); the installed version
    /// stays. Everything goes from inside the unit's own folder, a link as a link.
    public func keepInstalled(discarding url: URL) {
        var isFolder: ObjCBool = false
        guard Self.isDirectChild(url, of: paths.incoming),
              FileManager.default.fileExists(atPath: url.path, isDirectory: &isFolder), isFolder.boolValue else {
            discardPickedCopy(url)
            return
        }
        for file in (try? Self.files(inUnit: url)) ?? [] {
            let folders = Self.takenOutFolders(of: file)
            try? FileManager.default.removeItem(at: folders.contents)
            try? FileManager.default.removeItem(at: folders.obbs)
            try? FileManager.default.removeItem(at: folders.packs)
            try? FileManager.default.removeItem(at: file)
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

    /// Runs between an import's OBB moves and its removals; a test throws from it to stand for an interruption.
    private var faultBetweenTheOBBMoves: (@Sendable () throws -> Void)?

    func interruptBetweenTheOBBMoves(with fault: @escaping @Sendable () throws -> Void) {
        faultBetweenTheOBBMoves = fault
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

    /// Removes an app with its data and a Java app's guest /data in Caches (ADR-0039 c5). The file it was installed
    /// from is not touched.
    public func remove(id: String) throws {
        guard entries.contains(where: { $0.id == id }) else { return }
        if !id.hasPrefix("file:"), Self.isSafePackageName(id) {
            try? FileManager.default.removeItem(at: paths.installDirectory(for: id))
            try? FileManager.default.removeItem(at: paths.dataDirectory(for: id))
            try? FileManager.default.removeItem(at: paths.guestDataDirectory(for: id).deletingLastPathComponent())
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
                    try LibraryPaths.makeReadOnly(target)
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

    static func isOBB(_ url: URL) -> Bool {
        url.pathExtension.lowercased() == "obb"
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
