import APKSession
import UIKit
import os

private let log = Logger(subsystem: "io.github.kisame76.minato", category: "session")

/// ADR-0057's host for the guest's pickers over game mode, one request at a time (PickQueue), each answered once.
/// Documents (kinds 1 and 2) in UIDocumentPickerViewController, opened in place, so that each file is read under its
/// own security scope and coordination and cloned into the app's Download folder (DocumentClones); the save dialog,
/// the folder and the photo picker (kinds 3 to 5) answer RESULT_CANCELED until their views come, as does a request in
/// the background. The picker takes its turn with the open requests' sheets (SheetTurns).
@MainActor
final class PickPresenter: NSObject {
    /// One presentation of a request: its ID, a token of its own, as the guest may use an ID again after a cancel, the
    /// most documents it takes, its picker once shown, whether its files are being cloned, and its cancel, which the
    /// clone work reads.
    private struct Shown {
        let id: UInt64
        let token: UInt64
        let most: Int
        var picker: UIDocumentPickerViewController?
        var cloning = false
        let cancel = Cancel()
    }

    /// Set once the guest no longer wants a request; it also cancels the coordinated read of the file being cloned,
    /// which may wait for a download, so that the next request's clones do not wait behind it (the reviewer).
    private final class Cancel: @unchecked Sendable {
        private let lock = NSLock()
        private var set = false
        private var coordinator: NSFileCoordinator?

        var isSet: Bool { lock.withLock { set } }

        func callAsFunction() {
            lock.withLock {
                set = true
                return coordinator
            }?.cancel()
        }

        /// `coordinator` reads the file now (nil when it is done); false when the request is cancelled already.
        func reading(with coordinator: NSFileCoordinator?) -> Bool {
            lock.withLock {
                self.coordinator = coordinator
                return !set
            }
        }
    }

    private var queue = PickQueue()
    private var shown: Shown?
    private var tokens: UInt64 = 0
    private let clones: DocumentClones?
    /// The clones of one request after the other's, each to its end (the lead's reviewer).
    private let work = SerialWork()
    private let answer: (UInt64, Int?) -> Void
    private lazy var steps = SheetSteps<PickQueue.Step>(owner: self) { [unowned self] in perform($0) }

    /// `dataFolder` and `download` are the app's, from the host's records; `answer` gets a request's number of picks,
    /// nil for a cancel.
    init(dataFolder: URL?, download: URL?, answer: @escaping (UInt64, Int?) -> Void) {
        clones = dataFolder.flatMap { data in
            download.map {
                // A document picked again keeps its clone unless the user changed it elsewhere, and no new clone takes
                // a name a write-back record holds (ADR-0057's addendum).
                DocumentClones(dataFolder: data, download: $0,
                               reusable: { WriteBackHost.shared.reusable($0, originalNow: $1) },
                               busy: { WriteBackHost.shared.busy($0) })
            }
        }
        self.answer = answer
    }

    /// The guest's request `id` with the runtime's checked text.
    func requested(id: UInt64, text: String) {
        guard clones != nil else { return answer(id, nil) }
        steps.run(queue.requested(id: id, text: text,
                                  inBackground: UIApplication.shared.applicationState == .background))
    }

    /// The guest no longer wants request `id`: it goes without an answer, its picker with it.
    func cancelled(id: UInt64) {
        steps.run(queue.cancelled(id: id))
    }

    /// One step; a dismissal gives the turn once its picker is gone (SheetSteps).
    private func perform(_ step: PickQueue.Step) -> SheetSteps<PickQueue.Step>.Outcome {
        switch step {
        case .answer(let id, let picked):
            log.info("pick \(id): \(picked.map { "\($0) picked" } ?? "cancelled", privacy: .public)")
            answer(id, picked)
        case .dismiss(let id):
            guard let shown, shown.id == id else { return .next }
            self.shown = nil
            shown.cancel()
            return .gone(shown.picker.map(OpenPresenter.dismissal))
        case .present(let id, let request):
            tokens += 1
            let token = tokens
            shown = Shown(id: id, token: token, most: request.documentsAllowed)
            SheetTurns.shared.take(self) { [weak self] in self?.show(request, token) }
        }
        return .next
    }

    /// Presents the picker over the top controller once nothing of ours is on its way in or out there; after 2 s of
    /// waiting, or without a scene in front, or in the background, the request ends as a cancel.
    private func show(_ request: PickRequest, _ token: UInt64, attempt: Int = 0) {
        // A presentation that ended has given its turn; the turn now is a newer one's, or another owner's.
        guard shown?.token == token else { return }
        guard UIApplication.shared.applicationState != .background else { return ended(token, nil) }
        guard let top = OpenPresenter.topController(), top.presentedViewController == nil else {
            guard attempt < 40 else { return ended(token, nil) }
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) { [weak self] in
                self?.show(request, token, attempt: attempt + 1)
            }
            return
        }
        let picker = UIDocumentPickerViewController(forOpeningContentTypes: request.contentTypes, asCopy: false)
        picker.allowsMultipleSelection = request.several
        picker.delegate = self
        shown?.picker = picker
        top.present(picker, animated: true)
        // A swipe down ends the picker without a word to its delegate.
        picker.presentationController?.delegate = self
    }

    /// Presentation `token` ended with `picked` files, nil for a cancel; nothing for one that is no longer shown.
    private func ended(_ token: UInt64, _ picked: Int?) {
        guard let shown, shown.token == token else { return }
        self.shown = nil
        SheetTurns.shared.give(self)
        steps.run(queue.ended(id: shown.id, picked: picked))
    }

    /// The picked files cloned off the main thread, after the clones of the requests before (SerialWork), so that a
    /// cancelled request's work never removes a clone the next answer names; a cancel before the manifest leaves no
    /// clone, one after it takes the answer back.
    private func clone(_ urls: [URL], token: UInt64) {
        guard let clones, let shown else { return ended(token, nil) }
        self.shown?.cloning = true
        let id = shown.id, most = shown.most, cancel = shown.cancel
        work.run { [weak self] in
            let answer = clones.answer(id: id, count: urls.count, most: most, cancelled: { cancel.isSet }) { i in
                Self.cloned(urls[i], id: id, into: clones, cancel: cancel)
            }
            let taken = await MainActor.run { [weak self] in
                guard let self, self.shown?.token == token else { return false }
                self.ended(token, answer.picked)
                return true
            }
            if !taken { clones.withdraw(answer, id: id) }
        }
    }

    /// One file under its security scope and a coordinated read; nil when it cannot be read or cloned.
    private nonisolated static func cloned(_ url: URL, id: UInt64, into clones: DocumentClones,
                                           cancel: Cancel) -> DocumentClones.Clone? {
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        let coordinator = NSFileCoordinator()
        guard cancel.reading(with: coordinator) else { return nil }
        defer { _ = cancel.reading(with: nil) }
        var clone: DocumentClones.Clone?
        var failure: NSError?
        coordinator.coordinate(readingItemAt: url, options: .withoutChanges, error: &failure) { read in
            clone = clones.clone(DocumentClones.Pick(key: url.standardizedFileURL.path,
                                                     name: url.lastPathComponent, file: read))
        }
        // Its write-back record (ADR-0057's addendum) and bookmark after the coordinated read, still in the file's
        // scope, from the clone's own version: what was cloned is what the original is compared with later.
        if let clone, let version = clone.version {
            WriteBackHost.shared.track(clone: clone.name, original: url, version: version)
        }
        if clone == nil {
            let reason = failure?.localizedDescription ?? "no clone"
            log.error("pick \(id): a file could not be read: \(reason, privacy: .public)")
        }
        return clone
    }
}

extension PickPresenter: @MainActor UIDocumentPickerDelegate {
    func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
        guard let shown, shown.picker === controller, !shown.cloning else { return }
        guard !urls.isEmpty else { return ended(shown.token, nil) }
        clone(urls, token: shown.token)
    }

    func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) {
        guard let shown, shown.picker === controller, !shown.cloning else { return }
        ended(shown.token, nil)
    }
}

extension PickPresenter: @MainActor UIAdaptivePresentationControllerDelegate {
    func presentationControllerDidDismiss(_ presentationController: UIPresentationController) {
        // By its picker: once dismissed, the picker's presentationController is no longer this one.
        guard let shown, let picker = shown.picker, presentationController.presentedViewController === picker,
              !shown.cloning
        else { return }
        ended(shown.token, nil)
    }
}
