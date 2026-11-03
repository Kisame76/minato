import APKKit
import APKLibrary
import APKSession
import APKSigning
import Foundation
import MachO
import Observation
import WebKit
import os

private let log = Logger(subsystem: "io.github.kisame76.minato", category: "import")

/// One tile in the library grid: an installed entry or a pick that is not imported yet.
struct LibraryItem: Identifiable {
    enum Status {
        case new
        case installing(Double)
        /// Its converted code is being signed with a new certificate (ADR-0023).
        case updating
        case ready
        /// Ready, but the check left these libraries out: the app runs without them (ADR-0055 a2).
        case readyWithout([String])
        case needsSplits
        case incompatible(String)
        case invalid(String)
        /// A set that was not installed, with why, in words to show (ADR-0048 b7b).
        case notInstalled(String)
    }

    let id: String
    let title: String
    let status: Status
    let entry: LibraryEntry?
    let iconURL: URL?
    var isFavorite = false
}

/// A pick that waits in incoming/ (ADR-0048, decision 1): a unit's folder, or a file an older build left there.
struct WaitingUnit {
    let url: URL
    /// Its APKs', containers' and OBBs' names in the order the library reads them (`Library.importUnit`), so the title
    /// stays the same.
    let apks: [String]
    /// Their size together, for the log.
    let size: Int

    init(_ url: URL) {
        self.url = url
        let keys: Set<URLResourceKey> = [.isRegularFileKey, .fileSizeKey]
        let files = (try? FileManager.default.contentsOfDirectory(at: url, includingPropertiesForKeys: Array(keys),
                                                                  options: [.skipsHiddenFiles])) ?? [url]
        // Its APKs, containers (ADR-0049 a3) and OBBs (ADR-0049 d).
        let apks = files
            .filter { (try? $0.resourceValues(forKeys: keys))?.isRegularFile == true }
            .sorted { $0.lastPathComponent.utf8.lexicographicallyPrecedes($1.lastPathComponent.utf8) }
        self.apks = apks.map(\.lastPathComponent)
        size = apks.reduce(0) { $0 + ((try? $1.resourceValues(forKeys: keys))?.fileSize ?? 0) }
    }

    /// Its tile's title: the first APK or container, and how many more the unit holds.
    var title: String {
        guard let first = apks.first else { return url.lastPathComponent }
        let name = url.appendingPathComponent(first).deletingPathExtension().lastPathComponent
        return apks.count > 1 ? "\(name) +\(apks.count - 1)" : name
    }
}

/// A unit whose sets ask first (ADR-0034, decision 1; ADR-0048, F7): until the answer, its files stay in incoming/.
struct UpdateQuestion: Identifiable {
    /// One set or APK of the unit that asks.
    struct Asking {
        /// The installed app, which stays until the answer; nil where nothing of its package is installed.
        let installed: LibraryEntry?
        /// The app's name: the installed one's, or else the new version's.
        let name: String
        /// The new version's report.
        let report: APKReport
        /// Its APKs Minato cannot verify, by the names the user picked.
        let uncheckedFiles: [String]
        /// The installed OBBs it would replace with other contents, by name (ADR-0049 c2).
        let otherOBBs: [String]
        /// What "Replace" answers: this version, as the question shows it.
        let version: AskingVersion

        var warnings: Set<UpdateWarning> { version.warnings }

        /// The set `result` asks about; nil where it does not ask. `installed` tells an installed app from the entry
        /// a fresh install would make.
        init?(_ result: ImportResult, installed: [LibraryEntry]) {
            guard let report = result.report, let version = result.asking else { return nil }
            self.installed = installed.contains(result.entry) ? result.entry : nil
            name = result.entry.displayName
            self.report = report
            uncheckedFiles = result.uncheckedFiles
            otherOBBs = result.otherOBBs
            self.version = version
        }
    }

    let unit: URL
    let sets: [Asking]

    var id: URL { unit }

    /// Whether the question lists an installed app, which "Replace" or "Replace and Install" replaces, so the button is
    /// destructive; "Install" where it lists none.
    var replacesAny: Bool { sets.contains { $0.installed != nil } }

    /// Whether `result` is of a version the question lists.
    func lists(_ result: ImportResult) -> Bool {
        guard let report = result.report else { return false }
        return sets.contains { $0.version.package == report.packageName && $0.version.versionCode == report.versionCode }
    }
}

/// An OBB of a unit that no app takes by its name (ADR-0049 d): the user names the app, or lets it go with its unit.
struct OBBQuestion: Identifiable {
    let unit: URL
    /// Its name in the unit, as the user picked it.
    let file: String
    /// The apps it can go to, by name (`Library.appsForOBBs`); with none, an alert says so instead.
    let apps: [LibraryEntry]

    var id: String { unit.path + "/" + file }
}

/// UI state around the `Library` actor, which does the file work off the main thread.
@MainActor
@Observable
final class LibraryModel {
    private(set) var entries: [LibraryEntry] = []
    /// The favorites' packages in their own order (the navigation design, section 3).
    private(set) var favorites: [String] = []
    /// When each app was last opened (the navigation design, section 2).
    private(set) var lastOpened: [String: Date] = [:]
    private(set) var pending: [WaitingUnit] = []
    private(set) var progress: [URL: Double] = [:]
    /// Entries whose converted code is being signed again, shown as "Updating".
    private(set) var resigningIDs: Set<String> = []
    /// Entries whose native code is being signed before they start (ADR-0032), shown as "Updating" too.
    private(set) var signingBeforeStart: Set<String> = []
    /// Entries being checked again after a Minato update (ADR-0055 decision 5), shown as "Updating" too.
    private(set) var recheckingIDs: Set<String> = []
    /// The app whose restart into game mode did not work: the user opens Minato again, and its launch
    /// request is still there (ADR-0032, decision 2).
    var restartFailedFor: LibraryEntry?
    var errorMessage: String?
    /// A short note at the bottom of the library for about 3 s, after an update (ADR-0034, decision 5).
    private(set) var note: String?
    /// A file that would not run and leaves the installed version, with its title and message.
    var updateRejected: (title: String, message: String)?
    /// Imports that ask first, the first one shown.
    private(set) var questions: [UpdateQuestion] = []
    /// OBBs that wait for the user to name their app, the first one shown (ADR-0049 d).
    private(set) var obbQuestions: [OBBQuestion] = []
    /// The signer for native code at import (ADR-0032), which the signing coordinator provides; nil
    /// while the certificate is not valid, and then the import keeps the originals only.
    @ObservationIgnored var importSigning: @MainActor () -> ImportSigning? = { nil }
    /// What signs native code now, and its converter; the signing coordinator provides both.
    @ObservationIgnored var currentSigner: @MainActor () -> CurrentSigner = { .none }
    @ObservationIgnored var converter: @MainActor (CurrentSigner) -> (any ContainerConverting)? = { _ in nil }
    /// Returns once the signing coordinator's first look at the certificate after launch has ended.
    @ObservationIgnored var firstSigningLook: @MainActor () async -> Void = {}

    @ObservationIgnored private let library: Library?
    /// Hands the library the import's checks of native libraries once; the first scan and a signing before a start wait.
    @ObservationIgnored private var checks: Task<Void, Never>?
    @ObservationIgnored private let paths: LibraryPaths?
    @ObservationIgnored private var isRefreshing = false
    @ObservationIgnored private var needsAnotherScan = false
    /// A tap is being handled: until it ends, and after it started an app, taps do nothing.
    @ObservationIgnored private var isLaunching = false
    /// Units that wait for an answer or are being answered: a scan leaves them alone, so a question comes
    /// only once per process. The next launch asks again.
    @ObservationIgnored private var held: Set<URL> = []

    init() {
        var openedPaths: LibraryPaths?
        var openedLibrary: Library?
        do {
            let standard = try LibraryPaths.standard()
            openedPaths = standard
            openedLibrary = try Library(paths: standard)
        } catch {
            errorMessage = "The library could not be opened: \(error.localizedDescription)"
        }
        paths = openedPaths
        library = openedLibrary
        if let openedLibrary {
            checks = Task { await Self.useChecks(in: openedLibrary, guest: Bundle.main.bundleURL.appending(path: "Guest")) }
            Task { await recheckAll() }
        }
    }

    /// This build's identity, which every report records (ADR-0055 decision 5): the LC_UUID of the image that holds the
    /// app's code, its analyzer and its converter (Minato.debug.dylib in a Debug build), which every build changes. So
    /// any new Minato, a release or a test build, checks the library again once.
    nonisolated static let checkStamp: String = {
        let header = #dsohandle.assumingMemoryBound(to: mach_header_64.self)
        var command = UnsafeRawPointer(header).advanced(by: MemoryLayout<mach_header_64>.size)
        for _ in 0..<header.pointee.ncmds {
            let load = command.assumingMemoryBound(to: load_command.self).pointee
            if load.cmd == UInt32(LC_UUID) {
                return UUID(uuid: command.assumingMemoryBound(to: uuid_command.self).pointee.uuid).uuidString
            }
            command = command.advanced(by: Int(load.cmdsize))
        }
        return "no-uuid"
    }()

    /// Checks again, in the background after the launch, every entry another Minato checked (ADR-0055 decision 5),
    /// one after the other, each shown as "Updating" while it runs; the import's checks are set up first.
    private func recheckAll() async {
        guard let library else { return }
        await checks?.value
        guard !(await library.entriesToRecheck()).isEmpty else { return }
        let summary = await library.recheckAll { [weak self] id in
            await MainActor.run { self?.recheckingIDs = [id] }
        }
        recheckingIDs = []
        entries = await library.entries
        log.info("Checked again: \(summary.checked.count, privacy: .public) apps")
        for failure in summary.failures {
            log.error("Not checked again: \(failure, privacy: .public)")
        }
    }

    /// The import's checks of native libraries (ADR-0055 a2 to a4): the converter's check mode, and the guest's public
    /// libraries and export table as tools/embed-guest-system.sh embeds them. A list that is missing or no list leaves
    /// its check out, as the library does without it. Off the main actor: the export table is a megabyte.
    nonisolated private static func useChecks(in library: Library, guest: URL) async {
        let etc = guest.appending(path: "system/etc")
        func list(_ name: String) -> String? {
            do {
                return try String(contentsOf: etc.appending(path: name), encoding: .utf8)
            } catch {
                log.error("The guest's \(name, privacy: .public) cannot be read: \(error.localizedDescription, privacy: .public)")
                return nil
            }
        }
        await library.use(checkStamp: checkStamp)
        await library.use(checker: ELFChecker())
        await library.use(publicLibraries: list("public.libraries.txt"))
        do {
            try await library.use(exportTable: list("export-table.txt"))
        } catch {
            log.error("The guest's export table is no table: \(error.localizedDescription, privacy: .public)")
        }
    }

    var items: [LibraryItem] {
        let waiting = pending.map { unit in
            LibraryItem(
                id: "pending:" + unit.url.lastPathComponent,
                title: unit.title,
                status: progress[unit.url].map(LibraryItem.Status.installing) ?? .new,
                entry: nil,
                iconURL: nil)
        }
        let installed = entries
            .sorted { $0.displayName.localizedStandardCompare($1.displayName) == .orderedAscending }
            .map { entry in
                LibraryItem(
                    id: entry.id,
                    title: entry.displayName,
                    status: isUpdating(entry) ? .updating : Texts.status(of: entry),
                    entry: entry,
                    iconURL: entry.iconFile.flatMap { paths?.applicationSupport.appendingPathComponent($0) },
                    isFavorite: favorites.contains(entry.id))
            }
        return waiting + installed
    }

    /// Installs picks that are not installed yet, one unit after the other. A call while a refresh
    /// is running is not lost: the running refresh scans once more when it is done.
    func refresh() async {
        guard let library else { return }
        await checks?.value
        guard !isRefreshing else {
            needsAnotherScan = true
            return
        }
        isRefreshing = true
        defer { isRefreshing = false }

        repeat {
            needsAnotherScan = false
            entries = await library.entries
            favorites = await library.favorites
            lastOpened = await library.lastOpened
            do {
                pending = try await library.scanUnits().filter { !held.contains($0) }.map(WaitingUnit.init)
            } catch {
                errorMessage = "Waiting imports cannot be read: \(error.localizedDescription)"
                return
            }
            while let unit = pending.first {
                await install(unit, with: library)
                pending.removeFirst()
            }
        } while needsAnotherScan
    }

    /// "Open with" and drops from other apps.
    func open(_ url: URL) async {
        await open([url])
    }

    /// The + button: takes the pick over as one unit, its files and folders together (ADR-0048, decision 1), then
    /// installs what waits. The originals stay where they are.
    func open(_ urls: [URL]) async {
        guard let library, !urls.isEmpty else { return }
        let names = Texts.files(urls.map(\.lastPathComponent))
        do {
            // The library reads the pick while it takes it over: one start per URL, and a stop for each that
            // started, on every path.
            var accessed: [URL] = []
            for url in urls where url.startAccessingSecurityScopedResource() {
                accessed.append(url)
            }
            defer {
                for url in accessed { url.stopAccessingSecurityScopedResource() }
            }
            let start = ContinuousClock.now
            _ = try await library.adopt(urls)
            log.info("Adopted \(names, privacy: .public) in \(ContinuousClock.now - start, privacy: .public)")
        } catch {
            errorMessage = "\(names) could not be added: \(error.localizedDescription)"
        }
        await refresh()
    }

    // MARK: - Starting an app (ADR-0032)

    /// Why a tap did not start an app, for the view to show.
    enum LaunchBlock {
        case openSigning
        case explain(LibraryEntry, LaunchDecision.Reason)
    }

    func isUpdating(_ entry: LibraryEntry) -> Bool {
        resigningIDs.contains(entry.id) || signingBeforeStart.contains(entry.id) || recheckingIDs.contains(entry.id)
    }

    /// One tap on an installed app (decision 3): starts it, after signing its native code when the current
    /// signer did not sign it; nil once it starts or an error message says why not. A tap while another
    /// is handled does nothing: the app that tap starts would be left behind anyway.
    func launch(_ tapped: LibraryEntry) async -> LaunchBlock? {
        guard !isLaunching, !Restart.isUnderway else { return nil }
        isLaunching = true
        defer { isLaunching = false }
        var entry = entries.first { $0.id == tapped.id } ?? tapped
        var signer = currentSigner()
        var decision = LaunchDecision(entry: entry, isUpdating: isUpdating(entry), signer: signer, containerVersion: ContainerSigner.containerVersion)
        if decision == .waitForSigner {
            // A tap right after launch: the first look may still bring a signer without a tap, so the
            // setup sheet opens only if it does not. Meanwhile the app shows "Updating…".
            signingBeforeStart.insert(entry.id)
            await firstSigningLook()
            signingBeforeStart.remove(entry.id)
            entry = entries.first { $0.id == tapped.id } ?? entry
            signer = currentSigner()
            decision = LaunchDecision(entry: entry, isUpdating: isUpdating(entry), signer: signer, containerVersion: ContainerSigner.containerVersion)
        }
        #if DEBUG
        SessionCheck.event("decision \(decision) signer \(signer.fingerprint ?? "none")")
        #endif
        switch decision {
        case .start:
            await start(entry)
        case .signFirst:
            guard let library, let fingerprint = signer.fingerprint, let converter = converter(signer) else {
                errorMessage = "“\(entry.displayName)” could not be signed. Check the signing certificate."
                return nil
            }
            signingBeforeStart.insert(entry.id)
            defer { signingBeforeStart.remove(entry.id) }
            await checks?.value
            do {
                try await library.prepareNativeCode(id: entry.id)
                let files = try await library.resign(id: entry.id, fingerprint: fingerprint, with: converter)
                entries = await library.entries
                #if DEBUG
                SessionCheck.event("signed \(entry.id) \(files) \(entries.first { $0.id == entry.id }?.signedWith ?? "none")")
                #endif
            } catch is ConversionRefused {
                // Its code cannot run: the app is incompatible now, with the converter's reason.
                entries = await library.entries
                return .explain(entries.first { $0.id == entry.id } ?? entry, .incompatible)
            } catch {
                errorMessage = "“\(entry.displayName)” could not be signed: \(error.localizedDescription)"
                entries = await library.entries
                return nil
            }
            await start(entry)
        case .openSigning, .waitForSigner:
            return .openSigning
        case .explain(let reason):
            return .explain(entry, reason)
        }
        return nil
    }

    /// Writes the launch request, then restarts Minato into game mode for `entry` (decisions 1 and 2). The app counts
    /// as opened for Home's Recently Opened now, since the process ends right after.
    private func start(_ entry: LibraryEntry) async {
        guard let paths else { return }
        do {
            try LaunchRequestStore(folder: paths.applicationSupport).write(LaunchRequest(package: entry.id, time: .now))
        } catch {
            errorMessage = "“\(entry.displayName)” could not be started: \(error.localizedDescription)"
            return
        }
        do {
            try await library?.recordOpened(entry.id)
        } catch {
            let reason = error.localizedDescription
            log.error("\(entry.id, privacy: .public): its opening was not recorded: \(reason, privacy: .public)")
        }
        log.info("Starting \(entry.id, privacy: .public)")
        Restart.now { [weak self] in self?.restartFailedFor = entry }
    }

    // MARK: - Signing again (ADR-0023)

    func resignCandidates() async -> [ResignCandidate] {
        await library?.resignCandidates() ?? []
    }

    /// Signs again everything the certificate `fingerprint` did not sign, one entry after the
    /// other, each shown as "Updating" while it runs.
    func resignAll(fingerprint: String, with converter: some ContainerConverting) async -> ResignSummary {
        guard let library else { return ResignSummary(entries: 0, files: 0, failures: []) }
        let summary = await library.resignAll(fingerprint: fingerprint, with: converter) { [weak self] id in
            await MainActor.run { self?.resigningIDs = [id] }
        }
        resigningIDs = []
        entries = await library.entries
        return summary
    }

    /// Makes all converted code count as signed by an old certificate ("Simulate Certificate Change").
    func forgetSignatures() async {
        guard let library else { return }
        do {
            try await library.forgetSignatures()
        } catch {
            errorMessage = "The library could not be updated: \(error.localizedDescription)"
        }
        entries = await library.entries
    }

    /// The app's profile (ADR-0065, ADR-0066): the default where none is stored.
    func profile(of entry: LibraryEntry) -> AppProfile {
        paths.map { AppProfile.load(for: entry.id, in: $0) } ?? AppProfile()
    }

    /// Keeps the app's profile for its next start.
    func setProfile(_ profile: AppProfile, of entry: LibraryEntry) {
        guard let paths else { return }
        do {
            try profile.save(for: entry.id, in: paths)
        } catch {
            log.error("\(entry.id, privacy: .public): its profile: \(error.localizedDescription, privacy: .public)")
            errorMessage = "The profile could not be saved: \(error.localizedDescription)"
        }
    }

    func remove(_ entries: [LibraryEntry]) async {
        guard let library else { return }
        for entry in entries {
            do {
                try await library.remove(id: entry.id)
            } catch {
                errorMessage = "“\(entry.displayName)” could not be deleted: \(error.localizedDescription)"
                continue
            }
            // Its WebViews' data goes with it, as on Android (ADR-0056 decision 7).
            if !entry.id.hasPrefix("file:") { await removeWebData(of: entry) }
        }
        self.entries = await library.entries
        favorites = await library.favorites
        lastOpened = await library.lastOpened
    }

    /// The app's report for Report a Problem (ADR-0070): Get Info's status line and issues, how its last session
    /// ended where the caller knows it, and the last session's kept log.
    func report(of entry: LibraryEntry, ending: AppReport.Ending? = nil) -> AppReport {
        let log = paths.flatMap { paths in
            try? String(contentsOf: SessionLog(folder: paths.sessionLogDirectory).keptFile(for: entry.id),
                        encoding: .utf8)
        }
        return AppReport(entry: entry, device: .current, status: Texts.statusLine(Texts.status(of: entry)),
                         issues: (entry.report?.compatibility.issues ?? []).map(Texts.issue), ending: ending,
                         log: log)
    }

    /// Every installed app's space for Settings › Storage, largest first (Library.storage).
    func storage() async -> [StorageUse] {
        await library?.storage() ?? []
    }

    /// Clears the app's data as Android's Clear storage does (Library.clearData), with its web data and the report of
    /// its last session; the app stays installed (the navigation design, section 6).
    func clearData(of entry: LibraryEntry) async {
        guard let library else { return }
        do {
            try await library.clearData(id: entry.id)
        } catch {
            errorMessage = "The data of “\(entry.displayName)” could not be cleared: \(error.localizedDescription)"
            return
        }
        guard !entry.id.hasPrefix("file:") else { return }
        await removeWebData(of: entry)
        if let paths { LastSessionStore(folder: paths.applicationSupport).forget(package: entry.id) }
    }

    /// Makes the apps favorites, or no favorites any more (the navigation design, section 3).
    func setFavorite(_ entries: [LibraryEntry], _ favorite: Bool) async {
        guard let library else { return }
        do {
            try await library.setFavorite(entries.map(\.id), favorite)
        } catch {
            errorMessage = "The favorites could not be saved: \(error.localizedDescription)"
        }
        favorites = await library.favorites
    }

    /// The app's WKWebsiteDataStore, if it made one. WebKit's class calls go out from the main thread with completion
    /// handlers: their async forms run on a worker, and the removal crashed on WebKit's IO queue (RunLoop::dispatch)
    /// in a process that had made no web view yet. The default store sets up WebKit in the process first; an app that
    /// never showed a WebView has no store, and none is asked to go.
    private func removeWebData(of entry: LibraryEntry) async {
        let id = WebDataStore.identifier(forPackage: entry.id)
        _ = WKWebsiteDataStore.default()
        let stores = await withCheckedContinuation { done in
            WKWebsiteDataStore.fetchAllDataStoreIdentifiers { done.resume(returning: $0) }
        }
        guard stores.contains(id) else { return }
        let error = await withCheckedContinuation { done in
            WKWebsiteDataStore.remove(forIdentifier: id) { done.resume(returning: $0) }
        }
        if let error {
            let reason = error.localizedDescription
            log.info("\(entry.id, privacy: .public): no web data removed: \(reason, privacy: .public)")
        }
    }

    // MARK: - Updates (ADR-0034)

    /// Tells the user what a unit's import did to installed apps: one note for those it updated or found installed,
    /// one alert for its rejections, and one question for every set that asks (ADR-0048, F7).
    private func show(_ results: [ImportResult], of unit: URL, installed: [LibraryEntry]) {
        var notes: [String] = []
        var asking: [UpdateQuestion.Asking] = []
        for result in results {
            #if DEBUG
            UpdateCheck.event("import \(result.decision)")
            #endif
            switch result.decision {
            case .install, .reject:
                break
            case .update, .reinstall:
                if let text = Texts.updateNote(result) { notes.append(text) }
            case .unchanged:
                // An OBB that came in says so on its own (ADR-0049 d).
                if result.installedOBBs.isEmpty, result.replacedOBBs.isEmpty, let text = Texts.updateNote(result) {
                    notes.append(text)
                }
            case .ask:
                if let set = UpdateQuestion.Asking(result, installed: installed) { asking.append(set) }
            }
            if let text = Texts.obbNote(result) { notes.append(text) }
        }
        #if DEBUG
        UpdateCheck.tiles(of: results, in: installed)
        #endif
        reject(results)
        refuseOBBs(results)
        if !notes.isEmpty { showNote(notes.joined(separator: " ")) }
        ask(asking, about: unit)
    }

    /// Picked OBBs that could not go in, in one alert (ADR-0049 d).
    private func refuseOBBs(_ results: [ImportResult]) {
        let texts = results.flatMap { result in
            result.refusedOBBs.map { Texts.obbRefused($0.path, app: result.entry.displayName, because: $0.reason) }
        }
        if !texts.isEmpty { errorMessage = texts.joined(separator: "\n\n") }
    }

    /// Holds the unit, and queues the question about its first OBB no app takes by its name (ADR-0049 d); none while
    /// a set of the unit waits for its own answer.
    private func askAboutOBBs(in unit: URL, with library: Library) async {
        guard let file = await library.obbsWaitingForAnApp(inUnit: unit).first else { return }
        held.insert(unit)
        obbQuestions.append(OBBQuestion(unit: unit, file: file, apps: await library.appsForOBBs()))
        #if DEBUG
        if obbQuestions.count == 1 { UpdateCheck.obbQuestionShown(obbQuestions[0], in: self) }
        #endif
    }

    /// The answer to an OBB's question: the app it goes to, under a name made for that app, or none, which lets that
    /// file go as "Don’t Import" does, and OK where no app can take it (ADR-0049 d, App's review). Then the unit's next
    /// OBB asks.
    func assign(_ question: OBBQuestion, to app: LibraryEntry?) async {
        obbQuestions.removeAll { $0.id == question.id }
        #if DEBUG
        UpdateCheck.event("answered \(app.map { "app \($0.id)" } ?? "keep")")
        if let next = obbQuestions.first { UpdateCheck.obbQuestionShown(next, in: self) }
        #endif
        guard let library else { return }
        var results: [ImportResult] = []
        if let app {
            do {
                results = try await library.importUnit(question.unit, assigning: [question.file: app.id], signing: importSigning())
            } catch {
                errorMessage = "“\(question.file)” could not be added to “\(app.displayName)”: \(error.localizedDescription)"
            }
        } else {
            await library.discardPickedOBB(question.file, inUnit: question.unit)
        }
        entries = await library.entries
        held.remove(question.unit)
        let notes = results.compactMap { Texts.obbNote($0, file: question.file) }
        refuseOBBs(results)
        if !notes.isEmpty { showNote(notes.joined(separator: " ")) }
        await askAboutOBBs(in: question.unit, with: library)
    }

    /// The files of a unit the installed apps keep their versions against, in one alert.
    private func reject(_ results: [ImportResult]) {
        guard let texts = Texts.updateRejected(results) else { return }
        updateRejected = texts
        #if DEBUG
        // Recorded here too, as the alert on top may be a question: SwiftUI shows the two in either order.
        UpdateCheck.event("rejection \(texts.title) | \(texts.message)")
        UpdateCheck.recordAlert()
        #endif
    }

    /// Holds the unit, and queues one question for its sets that ask.
    private func ask(_ sets: [UpdateQuestion.Asking], about unit: URL) {
        guard !sets.isEmpty else { return }
        held.insert(unit)
        questions.append(UpdateQuestion(unit: unit, sets: sets))
        #if DEBUG
        if questions.count == 1 { UpdateCheck.questionShown(questions[0], in: self) }
        #endif
    }

    /// "Replace" (or "Install") installs the versions the question listed, as it listed them, over the installed apps,
    /// which keep their data; one that asks otherwise now asks again. "Keep Installed" (or "Don’t Install") discards
    /// the unit's APKs: its other sets were installed before the question.
    func answer(_ question: UpdateQuestion, replace: Bool) async {
        questions.removeAll { $0.id == question.id }
        #if DEBUG
        UpdateCheck.event("answered \(replace ? "replace" : "keep")")
        if let next = questions.first { UpdateCheck.questionShown(next, in: self) }
        #endif
        guard let library else { return }
        var results: [ImportResult] = []
        if replace {
            do {
                results = try await library.importUnit(question.unit, replacing: Set(question.sets.map(\.version)),
                                                       signing: importSigning())
            } catch {
                let installs = question.sets.contains { $0.installed == nil }
                let done = question.replacesAny ? (installs ? "replaced and installed" : "replaced") : "installed"
                errorMessage = "\(Texts.subject(of: question)) could not be \(done): \(error.localizedDescription)"
            }
        } else {
            await library.keepInstalled(discarding: question.unit)
        }
        let installed = await library.entries
        entries = installed
        // Nothing suspends from here on: no scan takes the unit between its release and a question that holds it again.
        held.remove(question.unit)
        var notes: [String] = []
        var asking: [UpdateQuestion.Asking] = []
        for result in results {
            switch result.decision {
            case .update where question.lists(result):
                notes.append(Texts.replacedNote(result.entry))
            case .ask:
                if let set = UpdateQuestion.Asking(result, installed: installed) { asking.append(set) }
            default:
                // A fresh install shows its tile, and the unit's other sets were noted or alerted before the question.
                break
            }
        }
        #if DEBUG
        UpdateCheck.tiles(of: results, in: installed)
        #endif
        // The answer imports the whole unit again: only a rejection of a version the question listed alerts here.
        reject(results.filter(question.lists))
        if !notes.isEmpty { showNote(notes.joined(separator: " ")) }
        ask(asking, about: question.unit)
        if asking.isEmpty { await askAboutOBBs(in: question.unit, with: library) }
    }

    private func showNote(_ text: String) {
        note = text
        #if DEBUG
        UpdateCheck.event("note \(text)")
        #endif
        Task { [weak self] in
            try? await Task.sleep(for: .seconds(3))
            guard let self, self.note == text else { return }
            self.note = nil
            #if DEBUG
            UpdateCheck.event("note hidden")
            #endif
        }
    }

    private func install(_ unit: WaitingUnit, with library: Library) async {
        let url = unit.url
        progress[url] = 0
        do {
            let start = ContinuousClock.now
            let results = try await library.importUnit(url, signing: importSigning()) { [weak self] value in
                Task { @MainActor in
                    guard let self, self.pending.contains(where: { $0.url == url }) else { return }
                    self.progress[url] = value
                }
            }
            let megabytes = Double(unit.size) / 1_000_000
            let decisions = results.map { String(describing: $0.decision) }.joined(separator: ", ")
            log.info("Imported \(unit.title, privacy: .public) (\(megabytes, format: .fixed(precision: 1), privacy: .public) MB, \(decisions, privacy: .public)) in \(ContinuousClock.now - start, privacy: .public)")
            let installed = await library.entries
            show(results, of: url, installed: installed)
            await askAboutOBBs(in: url, with: library)
        } catch {
            errorMessage = "\(Texts.files(unit.apks)) could not be imported: \(error.localizedDescription)"
        }
        progress[url] = nil
        entries = await library.entries
    }
}

/// The converter's check mode is the import's checker (ADR-0055 a2).
extension ELFChecker: ELFRefusing {}
