import APKSession
import Foundation
import Photos
import Synchronization
import os

private let log = Logger(subsystem: "io.github.kisame76.minato", category: "photos")

/// ADR-0057 step e's host: the images and videos an app finished in MediaStore go to the user's Photos. The request
/// handler only takes the file (MediaExports); this copies it from that descriptor into a folder of Minato's own, off
/// the guest's thread, and gives Photos only the copy, never the guest's path. Photos gets them one at a time, with
/// add-only access after iOS's one prompt, and nothing once the user declined it; each copy goes once it is added or
/// refused.
final class PhotoExports: Sendable {
    /// The copies' folder, emptied of an earlier process's copies once per process, at the first copy: SwiftUI may
    /// make a session's state more than once, and a second one must not empty the folder under the first.
    private static let folder: URL = {
        let folder = FileManager.default.temporaryDirectory.appending(path: "photo-exports")
        try? FileManager.default.removeItem(at: folder)
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        return folder
    }()
    /// The copies are made here, not on Swift's cooperative threads: one that is not a clone reads up to 1 GiB.
    private static let copying = DispatchQueue(label: "io.github.kisame76.minato.photo-exports", qos: .utility)

    private let library = PhotoLibraryAdds()
    /// The exports taken and not yet added or refused.
    private let pending = Mutex(0)

    /// The user declined Minato's access to Photos (or a profile restricts it): nothing more is taken.
    var declined: Bool { library.declined }

    /// From the request handler: the copy is made on a queue of its own, so the guest's thread returns at once; the
    /// job's descriptor is closed once the copy is made.
    func export(_ job: MediaExportJob) {
        pending.withLock { $0 += 1 }
        Self.copying.async { [self] in
            guard let copy = job.copy(into: Self.folder) else {
                log.error("export: the copy failed")
                pending.withLock { $0 -= 1 }
                return
            }
            let kind = job.kind
            let name = job.shownName
            Task {
                await library.add(copy, kind: kind, name: name)
                pending.withLock { $0 -= 1 }
            }
        }
    }

    /// Waits up to `seconds` for the exports under way, before the session ends the process (apk-run's model waits
    /// 5 s too): a photo saved just before the app finishes still reaches Photos.
    func settle(within seconds: Double) async {
        let clock = ContinuousClock()
        let deadline = clock.now + .seconds(seconds)
        while pending.withLock({ $0 }) > 0 && clock.now < deadline {
            try? await Task.sleep(for: .milliseconds(50))
        }
    }
}

/// Photos' additions, one at a time.
private final class PhotoLibraryAdds: Sendable {
    private struct Waiting {
        let copy: URL
        let kind: MediaExportJob.Kind
        let name: String
    }

    private struct State {
        var waiting: [Waiting] = []
        var running = false
        var declined: Bool
    }

    private let state: Mutex<State>

    init() {
        let access = PHPhotoLibrary.authorizationStatus(for: .addOnly)
        state = Mutex(State(declined: access == .denied || access == .restricted))
    }

    var declined: Bool { state.withLock { $0.declined } }

    /// Adds the copy after those before it; the first caller adds them all, until none waits.
    func add(_ copy: URL, kind: MediaExportJob.Kind, name: String) async {
        let first = state.withLock { state in
            state.waiting.append(Waiting(copy: copy, kind: kind, name: name))
            defer { state.running = true }
            return !state.running
        }
        guard first else { return }
        while true {
            let next = state.withLock { state -> Waiting? in
                guard !state.waiting.isEmpty else {
                    state.running = false
                    return nil
                }
                return state.waiting.removeFirst()
            }
            guard let next else { return }
            await addToPhotos(next)
            try? FileManager.default.removeItem(at: next.copy.deletingLastPathComponent())
        }
    }

    /// iOS asks the user once; add-only access never shows the library to Minato.
    private func addToPhotos(_ export: Waiting) async {
        let access = await PHPhotoLibrary.requestAuthorization(for: .addOnly)
        guard access == .authorized || access == .limited else {
            state.withLock { $0.declined = true }
            log.info("export: no access to Photos")
            return
        }
        do {
            try await PHPhotoLibrary.shared().performChanges {
                let options = PHAssetResourceCreationOptions()
                options.originalFilename = export.name
                // The copy is Minato's own: Photos takes it rather than making a copy of its own.
                options.shouldMoveFile = true
                PHAssetCreationRequest.forAsset()
                    .addResource(with: export.kind == .image ? .photo : .video, fileURL: export.copy, options: options)
            }
            log.info("export: added to Photos")
        } catch {
            // Not a picture or a video Photos takes: the kind is the app's word only.
            log.info("export: Photos refused it: \(error.localizedDescription, privacy: .public)")
        }
    }
}
