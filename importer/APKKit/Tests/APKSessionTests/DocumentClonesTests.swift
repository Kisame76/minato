import Darwin
import Foundation
import Synchronization
import Testing

@testable import APKSession

/// ADR-0057 step b on the iPhone: the files the user picks are cloned into the app's Download folder and named in the
/// manifest below its data folder, through directory descriptors only, as runtime/tools/pick_host.h does on the Mac
/// (pick_host_test's cases).
@Suite("DocumentClones")
final class DocumentClonesTests {
    let root: URL
    let data: URL
    let download: URL
    let outside: URL
    let source: URL

    init() throws {
        root = FileManager.default.temporaryDirectory.appending(path: "document-clones-\(UUID().uuidString)")
        data = root.appending(path: "data")
        download = root.appending(path: "download")
        outside = root.appending(path: "outside")
        source = root.appending(path: "source")
        for folder in [data, download, outside, source] {
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        }
        try Data("hello\n".utf8).write(to: source.appending(path: "known.txt"))
        try Data([1, 2, 3, 4]).write(to: source.appending(path: "data.bin"))
    }

    deinit {
        chflags(source.appending(path: "locked.txt").path, 0)
        try? FileManager.default.removeItem(at: root)
    }

    var known: DocumentClones.Pick { pick("known.txt") }
    var bin: DocumentClones.Pick { pick("data.bin") }

    func pick(_ name: String) -> DocumentClones.Pick {
        DocumentClones.Pick(key: source.appending(path: name).path, name: name, file: source.appending(path: name))
    }

    func clones(cloning: Bool = true) -> DocumentClones {
        DocumentClones(dataFolder: data, download: download, cloning: cloning)
    }

    func manifest(_ id: Int) -> String? {
        try? String(contentsOf: data.appending(path: ".minato/picks/\(id)"), encoding: .utf8)
    }

    func names(_ folder: URL) -> [String] {
        ((try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? []).sorted()
    }

    func text(_ url: URL) -> String? { try? String(contentsOf: url, encoding: .utf8) }

    func exists(_ url: URL) -> Bool {
        var status = stat()
        return lstat(url.path, &status) == 0
    }

    func link(_ at: URL, to: String) { #expect(symlink(to, at.path) == 0) }

    @Test func aPickIsClonedAndItsManifestWrittenWhole() {
        let clones = clones()
        #expect(clones.answer(id: 1, picks: [known, bin]) == 2)
        #expect(manifest(1) == "known.txt\ndata.bin\n")
        #expect(text(download.appending(path: "known.txt")) == "hello\n")
        #expect(names(data.appending(path: ".minato/picks")) == ["1"])
        #expect(clones.answer(id: 2, picks: [known]) == 1)
        #expect(manifest(2) == "known.txt\n")
        #expect(names(download) == ["data.bin", "known.txt"])
        #expect(text(source.appending(path: "known.txt")) == "hello\n")  // untouched
    }

    @Test func aDanglingLinkWhereACloneGoesIsNotWrittenThrough() {
        link(download.appending(path: "known.txt"), to: outside.appending(path: "x").path)
        #expect(clones().answer(id: 1, picks: [known]) == 1)
        #expect(!exists(outside.appending(path: "x")))
        #expect(manifest(1) == "known (1).txt\n")
        #expect(text(download.appending(path: "known (1).txt")) == "hello\n")
    }

    @Test func aFileWhereACloneGoesIsNotWrittenOver() throws {
        try Data("the app's".utf8).write(to: download.appending(path: "known.txt"))
        #expect(clones().answer(id: 1, picks: [known]) == 1)
        #expect(text(download.appending(path: "known.txt")) == "the app's")
        #expect(manifest(1) == "known (1).txt\n")
    }

    @Test func aLinkPlantedAsTheManifestsTemporaryFileIsReplacedNotFollowed() throws {
        try Data("keep me".utf8).write(to: outside.appending(path: "victim"))
        try FileManager.default.createDirectory(at: data.appending(path: ".minato/picks"),
                                                withIntermediateDirectories: true)
        link(data.appending(path: ".minato/picks/1.partial"), to: outside.appending(path: "victim").path)
        #expect(clones().answer(id: 1, picks: [known]) == 1)
        #expect(text(outside.appending(path: "victim")) == "keep me")
        #expect(manifest(1) == "known.txt\n")
        #expect(!exists(data.appending(path: ".minato/picks/1.partial")))
    }

    @Test func aLinkPlantedAsTheManifestItselfIsReplacedNotFollowed() throws {
        try Data("keep me".utf8).write(to: outside.appending(path: "victim"))
        try FileManager.default.createDirectory(at: data.appending(path: ".minato/picks"),
                                                withIntermediateDirectories: true)
        link(data.appending(path: ".minato/picks/1"), to: outside.appending(path: "victim").path)
        #expect(clones().answer(id: 1, picks: [known]) == 1)
        #expect(text(outside.appending(path: "victim")) == "keep me")
        #expect(manifest(1) == "known.txt\n")
    }

    @Test func aLinkAsAFolderOfTheDataFolderMakesTheAnswerACancel() throws {
        let elsewhere = outside.appending(path: "elsewhere")
        try FileManager.default.createDirectory(at: elsewhere, withIntermediateDirectories: true)
        link(data.appending(path: ".minato"), to: elsewhere.path)
        #expect(clones().answer(id: 1, picks: [known]) == nil)
        #expect(names(elsewhere).isEmpty)
        try FileManager.default.removeItem(at: data.appending(path: ".minato"))
        try FileManager.default.createDirectory(at: data.appending(path: ".minato"), withIntermediateDirectories: true)
        link(data.appending(path: ".minato/picks"), to: elsewhere.path)
        #expect(clones().answer(id: 2, picks: [known]) == nil)
        #expect(names(elsewhere).isEmpty)
    }

    @Test func aNameThatLinksToItselfDoesNotStopTheHost() {
        link(download.appending(path: "known.txt"), to: "known.txt")
        link(download.appending(path: "loop"), to: "loop")
        #expect(clones().answer(id: 1, picks: [known]) == 1)
        #expect(manifest(1) == "known (1).txt\n")
    }

    @Test func aFilePickedAgainIsOneDocumentUnlessTheAppReplacedItsClone() throws {
        let clones = clones()
        #expect(clones.answer(id: 1, picks: [known]) == 1)
        #expect(clones.answer(id: 2, picks: [known]) == 1)
        #expect(manifest(1) == manifest(2))
        #expect(names(download) == ["known.txt"])
        try FileManager.default.removeItem(at: download.appending(path: "known.txt"))
        #expect(clones.answer(id: 3, picks: [known]) == 1)
        #expect(manifest(3) == "known.txt\n")
        #expect(text(download.appending(path: "known.txt")) == "hello\n")
        try FileManager.default.removeItem(at: download.appending(path: "known.txt"))
        link(download.appending(path: "known.txt"), to: outside.appending(path: "x").path)
        #expect(clones.answer(id: 4, picks: [known]) == 1)
        #expect(manifest(4) == "known (1).txt\n")
        #expect(!exists(outside.appending(path: "x")))
    }

    /// pick_host_test's a_file_picked_again_after_the_user_changed_it_gets_a_new_clone_unless_the_app_has_edits, with
    /// the write-back's records; the picked file's version is read by the clone, from the descriptor it clones from.
    @Test func aFilePickedAgainAfterTheUserChangedItGetsANewCloneUnlessTheAppHasEdits() throws {
        let state = root.appending(path: "host-state")
        try FileManager.default.createDirectory(at: state, withIntermediateDirectories: true)
        let writeBack = WriteBack(download: download, state: state)
        let clones = DocumentClones(dataFolder: data, download: download,
                                    reusable: { writeBack.reusable($0, originalNow: $1) },
                                    busy: { writeBack.busy($0) })
        let original = PathOriginal(path: source.appending(path: "known.txt").path)
        func pickKnown(_ id: UInt64) -> String? {
            guard let clone = clones.clone(known) else { return nil }
            if clone.made, let base = original.version() {
                writeBack.track(clone: clone.name, original: original, originalNow: base, cloneNow: base)
            }
            return clones.finish(id: id, clones: [clone]) == 1 ? manifest(Int(id)) : nil
        }
        #expect(pickKnown(1) == "known.txt\n")
        try Data("changed elsewhere\n".utf8).write(to: source.appending(path: "known.txt"))  // the user edits it
        #expect(pickKnown(2) == "known (1).txt\n")
        #expect(text(download.appending(path: "known (1).txt")) == "changed elsewhere\n")
        #expect(text(download.appending(path: "known.txt")) == "hello\n")  // the old clone stays as it was
        #expect(pickKnown(3) == "known (1).txt\n")  // now the same again
        // An app with unsent edits keeps its clone: the write-back decides what becomes of the original.
        try Data("app edit\n".utf8).write(to: download.appending(path: "known (1).txt"))
        writeBack.closed("known (1).txt", at: 1000)
        try Data("changed again\n".utf8).write(to: source.appending(path: "known.txt"))
        #expect(pickKnown(4) == "known (1).txt\n")
        #expect(text(download.appending(path: "known (1).txt")) == "app edit\n")
    }

    /// The review, point 6: a clone says the version it was made with, read from the clone itself, which its
    /// write-back record starts from (the original read again may have changed meanwhile); a clone used again has none.
    @Test func aCloneSaysTheVersionItWasMadeWith() throws {
        let clones = clones()
        let clone = try #require(clones.clone(known))
        let version = try #require(clone.version)
        #expect(version == FileVersion.read(path: download.appending(path: clone.name).path))
        #expect(version.digest.count == 64 && version.size == 6)
        #expect(clones.clone(known)?.version == nil)
    }

    /// A name a write-back record holds (a conflict whose clone the guest removed) is not taken by a new clone, which
    /// would orphan the record's kept version (the reviewer).
    @Test func aNameARecordHoldsIsNotTakenByANewClone() {
        let clones = DocumentClones(dataFolder: data, download: download, cloning: true,
                                    reusable: { _, _ in true }, busy: { $0 == "known.txt" })
        #expect(clones.answer(id: 1, picks: [known]) == 1)
        #expect(manifest(1) == "known (1).txt\n")
    }

    /// The version a reuse is decided by is the picked file's, read from the descriptor the clone would come from,
    /// and only asked when there is an earlier clone.
    @Test func theReuseIsDecidedByThePickedFilesVersion() throws {
        let asked = Mutex([String]())
        let clones = DocumentClones(dataFolder: data, download: download, cloning: true,
                                    reusable: { name, now in
                                        asked.withLock { $0.append(name + " " + (now()?.digest.prefix(8) ?? "none")) }
                                        return false
                                    }, busy: { _ in false })
        #expect(clones.answer(id: 1, picks: [known]) == 1)
        #expect(asked.withLock { $0 }.isEmpty)
        #expect(clones.answer(id: 2, picks: [known]) == 1)
        let digest = try #require(FileVersion.read(path: source.appending(path: "known.txt").path)).digest
        #expect(asked.withLock { $0 } == ["known.txt " + digest.prefix(8)])
        #expect(manifest(2) == "known (1).txt\n")
    }

    @Test func twoFilesOfOneNameGetTwoClones() throws {
        try FileManager.default.createDirectory(at: source.appending(path: "b"), withIntermediateDirectories: true)
        try Data("other\n".utf8).write(to: source.appending(path: "b/known.txt"))
        let other = DocumentClones.Pick(key: source.appending(path: "b/known.txt").path, name: "known.txt",
                                        file: source.appending(path: "b/known.txt"))
        #expect(clones().answer(id: 1, picks: [known, other]) == 2)
        #expect(manifest(1) == "known.txt\nknown (1).txt\n")
        #expect(text(download.appending(path: "known (1).txt")) == "other\n")
    }

    @Test func aFileThatCannotBeReadMakesTheAnswerACancelAndLeavesNoCloneItMade() {
        let missing = DocumentClones.Pick(key: "missing", name: "missing.txt",
                                          file: source.appending(path: "missing.txt"))
        let clones = clones()
        #expect(clones.answer(id: 1, picks: [known, missing]) == nil)
        #expect(manifest(1) == nil)
        #expect(names(download).isEmpty)
        #expect(clones.answer(id: 2, picks: [known]) == 1)
        #expect(clones.answer(id: 3, picks: [known, bin, missing]) == nil)
        #expect(names(download) == ["known.txt"])  // the earlier clone of a document picked again stays
        #expect(clones.answer(id: 4, picks: [bin]) == 1)
        #expect(manifest(4) == "data.bin\n")  // its name was freed with the clone
    }

    /// pick_host_test's a_name_freed_by_the_app_never_hands_back_another_document.
    @Test func aNameFreedByTheAppNeverHandsBackAnotherDocument() throws {
        try FileManager.default.createDirectory(at: source.appending(path: "two"), withIntermediateDirectories: true)
        try Data("second\n".utf8).write(to: source.appending(path: "two/known.txt"))
        let second = DocumentClones.Pick(key: source.appending(path: "two/known.txt").path, name: "known.txt",
                                         file: source.appending(path: "two/known.txt"))
        let clones = clones()
        #expect(clones.answer(id: 1, picks: [known]) == 1)
        try FileManager.default.removeItem(at: download.appending(path: "known.txt"))  // the app deletes it
        #expect(clones.answer(id: 2, picks: [second]) == 1)
        #expect(manifest(2) == "known.txt\n")
        #expect(clones.answer(id: 3, picks: [known]) == 1)
        #expect(manifest(3) == "known (1).txt\n")
        #expect(text(download.appending(path: "known (1).txt")) == "hello\n")
        #expect(clones.answer(id: 4, picks: [second]) == 1)
        #expect(manifest(4) == "known.txt\n")
    }

    @Test func aReadOnlyOrLockedFileGivesACloneTheAppCanEditAndRemove() throws {
        try Data("locked\n".utf8).write(to: source.appending(path: "locked.txt"))
        #expect(chmod(source.appending(path: "locked.txt").path, 0o400) == 0)
        #expect(chflags(source.appending(path: "locked.txt").path, UInt32(UF_IMMUTABLE)) == 0)
        for cloning in [true, false] {
            let clones = clones(cloning: cloning)
            #expect(clones.answer(id: 1, picks: [pick("locked.txt")]) == 1)
            let clone = download.appending(path: "locked.txt")
            var status = stat()
            #expect(lstat(clone.path, &status) == 0)
            #expect(status.st_mode & 0o777 == 0o644)
            #expect(status.st_flags == 0)
            #expect(text(clone) == "locked\n")
            #expect(unlink(clone.path) == 0)
        }
    }

    @Test func aLinkOrAFIFOPickedIsNoFile() throws {
        try Data("secret".utf8).write(to: outside.appending(path: "secret.txt"))
        link(source.appending(path: "link.txt"), to: outside.appending(path: "secret.txt").path)
        #expect(clones().answer(id: 1, picks: [pick("link.txt")]) == nil)
        #expect(mkfifo(source.appending(path: "pipe.txt").path, 0o600) == 0)
        #expect(clones().answer(id: 2, picks: [pick("pipe.txt")]) == nil)  // returns
        try FileManager.default.createDirectory(at: source.appending(path: "folder.txt/inner"),
                                                withIntermediateDirectories: true)
        #expect(clones().answer(id: 3, picks: [pick("folder.txt")]) == nil)  // clonefile would clone a folder
        #expect(names(download).isEmpty)
    }

    @Test func theCopyWithoutACloneNamesAndWritesAlike() throws {
        link(download.appending(path: "known.txt"), to: outside.appending(path: "x").path)
        try Data("the app's".utf8).write(to: download.appending(path: "data.bin"))
        let big = Data((0..<300_000).map { UInt8(truncatingIfNeeded: $0 &* 7) })
        try big.write(to: source.appending(path: "big.bin"))
        #expect(clones(cloning: false).answer(id: 1, picks: [known, bin, pick("big.bin")]) == 3)
        #expect(manifest(1) == "known (1).txt\ndata (1).bin\nbig.bin\n")
        #expect(!exists(outside.appending(path: "x")))
        #expect(text(download.appending(path: "data.bin")) == "the app's")
        #expect(try Data(contentsOf: download.appending(path: "big.bin")) == big)
    }

    @Test func aFolderPlantedWhereTheManifestGoes() throws {
        let picks = data.appending(path: ".minato/picks")
        try FileManager.default.createDirectory(at: picks.appending(path: "1.partial"), withIntermediateDirectories: true)
        #expect(clones().answer(id: 1, picks: [known]) == 1)  // under a name at random
        #expect(manifest(1) == "known.txt\n")
        try FileManager.default.createDirectory(at: picks.appending(path: "2"), withIntermediateDirectories: true)
        #expect(clones().answer(id: 2, picks: [bin]) == nil)
        #expect(names(picks) == ["1", "1.partial", "2"])  // no temporary file left
        #expect(!exists(download.appending(path: "data.bin")))
    }

    @Test func atMostAThousandPicks() {
        let many = (0..<1001).map { DocumentClones.Pick(key: "k\($0)", name: "n\($0).txt",
                                                        file: source.appending(path: "known.txt")) }
        #expect(clones().answer(id: 1, picks: many) == nil)
        #expect(names(download).isEmpty)
        #expect(clones().answer(id: 2, picks: Array(many.prefix(1000))) == 1000)
    }

    @Test func noPickIsACancel() {
        #expect(clones().answer(id: 1, picks: []) == nil)
        #expect(manifest(1) == nil)
    }

    /// The limits come before any clone: more than 1000 picks, or more than one for a request of one document
    /// (without EXTRA_ALLOW_MULTIPLE), clone nothing (the review).
    @Test func theLimitsComeBeforeAnyClone() {
        let clones = clones()
        var cloned = 0
        let count = { (_: Int) -> DocumentClones.Clone? in
            cloned += 1
            return nil
        }
        #expect(clones.answer(id: 1, count: 1001, most: .max, cancelled: { false }, cloning: count).picked == nil)
        #expect(clones.answer(id: 2, count: 2, most: 1, cancelled: { false }, cloning: count).picked == nil)
        #expect(clones.answer(id: 3, count: 0, most: 1, cancelled: { false }, cloning: count).picked == nil)
        #expect(cloned == 0)
        #expect(clones.answer(id: 4, count: 1, most: 1, cancelled: { false }) { _ in clones.clone(self.known) }
            .picked == 1)
    }

    @Test func aManifestOfMoreThanAThousandIsACancelThatLeavesNoClone() {
        let clones = clones()
        let made = (0..<1001).compactMap { n in
            clones.clone(DocumentClones.Pick(key: "k\(n)", name: "n\(n).txt", file: source.appending(path: "known.txt")))
        }
        #expect(made.count == 1001)
        #expect(clones.finish(id: 1, clones: made) == nil)
        #expect(manifest(1) == nil)
        #expect(names(download).isEmpty)
    }

    /// A request the guest cancelled while its files were cloned writes no manifest and leaves none of the clones it
    /// made (the review).
    @Test func aCancelWhileCloningLeavesNoManifestAndNoClone() {
        let clones = clones()
        var cancelled = false
        let picks = [known, bin]
        let answer = clones.answer(id: 1, count: 2, most: 2, cancelled: { cancelled }) { i in
            defer { if i == 1 { cancelled = true } }  // after the last clone, before the manifest
            return clones.clone(picks[i])
        }
        #expect(answer.picked == nil)
        #expect(manifest(1) == nil)
        #expect(names(download).isEmpty)
    }

    /// A cancel stops the cloning: no file after it is cloned.
    @Test func aCancelStopsTheCloning() {
        let clones = clones()
        var cancelled = false, cloned = 0
        let picks = [known, bin]
        let answer = clones.answer(id: 1, count: 2, most: 2, cancelled: { cancelled }) { i in
            cloned += 1
            cancelled = true
            return clones.clone(picks[i])
        }
        #expect(answer.picked == nil)
        #expect(cloned == 1)
        #expect(names(download).isEmpty)
    }

    /// A cancel that came once the manifest was written takes back the answer: its manifest and the clones it made;
    /// the earlier clone of a document picked again stays.
    @Test func aWithdrawnAnswerLeavesNoManifestAndOnlyTheEarlierClones() {
        let clones = clones()
        #expect(clones.answer(id: 1, picks: [known]) == 1)
        let picks = [known, bin]
        let answer = clones.answer(id: 2, count: 2, most: 2, cancelled: { false }) { clones.clone(picks[$0]) }
        #expect(answer.picked == 2)
        clones.withdraw(answer, id: 2)
        #expect(manifest(2) == nil)
        #expect(manifest(1) == "known.txt\n")
        #expect(names(download) == ["known.txt"])
        #expect(clones.answer(id: 3, picks: [bin]) == 1)
        #expect(manifest(3) == "data.bin\n")
    }

    /// The Download folder is the root of the guest's mount, so nothing should stand in its place; a link there all the
    /// same is never followed, by a clone or a cancel's removal (the review).
    @Test func aLinkWhereTheDownloadFolderBelongsIsNotFollowed() throws {
        let clones = clones()
        #expect(clones.answer(id: 1, picks: [known]) == 1)
        let made = try #require(clones.clone(bin))
        try FileManager.default.removeItem(at: download)
        link(download, to: outside.path)
        try Data("outside".utf8).write(to: outside.appending(path: "data.bin"))
        clones.discard([made])
        #expect(text(outside.appending(path: "data.bin")) == "outside")
        #expect(clones.answer(id: 2, picks: [pick("data.bin")]) == nil)
        #expect(names(outside) == ["data.bin"])
    }

    /// The copy for a file that cannot be cloned (another volume) leaves the device room: a file that does not fit is
    /// a cancel and leaves nothing.
    @Test func aCopyThatDoesNotFitIsACancel() {
        let full = DocumentClones(dataFolder: data, download: download, cloning: false, freeSpace: { _ in 1 << 20 })
        #expect(full.answer(id: 1, picks: [known]) == nil)
        #expect(names(download).isEmpty)
        #expect(DocumentClones.fits(0, free: DocumentClones.copyReserve))
        #expect(!DocumentClones.fits(1, free: DocumentClones.copyReserve))
        #expect(!DocumentClones.fits(-1, free: .max))
        #expect(DocumentClones.fits(1 << 30, free: DocumentClones.copyReserve + (1 << 30)))
    }

    @Test func aBigFileIsClonedWhole() throws {
        let big = Data((0..<300_000).map { UInt8(truncatingIfNeeded: $0 &* 7) })
        try big.write(to: source.appending(path: "big.bin"))
        #expect(clones().answer(id: 1, picks: [pick("big.bin")]) == 1)
        #expect(try Data(contentsOf: download.appending(path: "big.bin")) == big)
    }

    @Test func aUsersFileNameBecomesANameTheGuestAccepts() {
        func plain(_ bytes: [UInt8]) -> String { DocumentClones.plainName(bytes) }
        func plain(_ text: String) -> String { DocumentClones.plainName(Array(text.utf8)) }
        #expect(plain("known.txt") == "known.txt")
        #expect(plain("note 😀 ä.txt") == "note 😀 ä.txt")
        #expect(plain("a/b") == "a_b")
        #expect(plain("a\tb\nc\rd") == "a_b_c_d")
        #expect(plain("a\u{200D}b") == "a_b")  // zero-width joiner
        #expect(plain("a\u{202E}b") == "a_b")  // right-to-left override
        #expect(plain("a\u{2028}b") == "a_b")  // line separator
        #expect(plain("a\u{FEFF}b") == "a_b")  // byte order mark
        #expect(plain("a\u{061C}b\u{034F}c") == "a_b_c")  // Arabic letter mark, combining grapheme joiner
        #expect(plain("a\u{E0041}\u{E007F}b") == "a__b")  // tag characters outside a flag
        let wales = "\u{1F3F4}\u{E0067}\u{E0062}\u{E0077}\u{E006C}\u{E0073}\u{E007F}"
        #expect(plain("flag \(wales).txt") == "flag \(wales).txt")  // a subdivision's flag stays
        #expect(plain("\u{1F3F4}\u{E0067}.txt") == "\u{1F3F4}_.txt")  // tags without their cancel tag
        #expect(plain("a\u{85}b") == "a_b")  // C1 control
        #expect(plain("a\u{1}b\u{7F}") == "a_b_")
        #expect(plain([0x61, 0xFF, 0x62]) == "a_b")  // not UTF-8
        #expect(plain([0x61, 0xC0, 0x80, 0x62]) == "a__b")  // overlong NUL
        #expect(plain([0x61, 0xED, 0xA0, 0x80, 0x62]) == "a___b")  // a surrogate
        #expect(plain("") == "Untitled")
        #expect(plain(".") == "Untitled")
        #expect(plain("..") == "Untitled")
        let longest = plain(String(repeating: "x", count: 400) + ".jpg")
        #expect(longest.utf8.count == 255 && longest.hasSuffix(".jpg"))
        let cut = plain(String(repeating: "😀", count: 100) + ".txt")
        #expect(cut.utf8.count <= 255 && cut.hasSuffix(".txt"))
        #expect((cut.utf8.count - 4) % 4 == 0)  // cut between code points
        let numbered = DocumentClones.numbered(String(repeating: "y", count: 255), 3)
        #expect(numbered.utf8.count <= 255 && numbered.hasSuffix(" (3)"))
        #expect(DocumentClones.numbered("known.txt", 2) == "known (2).txt")
        #expect(DocumentClones.numbered(".profile", 1) == ".profile (1)")
        let long = DocumentClones.numbered(String(repeating: "z", count: 260) + ".jpeg", 12)
        #expect(long.utf8.count == 255 && long.hasSuffix("z (12).jpeg"))
        #expect(plain("a" + String(repeating: ".", count: 0) + ".abcdefghijklmno") == "a.abcdefghijklmno")
        let sixteen = plain(String(repeating: "x", count: 300) + ".abcdefghijklmno")  // a 16-byte extension
        #expect(sixteen.utf8.count == 255 && sixteen.hasSuffix(".abcdefghijklmno"))
        let seventeen = plain(String(repeating: "x", count: 300) + ".abcdefghijklmnop")  // none
        #expect(seventeen.utf8.count == 255 && !seventeen.contains("."))
        let dotfile = plain("." + String(repeating: "d", count: 300))
        #expect(dotfile.utf8.count == 255 && dotfile.hasPrefix(".d"))
    }
}
