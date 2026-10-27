import APKLibrary
import APKRuntime
import APKSession
import Foundation
import Synchronization
import UIKit
import os

private let log = Logger(subsystem: "io.github.kisame76.minato", category: "session")

/// ADR-0057's addendum on the iPhone: the app's edits of a clone in Download reach the user's file (WriteBack). One per
/// process, as one app runs in it: the records in Minato's own folder for the app, a watch of each clone's last close
/// (apkrt_watch_writes), the work on a serial queue of its own every 250 ms, the app's versions kept before the guest
/// starts, a flush when the session goes to the background and at its end, and the user's decision when the original
/// changed meanwhile.
final class WriteBackHost: Sendable {
    static let shared = WriteBackHost()

    private struct Session {
        let writeBack: WriteBack
        let state: URL
        /// Left once the app's versions are kept (WriteBack.secure), which the guest's start waits for.
        let secured: DispatchGroup
    }

    private let serial = DispatchQueue(label: "io.github.kisame76.minato.write-back", qos: .utility)
    private let session = Mutex<Session?>(nil)
    /// The watch of each clone, by name.
    private let watches = Mutex([String: Int32]())
    /// The conflicts asked about in this process, so that each is asked once at a time.
    private let asked = Mutex(Set<String>())
    /// Only touched on `serial`.
    nonisolated(unsafe) private var timer: DispatchSourceTimer?

    private init() {}

    /// The session of `package` begins, its mounts made and the guest not yet started. On the queue, the records stored
    /// for it are loaded, each clone watched again, and the app's versions not yet written back kept in Minato's folder
    /// (WriteBack.secure: the guest's cleaning deletes the clones it keeps no grant for; local work, clones of the
    /// files), before the first tick, which may wait on an original out of reach. The guest's start waits for the
    /// keeping (`waitUntilSecured`), not for the ticks.
    func begin(package: String, download: URL) {
        guard Library.isSafePackageName(package), let paths = try? LibraryPaths.standard() else { return }
        let state = paths.writeBackDirectory(for: package)
        let writeBack = WriteBack(download: download, state: state, writers: { [self] in writers($0) }) { [self] in
            unwatch($0)
        }
        let secured = DispatchGroup()
        secured.enter()
        session.withLock { $0 = Session(writeBack: writeBack, state: state, secured: secured) }
        serial.async { [self] in
            _ = writeBack.load { BookmarkOriginal(token: $0) }
            for clone in writeBack.clones { watch(clone) }
            writeBack.secure(at: Self.now)
            secured.leave()
            timer?.cancel()
            let timer = DispatchSource.makeTimerSource(queue: serial)
            timer.schedule(deadline: .now() + .milliseconds(250), repeating: .milliseconds(250))
            timer.setEventHandler { [self] in tick() }
            timer.resume()
            self.timer = timer
        }
    }

    private var writeBack: WriteBack? { session.withLock { $0?.writeBack } }

    /// Waits until the app's versions are kept (`begin`); called off the main thread, before the guest starts.
    func waitUntilSecured() {
        session.withLock { $0?.secured }?.wait()
    }

    /// Whether a document picked again may get `clone` again (WriteBack.reusable), `originalNow` the picked file's
    /// version, read under the pick's coordination.
    func reusable(_ clone: String, originalNow: () -> FileVersion?) -> Bool {
        writeBack?.reusable(clone, originalNow: originalNow) ?? false
    }

    /// Whether a new clone must not take `clone` (WriteBack.busy).
    func busy(_ clone: String) -> Bool { writeBack?.busy(clone) ?? false }

    /// A clone the picker made from the user's file at `original`, `version` the clone's own as made (its record starts
    /// from what was cloned, not from the original read again): its record, with the bookmark that reaches the file
    /// again, made while the file's scope lasts, and its watch, a new one, as a watch follows the file it began on.
    /// Called once the coordinated read is over, so that no lock of the write-back is ever taken inside it.
    func track(clone: String, original: URL, version: FileVersion) {
        guard let (writeBack, state) = session.withLock({ $0.map { ($0.writeBack, $0.state) } }),
              let bookmark = try? original.bookmarkData()
        else { return log.error("pick: no write-back record for a clone") }
        Self.makeStateFolder(state)
        writeBack.track(clone: clone, original: BookmarkOriginal(bookmark: bookmark), originalNow: version,
                        cloneNow: version)
        watch(clone, anew: true)
    }

    /// The session went to the background: once the app has had a moment to save in onPause and onStop (its closes
    /// quiet for 0.5 s, 3 s at most), everything dirty is written, in a background task, as iOS may suspend the
    /// process and end it without notice; the task ends when iOS says its time is up.
    func setBackground(_ on: Bool) {
        guard on, let writeBack else { return }
        DispatchQueue.main.async { [self] in
            let task = BackgroundTask()
            serial.async {
                let start = Self.now
                while Self.now - start < 3000, writeBack.closedSince(Self.now - 500) || Self.now - start < 500 {
                    usleep(100_000)
                }
                writeBack.scan(at: Self.now)
                writeBack.flush(at: Self.now, force: true)
                DispatchQueue.main.async { task.end() }
            }
        }
    }

    /// The session ends: what a mapping changed is found and everything is written, before the process goes; after
    /// 5 s the process goes all the same, and the next start of the app finishes from the stored records.
    func endSession() async {
        guard let writeBack else { return }
        await withCheckedContinuation { (done: CheckedContinuation<Void, Never>) in
            let once = Once(done)
            serial.async {
                writeBack.endSession(at: Self.now)
                once.resume()
            }
            DispatchQueue.global().asyncAfter(deadline: .now() + 5) { once.resume() }
        }
    }

    /// A continuation resumed once, by the first of two.
    private final class Once: Sendable {
        private let continuation: Mutex<CheckedContinuation<Void, Never>?>

        init(_ continuation: CheckedContinuation<Void, Never>) { self.continuation = Mutex(continuation) }

        func resume() {
            continuation.withLock { continuation in
                defer { continuation = nil }
                return continuation
            }?.resume()
        }
    }

    /// The wall clock in milliseconds, which the stored records' times keep across launches.
    private static var now: Int64 { Int64(Date().timeIntervalSince1970 * 1000) }

    private func tick() {
        guard let writeBack else { return }
        writeBack.scan(at: Self.now)
        writeBack.flush(at: Self.now, force: false)
        ask(writeBack, package: nil)
    }

    /// Asks the user about each conflict of `writeBack` not asked yet.
    private func ask(_ writeBack: WriteBack, package: String?) {
        for clone in writeBack.conflicts {
            let key = (package ?? "") + "/" + clone
            guard asked.withLock({ $0.insert(key).inserted }) else { continue }
            Self.ask(clone, writeBack: writeBack, on: serial) { [self] in asked.withLock { _ = $0.remove(key) } }
        }
    }

    /// One conflict's question; `done` once the user's choice is made.
    private static func ask(_ clone: String, writeBack: WriteBack, on queue: DispatchQueue,
                            done: @escaping @Sendable () -> Void) {
        let appVersion = writeBack.appVersion(clone), replaceable = writeBack.replaceable(clone)
        DispatchQueue.main.async {
            ConflictQuestion.shared.ask(clone, appVersion: appVersion, replaceable: replaceable) { choice in
                queue.async {
                    let resolved = writeBack.resolve(clone, replace: choice == .replace)
                    log.info("write-back: \(String(describing: choice), privacy: .public) \(resolved)")
                    guard resolved || choice != .replace else {
                        DispatchQueue.main.async { ConflictQuestion.shared.couldNotReplace(clone) }
                        return ask(clone, writeBack: writeBack, on: queue, done: done)
                    }
                    done()
                }
            }
        }
    }

    /// The conflicts a session left that ended before the user was asked (at its end, or killed): asked in the
    /// library, app by app, on a queue of their own.
    static func askLeftConflicts() {
        let queue = DispatchQueue(label: "io.github.kisame76.minato.write-back.left", qos: .utility)
        queue.async {
            guard let paths = try? LibraryPaths.standard() else { return }
            let folder = paths.applicationSupport.appending(path: "write-back")
            for package in (try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? []
            where Library.isSafePackageName(package) {
                let writeBack = WriteBack(download: paths.dataDirectory(for: package).appending(path: "download"),
                                          state: paths.writeBackDirectory(for: package))
                guard writeBack.load({ BookmarkOriginal(token: $0) }) else { continue }
                for clone in writeBack.conflicts { ask(clone, writeBack: writeBack, on: queue) {} }
            }
        }
    }

    /// Minato's folder for the app's records, made at its first record and left out of backups: it holds copies of
    /// the user's files.
    private static func makeStateFolder(_ state: URL) {
        guard (try? FileManager.default.createDirectory(at: state, withIntermediateDirectories: true)) != nil else {
            return
        }
        var excluded = URLResourceValues()
        excluded.isExcludedFromBackup = true
        var folder = state
        try? folder.setResourceValues(excluded)
    }

    /// Asks the runtime to report the app's last close of `clone`, once per clone and process. A watch is on the file
    /// it began on, not the name: `anew` (a new clone under a name) ends the one before.
    private func watch(_ clone: String, anew: Bool = false) {
        if anew, let old = watches.withLock({ $0.removeValue(forKey: clone) }) { apkrt_unwatch_writes(old) }
        guard watches.withLock({ $0[clone] == nil }) else { return }
        let context = Unmanaged.passUnretained(self).toOpaque()
        let id = apkrt_watch_writes("/storage/emulated/0/Download/\(clone)", { context, path in
            // On the guest's thread inside its close: only noted (WriteBack.closed).
            guard let context, let path else { return }
            let name = (String(cString: path) as NSString).lastPathComponent
            let host = Unmanaged<WriteBackHost>.fromOpaque(context).takeUnretainedValue()
            host.writeBack?.closed(name, at: WriteBackHost.now)
        }, context)
        // A clone the app removed is not watched; its record goes the write-back's way at the next scan or flush.
        guard id > 0 else { return log.info("write-back: no watch (\(id))") }
        watches.withLock { $0[clone] = id }
    }

    /// The guest's open writers of `clone` (apkrt_watch_writers), nil without a watch; the watches' lock is not held
    /// while the runtime is asked.
    private func writers(_ clone: String) -> Int32? {
        guard let id = watches.withLock({ $0[clone] }) else { return nil }
        let count = apkrt_watch_writers(id)
        return count < 0 ? nil : count
    }

    /// The record of `clone` went: its watch ends (called with no lock of the write-back held), unless a record was
    /// made anew under the name meanwhile (a pick after the old one went), which keeps it. Decided under the watches'
    /// lock, which a new record's `watch` waits for.
    private func unwatch(_ clone: String) {
        let writeBack = writeBack
        guard let id = watches.withLock({ watches in
            writeBack?.tracked(clone) == true ? nil : watches.removeValue(forKey: clone)
        }) else { return }
        apkrt_unwatch_writes(id)
    }
}

/// A background task for the write-back, ended once, by its work or when iOS's time is up.
@MainActor
private final class BackgroundTask: Sendable {
    private var id = UIBackgroundTaskIdentifier.invalid

    init() {
        id = UIApplication.shared.beginBackgroundTask(withName: "write-back") { [weak self] in
            MainActor.assumeIsolated { self?.end() }
        }
    }

    func end() {
        guard id != .invalid else { return }
        UIApplication.shared.endBackgroundTask(id)
        id = .invalid
    }
}

/// The user's file a clone was made from, reached by its security-scoped bookmark: read and replaced under one
/// coordinated read and write (the read for an evicted iCloud file, which the write alone does not download), the
/// content compared again inside it and replaced through a file beside it and replaceItemAt, so that a crash never
/// leaves half a file. A bookmark that no longer resolves, or leads into the trash (the user deleted the file), is a
/// conflict; a stale one is made anew while the file's scope lasts.
final class BookmarkOriginal: Original {
    private let bookmark: Mutex<Data>

    init(bookmark: Data) { self.bookmark = Mutex(bookmark) }

    init?(token: String) {
        guard let bookmark = Data(base64Encoded: token) else { return nil }
        self.bookmark = Mutex(bookmark)
    }

    var token: String { bookmark.withLock { $0.base64EncodedString() } }

    func version() -> FileVersion? {
        withFile { url in
            var version: FileVersion?
            var failure: NSError?
            NSFileCoordinator().coordinate(readingItemAt: url, options: [], error: &failure) { url in
                version = FileVersion.read(path: url.path)
            }
            return version
        } ?? nil
    }

    func replace(ifStill expected: FileVersion, from source: Int32, content: FileVersion) -> ReplaceResult {
        withFile { url in
            var result = ReplaceResult.failed
            var failure: NSError?
            NSFileCoordinator().coordinate(readingItemAt: url, options: [], writingItemAt: url,
                                           options: .forReplacing, error: &failure) { _, url in
                guard let now = FileVersion.read(path: url.path) else {
                    result = .changed
                    return
                }
                if now.sameContent(content) {
                    result = .written(now)
                    return
                }
                guard now.sameContent(expected) else {
                    result = .changed
                    return
                }
                guard let folder = try? FileManager.default.url(for: .itemReplacementDirectory, in: .userDomainMask,
                                                                appropriateFor: url, create: true)
                else { return }
                defer { try? FileManager.default.removeItem(at: folder) }
                let into = Darwin.open(folder.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
                guard into >= 0 else { return }
                let copied = FileVersion.copyWhole(from: source, to: url.lastPathComponent, in: into, mode: 0o644,
                                                   content: content)
                close(into)
                let replacement = folder.appending(path: url.lastPathComponent)
                guard copied, (try? FileManager.default.replaceItemAt(url, withItemAt: replacement)) != nil,
                      let after = FileVersion.read(path: url.path)
                else { return }
                result = .written(after)
            }
            return failure == nil ? result : .failed
        } ?? .changed
    }

    /// Runs `body` with the file's URL inside its security scope; nil when the bookmark no longer resolves or the file
    /// is in the trash.
    private func withFile<Result>(_ body: (URL) -> Result) -> Result? {
        var stale = false
        guard let url = try? URL(resolvingBookmarkData: bookmark.withLock { $0 }, options: [], relativeTo: nil,
                                 bookmarkDataIsStale: &stale)
        else { return nil }
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        // A file the user deleted is in the trash, where its bookmark still leads: gone, so the user is asked.
        let trash = try? FileManager.default.url(for: .trashDirectory, in: .userDomainMask, appropriateFor: url,
                                                 create: false)
        guard !WriteBack.inTrash(url, trash: trash) else { return nil }
        if stale, let fresh = try? url.bookmarkData() { bookmark.withLock { $0 = fresh } }
        return body(url)
    }
}

/// Asks the user what becomes of a file the app edited after it changed elsewhere, one question at a time in the turn
/// the sheets over game mode share: the app's version over the file, the app's version saved elsewhere (the share
/// sheet), or the app's version let go (the addendum's point 3). Nothing is decided without the user.
@MainActor
final class ConflictQuestion {
    enum Choice: Sendable {
        case replace, savedElsewhere, discard
    }

    static let shared = ConflictQuestion()

    /// One conflict: the app's version kept in Minato's folder (none for a link or a FIFO where the clone was) and
    /// whether Replace can write it (not a file over the limit).
    struct Question {
        let clone: String
        let appVersion: URL?
        let replaceable: Bool
        let answer: @Sendable (Choice) -> Void
    }

    private var waiting: [Question] = []
    private var asking = false

    func ask(_ clone: String, appVersion: URL?, replaceable: Bool, answer: @escaping @Sendable (Choice) -> Void) {
        waiting.append(Question(clone: clone, appVersion: appVersion, replaceable: replaceable, answer: answer))
        next()
    }

    /// A Replace that did not happen (the file out of reach): said once; the question comes again.
    func couldNotReplace(_ clone: String) {
        notice = clone
    }

    private var notice: String?

    private func next() {
        guard !asking, !waiting.isEmpty else { return }
        asking = true
        let question = waiting.removeFirst()
        SheetTurns.shared.take(self) { [weak self] in self?.show(question, attempt: 0) }
    }

    private func show(_ question: Question, attempt: Int) {
        guard let top = OpenPresenter.topController(), top.presentedViewController == nil else {
            // Later, when Minato is in front again with nothing over it; the app's version stays meanwhile.
            DispatchQueue.main.asyncAfter(deadline: .now() + (attempt < 40 ? 0.05 : 2)) { [weak self] in
                self?.show(question, attempt: attempt + 1)
            }
            return
        }
        let failed = notice == question.clone
        if failed { notice = nil }
        let why = "The app's changes could not be written to this file: it changed outside the app, or the app saved "
            + "it in a way that cannot be written back."
        let message = switch (question.replaceable, question.appVersion != nil) {
        case (true, true):
            why + " Replace the file with the app's version, save the app's version as a copy, or discard the app's "
                + "changes?"
        case (true, false):
            why + " Replace the file with the app's version, or discard the app's changes?"
        case (false, true):
            "The app's changes to this file are too large to write back. Save the app's version as a copy, or "
                + "discard the app's changes?"
        case (false, false):
            "The app put something in place of this file that cannot be written back or kept. Its changes are "
                + "discarded."
        }
        let alert = UIAlertController(title: "Changes to “\(question.clone)”",
                                      message: (failed ? "The file could not be replaced. " : "") + message,
                                      preferredStyle: .alert)
        if question.replaceable {
            alert.addAction(UIAlertAction(title: "Replace", style: .destructive) { [weak self] _ in
                self?.answered(.replace, question.answer)
            })
        }
        if let appVersion = question.appVersion {
            alert.addAction(UIAlertAction(title: "Save a Copy…", style: .default) { [weak self] _ in
                self?.share(appVersion, question)
            })
        }
        let discard = question.replaceable || question.appVersion != nil ? "Discard Changes" : "OK"
        alert.addAction(UIAlertAction(title: discard, style: .cancel) { [weak self] _ in
            self?.answered(.discard, question.answer)
        })
        top.present(alert, animated: true)
    }

    /// The app's version in the export picker (a copy, under the clone's name); saved there, the conflict is over,
    /// else asked again. Not the share sheet, which also reports Copy and Print as done, after which Minato's copy
    /// would be gone with nothing saved.
    private func share(_ appVersion: URL, _ question: Question) {
        let folder = FileManager.default.temporaryDirectory.appending(path: "\(OpenPresenter.sharePrefix)\(UUID())")
        let named = folder.appending(path: question.clone)
        guard (try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: false)) != nil,
              (try? FileManager.default.copyItem(at: appVersion, to: named)) != nil,
              let top = OpenPresenter.topController()
        else {
            try? FileManager.default.removeItem(at: folder)
            return again(question)
        }
        let picker = UIDocumentPickerViewController(forExporting: [named], asCopy: true)
        exporting = Export { [weak self] saved in
            try? FileManager.default.removeItem(at: folder)
            self?.exporting = nil
            if saved { self?.answered(.savedElsewhere, question.answer) } else { self?.again(question) }
        }
        picker.delegate = exporting
        picker.presentationController?.delegate = exporting
        top.present(picker, animated: true)
    }

    /// The export picker's delegate while it is up.
    private var exporting: Export?

    /// Tells once whether the export picker saved the file: picked a place, or cancelled or swiped away.
    private final class Export: NSObject, UIDocumentPickerDelegate, UIAdaptivePresentationControllerDelegate {
        private var done: ((Bool) -> Void)?

        init(_ done: @escaping (Bool) -> Void) { self.done = done }

        func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
            finish(!urls.isEmpty)
        }

        func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) { finish(false) }

        func presentationControllerDidDismiss(_ presentationController: UIPresentationController) { finish(false) }

        private func finish(_ saved: Bool) {
            let done = done
            self.done = nil
            done?(saved)
        }
    }

    private func again(_ question: Question) {
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [weak self] in self?.show(question, attempt: 0) }
    }

    private func answered(_ choice: Choice, _ answer: @Sendable (Choice) -> Void) {
        answer(choice)
        asking = false
        SheetTurns.shared.give(self)
        next()
    }
}
