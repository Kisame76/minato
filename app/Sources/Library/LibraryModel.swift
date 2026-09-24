import APKKit
import APKLibrary
import APKSigning
import Foundation
import Observation
import os

private let log = Logger(subsystem: "io.github.kisame76.apkrunner", category: "import")

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

/// UI state around the `Library` actor, which does the file work off the main thread.
@MainActor
@Observable
final class LibraryModel {
    private(set) var entries: [LibraryEntry] = []
    private(set) var pending: [URL] = []
    private(set) var progress: [URL: Double] = [:]
    /// Entries whose converted code is being signed again, shown as "Updating".
    private(set) var resigningIDs: Set<String> = []
    var errorMessage: String?

    @ObservationIgnored private let library: Library?
    @ObservationIgnored private let paths: LibraryPaths?
    @ObservationIgnored private var isRefreshing = false
    @ObservationIgnored private var needsAnotherScan = false

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
                    status: resigningIDs.contains(entry.id) ? .updating : Texts.status(of: entry),
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
                pending = try await library.scan()
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

    private func install(_ url: URL, with library: Library) async {
        progress[url] = 0
        do {
            let start = ContinuousClock.now
            let entry = try await library.importFile(url) { [weak self] value in
                Task { @MainActor in
                    guard let self, self.pending.contains(url) else { return }
                    self.progress[url] = value
                }
            }
            let megabytes = Double(entry.source.size) / 1_000_000
            log.info("Imported \(url.lastPathComponent, privacy: .public) (\(megabytes, format: .fixed(precision: 1), privacy: .public) MB) in \(ContinuousClock.now - start, privacy: .public)")
        } catch {
            errorMessage = "“\(url.lastPathComponent)” could not be imported: \(error.localizedDescription)"
        }
        progress[url] = nil
        entries = await library.entries
    }
}
