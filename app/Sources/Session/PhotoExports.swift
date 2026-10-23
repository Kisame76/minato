import APKSession
import Foundation
import Photos
import os

private let log = Logger(subsystem: "io.github.kisame76.minato", category: "photos")

/// ADR-0057 step e's host: the images and videos an app finished in MediaStore go to the user's Photos. The request
/// handler only takes the file (MediaExports); PhotoExportQueue copies it from that descriptor into Minato's own
/// folder and hands Photos the copies one at a time, never the guest's path; this adds each with add-only access,
/// after iOS's one prompt.
enum PhotoExports {
    /// The copies' folder, in Minato's temporary folder.
    static let folder = FileManager.default.temporaryDirectory.appending(path: "photo-exports")

    /// The queue of a session, with the access iOS reports now.
    static func queue() -> PhotoExportQueue {
        let access = PHPhotoLibrary.authorizationStatus(for: .addOnly)
        return PhotoExportQueue(folder: folder, declined: access == .denied || access == .restricted) {
            await add($0, $1, $2)
        }
    }

    /// At each launch, in either mode: the copies an earlier process left (a kill, or a settle that ran out).
    static func removeEarlierCopies() {
        PhotoExportQueue.removeEarlierCopies(folder)
    }

    /// iOS asks the user once; add-only access never shows the library to Minato.
    private static func add(_ kind: MediaExportJob.Kind, _ copy: URL,
                            _ name: String) async -> PhotoExportQueue.Outcome {
        let access = await PHPhotoLibrary.requestAuthorization(for: .addOnly)
        guard access == .authorized || access == .limited else {
            log.info("export: no access to Photos")
            return .declined
        }
        do {
            try await PHPhotoLibrary.shared().performChanges {
                let options = PHAssetResourceCreationOptions()
                options.originalFilename = name
                // The copy is Minato's own: Photos takes it rather than making a copy of its own.
                options.shouldMoveFile = true
                PHAssetCreationRequest.forAsset()
                    .addResource(with: kind == .image ? .photo : .video, fileURL: copy, options: options)
            }
            log.info("export: added to Photos")
            return .added
        } catch {
            // The error may name the app's file.
            log.info("export: Photos refused it: \(error.localizedDescription, privacy: .private)")
            return .refused
        }
    }
}
