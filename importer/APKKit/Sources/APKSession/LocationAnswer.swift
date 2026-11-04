import Darwin
import Foundation

/// The answer to a save or a folder request (ADR-0057 step d, kinds 3 and 4) on the iPhone, once the user allowed it:
/// the place is the app's own Documents folder, which the guest sees as /storage/emulated/0/Documents. A save is a new
/// empty file there under its title, made plain and unique ("note (1).txt"), as runtime/tools/pick_host.h's location
/// makes it in Download; a folder is Documents itself. The manifest ".minato/picks/<ID>" below the app's data folder
/// names the place relative to /storage/emulated/0 (PickLocation). The app can write Documents and leave links there,
/// so the host opens it once from its own records, through its parent with O_NOFOLLOW, and makes the file with O_EXCL:
/// a name a link takes is busy, never followed.
public struct LocationAnswer: Sendable {
    /// The guest's name of the folder, below /storage/emulated/0.
    public static let folderName = "Documents"

    private let dataFolder: URL
    private let documents: URL

    /// `dataFolder` holds the manifests (/data/data/<package>), `documents` is the host's Documents folder.
    public init(dataFolder: URL, documents: URL) {
        self.dataFolder = dataFolder
        self.documents = documents
    }

    /// Makes the place of request `id` and writes its manifest: 1, the number of places, or nil for a cancel (a kind
    /// with no place, a link or no folder where Documents belongs, a file that cannot be made, no manifest).
    public func answer(id: UInt64, _ request: PickRequest) -> Int? {
        guard request.kind == .createDocument || request.kind == .openTree else { return nil }
        let parent = Darwin.open(documents.deletingLastPathComponent().path,
                                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard parent >= 0 else { return nil }
        defer { close(parent) }
        guard let folder = DocumentClones.openSub(parent, documents.lastPathComponent) else { return nil }
        defer { close(folder) }
        guard request.kind == .createDocument else {
            return DocumentClones.writeManifest(in: dataFolder, id: id, Self.folderName + "\n") ? 1 : nil
        }
        guard let name = Self.createNew(DocumentClones.plainName(Array(request.title.utf8)), in: folder) else {
            return nil
        }
        guard DocumentClones.writeManifest(in: dataFolder, id: id, "\(Self.folderName)/\(name)\n") else {
            unlinkat(folder, name, 0)
            return nil
        }
        return 1
    }

    /// The first free numbered form of `name` made in `folder` with O_EXCL (pick_host.h's create_new); nil when none
    /// can be.
    static func createNew(_ name: String, in folder: Int32) -> String? {
        for n in 0..<10000 {
            let chosen = DocumentClones.numbered(name, n)
            let file = openat(folder, chosen, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0o644)
            if file >= 0 {
                close(file)
                return chosen
            }
            guard errno == EEXIST else { return nil }
        }
        return nil
    }
}
