import APKKit
import APKLibrary
import APKSession
import APKSigning
import Foundation
import Observation
import os

private let log = Logger(subsystem: "io.github.kisame76.minato", category: "import")

/// One tile in the library grid: an installed entry or a dropped file that is not imported yet.
struct LibraryItem: Identifiable {
    enum Status {
        case new
        case installing(Double)
        /// Its converted code is being signed with a new certificate (ADR-0023).
        case updating
        case ready
        case needsSplits
        case incompatible(String)
        case invalid(String)
    }

    let id: String
    let title: String
    let status: Status
    let entry: LibraryEntry?
    let iconURL: URL?
}

/// An import that waits for the user's answer (ADR-0034, decision 1): until then its file stays in incoming/.
struct UpdateQuestion: Identifiable {
    let url: URL
    /// The installed app, which stays until the answer.
    let installed: LibraryEntry
    /// The new file's report.
    let report: APKReport
    let warnings: Set<UpdateWarning>

    var id: URL { url }
}

/// UI state around the `Library` actor, which does the file work off the main thread.
@MainActor
@Observable
final class LibraryModel {
    private(set) var entries: [LibraryEntry] = []
    private(set) var pending: [URL] = []
    private(set) var progress: [URL: Double] = [:]
    /// Entries whose converted code is being signed again, shown as "Updating".
    private(set) var resigningIDs: Set<String> = []
    /// Entries whose native code is being signed before they start (ADR-0032), shown as "Updating" too.
    private(set) var signingBeforeStart: Set<String> = []
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
    /// The signer for native code at import (ADR-0032), which the signing coordinator provides; nil
    /// while the certificate is not valid, and then the import keeps the originals only.
    @ObservationIgnored var importSigning: @MainActor () -> ImportSigning? = { nil }
    /// What signs native code now, and its converter; the signing coordinator provides both.
    @ObservationIgnored var currentSigner: @MainActor () -> CurrentSigner = { .none }
    @ObservationIgnored var converter: @MainActor (CurrentSigner) -> (any ContainerConverting)? = { _ in nil }

    @ObservationIgnored private let library: Library?
    @ObservationIgnored private let paths: LibraryPaths?
    @ObservationIgnored private var isRefreshing = false
    @ObservationIgnored private var needsAnotherScan = false
    /// A tap is being handled: until it ends, and after it started an app, taps do nothing.
    @ObservationIgnored private var isLaunching = false
    /// Files that wait for an answer or are being answered: a scan leaves them alone, so a question comes
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
    }

    var items: [LibraryItem] {
        let waiting = pending.map { url in
            LibraryItem(
                id: "pending:" + url.lastPathComponent,
                title: url.deletingPathExtension().lastPathComponent,
                status: progress[url].map(LibraryItem.Status.installing) ?? .new,
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
                    iconURL: entry.iconFile.flatMap { paths?.applicationSupport.appendingPathComponent($0) })
            }
        return waiting + installed
    }

    /// Installs picked files that are not installed yet, one after the other. A call while a refresh
    /// is running is not lost: the running refresh scans once more when it is done.
    func refresh() async {
        guard let library else { return }
        guard !isRefreshing else {
            needsAnotherScan = true
            return
        }
        isRefreshing = true
        defer { isRefreshing = false }

        repeat {
            needsAnotherScan = false
            entries = await library.entries
            do {
                pending = try await library.scan().filter { !held.contains($0) }
            } catch {
                errorMessage = "Waiting imports cannot be read: \(error.localizedDescription)"
                return
            }
            while let url = pending.first {
                await install(url, with: library)
                pending.removeFirst()
            }
        } while needsAnotherScan
    }

    /// "Open with" and drops from other apps.
    func open(_ url: URL) async {
        await open([url])
    }

    /// The + button: takes over every picked file, then installs them in one pass. The originals stay where they are.
    func open(_ urls: [URL]) async {
        guard let library else { return }
        for url in urls {
            let hasAccess = url.startAccessingSecurityScopedResource()
            defer {
                if hasAccess { url.stopAccessingSecurityScopedResource() }
            }
            do {
                let start = ContinuousClock.now
                _ = try await library.adopt(url)
                log.info("Adopted \(url.lastPathComponent, privacy: .public) in \(ContinuousClock.now - start, privacy: .public)")
            } catch {
                errorMessage = "“\(url.lastPathComponent)” could not be added: \(error.localizedDescription)"
            }
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
        resigningIDs.contains(entry.id) || signingBeforeStart.contains(entry.id)
    }

    /// One tap on an installed app (decision 3): starts it, after signing its native code when the current
    /// signer did not sign it; nil once it starts or an error message says why not. A tap while another
    /// is handled does nothing: the app that tap starts would be left behind anyway.
    func launch(_ tapped: LibraryEntry) async -> LaunchBlock? {
        guard !isLaunching, !Restart.isUnderway else { return nil }
        isLaunching = true
        defer { isLaunching = false }
        let entry = entries.first { $0.id == tapped.id } ?? tapped
        let signer = currentSigner()
        let decision = LaunchDecision(entry: entry, isUpdating: isUpdating(entry), signer: signer)
        #if DEBUG
        SessionCheck.event("decision \(decision) signer \(signer.fingerprint ?? "none")")
        #endif
        switch decision {
        case .start:
            start(entry)
        case .signFirst:
            guard let library, let fingerprint = signer.fingerprint, let converter = converter(signer) else {
                errorMessage = "“\(entry.displayName)” could not be signed. Check the signing certificate."
                return nil
            }
            signingBeforeStart.insert(entry.id)
            defer { signingBeforeStart.remove(entry.id) }
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
            start(entry)
        case .openSigning:
            return .openSigning
        case .explain(let reason):
            return .explain(entry, reason)
        }
        return nil
    }

    /// Writes the launch request, then restarts Minato into game mode for `entry` (decisions 1 and 2).
    private func start(_ entry: LibraryEntry) {
        guard let paths else { return }
        do {
            try LaunchRequestStore(folder: paths.applicationSupport).write(LaunchRequest(package: entry.id, time: .now))
        } catch {
            errorMessage = "“\(entry.displayName)” could not be started: \(error.localizedDescription)"
            return
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

    func remove(_ entries: [LibraryEntry]) async {
        guard let library else { return }
        for entry in entries {
            do {
                try await library.remove(id: entry.id)
            } catch {
                errorMessage = "“\(entry.displayName)” could not be deleted: \(error.localizedDescription)"
            }
        }
        self.entries = await library.entries
    }

    // MARK: - Updates (ADR-0034)

    /// Tells the user what an import did to an installed app: a note, the rejection or the question.
    private func show(_ result: ImportResult, of url: URL) {
        #if DEBUG
        UpdateCheck.event("import \(result.decision)")
        #endif
        switch result.decision {
        case .install:
            break
        case .update, .unchanged, .reinstall:
            if let text = Texts.updateNote(result) { showNote(text) }
        case .reject(let state):
            updateRejected = Texts.updateRejected(installed: result.entry, report: result.report, state: state)
            #if DEBUG
            UpdateCheck.recordAlert()
            #endif
        case .ask(let warnings):
            guard let report = result.report else { return }
            held.insert(url)
            questions.append(UpdateQuestion(url: url, installed: result.entry, report: report, warnings: warnings))
            #if DEBUG
            if questions.count == 1 { UpdateCheck.questionShown(questions[0], in: self) }
            #endif
        }
    }

    /// "Replace" installs the waiting file over the installed app, which keeps its data; "Keep Installed"
    /// discards the file.
    func answer(_ question: UpdateQuestion, replace: Bool) async {
        questions.removeAll { $0.id == question.id }
        defer { held.remove(question.url) }
        #if DEBUG
        UpdateCheck.event("answered \(replace ? "replace" : "keep")")
        if let next = questions.first { UpdateCheck.questionShown(next, in: self) }
        #endif
        guard let library else { return }
        if replace {
            do {
                let result = try await library.importFile(question.url, replacing: true, signing: importSigning())
                showNote(Texts.replacedNote(result.entry))
            } catch {
                errorMessage = "“\(question.installed.displayName)” could not be replaced: \(error.localizedDescription)"
            }
        } else {
            await library.keepInstalled(discarding: question.url)
        }
        entries = await library.entries
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

    private func install(_ url: URL, with library: Library) async {
        progress[url] = 0
        do {
            let start = ContinuousClock.now
            // Read first: the import discards the picked copy.
            let size = (try? url.resourceValues(forKeys: [.fileSizeKey]))?.fileSize ?? 0
            let result = try await library.importFile(url, signing: importSigning()) { [weak self] value in
                Task { @MainActor in
                    guard let self, self.pending.contains(url) else { return }
                    self.progress[url] = value
                }
            }
            let megabytes = Double(size) / 1_000_000
            log.info("Imported \(url.lastPathComponent, privacy: .public) (\(megabytes, format: .fixed(precision: 1), privacy: .public) MB, \(String(describing: result.decision), privacy: .public)) in \(ContinuousClock.now - start, privacy: .public)")
            show(result, of: url)
        } catch {
            errorMessage = "“\(url.lastPathComponent)” could not be imported: \(error.localizedDescription)"
        }
        progress[url] = nil
        entries = await library.entries
    }
}
