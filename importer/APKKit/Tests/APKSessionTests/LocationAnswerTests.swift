import Darwin
import Foundation
import Testing

@testable import APKSession

/// ADR-0057 step d on the iPhone: a save (ACTION_CREATE_DOCUMENT) is made in the app's Documents folder under its
/// title, made unique, and a folder (ACTION_OPEN_DOCUMENT_TREE) is Documents itself; the manifest names the place
/// relative to /storage/emulated/0, through directory descriptors only, as runtime/tools/pick_host.h's location does.
@Suite("LocationAnswer")
final class LocationAnswerTests {
    let root: URL
    let data: URL
    let documents: URL
    let outside: URL

    init() throws {
        root = FileManager.default.temporaryDirectory.appending(path: "location-answer-\(UUID().uuidString)")
        data = root.appending(path: "data")
        documents = root.appending(path: "documents")
        outside = root.appending(path: "outside")
        for folder in [data, documents, outside] {
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        }
    }

    deinit {
        try? FileManager.default.removeItem(at: root)
    }

    var answer: LocationAnswer { LocationAnswer(dataFolder: data, documents: documents) }

    static func save(_ title: String) -> PickRequest { PickRequest(text: "3\t0\t0\ttext/plain\t\(title)")! }
    static let tree = PickRequest(text: "4\t0\t0\t\t")!

    func manifest(_ id: Int) -> String? {
        try? String(contentsOf: data.appending(path: ".minato/picks/\(id)"), encoding: .utf8)
    }

    func names(_ folder: URL) -> [String] {
        ((try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? []).sorted()
    }

    @Test func aFolderIsDocuments() {
        #expect(answer.answer(id: 7, Self.tree) == 1)
        #expect(manifest(7) == "Documents\n")
        #expect(names(documents) == [])
    }

    @Test func aFolderTheAppRemovedIsMadeAgain() throws {
        try FileManager.default.removeItem(at: documents)
        #expect(answer.answer(id: 1, Self.tree) == 1)
        #expect(manifest(1) == "Documents\n")
        var status = stat()
        #expect(lstat(documents.path, &status) == 0 && status.st_mode & S_IFMT == S_IFDIR)
    }

    @Test func aSaveIsMadeUnderItsTitleAndNumberedWhenTaken() throws {
        #expect(answer.answer(id: 1, Self.save("note.txt")) == 1)
        #expect(manifest(1) == "Documents/note.txt\n")
        #expect(answer.answer(id: 2, Self.save("note.txt")) == 1)
        #expect(manifest(2) == "Documents/note (1).txt\n")
        #expect(names(documents) == ["note (1).txt", "note.txt"])
        let made = try FileManager.default.attributesOfItem(atPath: documents.appending(path: "note.txt").path)
        #expect(made[.size] as? Int == 0)
        #expect((made[.posixPermissions] as? Int).map { $0 & 0o777 } == 0o644)
    }

    @Test func aTitleIsMadePlain() {
        #expect(answer.answer(id: 1, Self.save("")) == 1)
        #expect(manifest(1) == "Documents/Untitled\n")
        #expect(answer.answer(id: 2, Self.save("a/b.txt")) == 1)
        #expect(manifest(2) == "Documents/a_b.txt\n")
    }

    /// The app can leave links in Documents: a name taken by one is busy, never followed.
    @Test func aLinkAtTheTitleIsNotFollowed() throws {
        let target = outside.appending(path: "note.txt")
        try FileManager.default.createSymbolicLink(atPath: documents.appending(path: "note.txt").path,
                                                   withDestinationPath: target.path)
        #expect(answer.answer(id: 1, Self.save("note.txt")) == 1)
        #expect(manifest(1) == "Documents/note (1).txt\n")
        #expect(names(outside) == [])
    }

    /// A link where Documents should be is not followed: the answer is a cancel, nothing is made outside.
    @Test func aLinkForDocumentsIsACancel() throws {
        try FileManager.default.removeItem(at: documents)
        try FileManager.default.createSymbolicLink(atPath: documents.path, withDestinationPath: outside.path)
        #expect(answer.answer(id: 1, Self.save("note.txt")) == nil)
        #expect(answer.answer(id: 2, Self.tree) == nil)
        #expect(manifest(1) == nil && manifest(2) == nil)
        #expect(names(outside) == [])
    }

    /// Without a manifest the guest never hears of the file, so the save takes it back.
    @Test func aSaveWithoutAManifestLeavesNoFile() throws {
        try Data().write(to: data.appending(path: ".minato"))
        #expect(answer.answer(id: 1, Self.save("note.txt")) == nil)
        #expect(names(documents) == [])
    }

    /// Only the save and the folder have a place to answer.
    @Test func otherKindsHaveNoPlace() {
        #expect(answer.answer(id: 1, PickRequest(text: "1\t0\t0\ttext/plain\t")!) == nil)
        #expect(manifest(1) == nil)
    }
}
