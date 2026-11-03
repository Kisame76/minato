import CryptoKit
import Darwin
import Foundation
import Synchronization
import Testing

@testable import APKSession

/// ADR-0057's addendum on the iPhone: what an app writes into a clone reaches the user's file, as runtime/tools/
/// pick_host.h's WriteBack does on the Mac (pick_host_test's cases, guest_interface.h's WRITE-BACK contract); the
/// original by path stands in for the bookmark.
@Suite("WriteBack")
final class WriteBackTests {
    let root: URL
    let download: URL
    let source: URL
    let outside: URL
    let state: URL
    /// The clones whose records went, as the host is told.
    final class Names: Sendable { let list = Mutex([String]()) }
    let forgotten = Names()

    init() throws {
        root = FileManager.default.temporaryDirectory.appending(path: "write-back-\(UUID().uuidString)")
        download = root.appending(path: "download")
        source = root.appending(path: "source")
        outside = root.appending(path: "outside")
        state = root.appending(path: "host-state")
        for folder in [download, source, outside, state] {
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        }
        try write(source.appending(path: "known.txt"), "hello\n")
        try write(download.appending(path: "known.txt"), "hello\n")
    }

    deinit { try? FileManager.default.removeItem(at: root) }

    func write(_ url: URL, _ text: String) throws { try Data(text.utf8).write(to: url) }
    func text(_ url: URL) -> String? { try? String(contentsOf: url, encoding: .utf8) }
    var known: String? { text(source.appending(path: "known.txt")) }
    var clone: URL { download.appending(path: "known.txt") }
    func names(_ folder: URL) -> [String] {
        ((try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? []).sorted()
    }
    var copies: [String] { names(state).filter { $0.hasPrefix("copy-") } }
    func fileTimes(_ url: URL) throws -> [timespec] {
        var status = stat()
        guard lstat(url.path, &status) == 0 else { throw POSIXError(.ENOENT) }
        return [status.st_atimespec, status.st_mtimespec]
    }
    func setFileTimes(_ url: URL, _ times: [timespec]) throws {
        guard utimensat(AT_FDCWD, url.path, times, 0) == 0 else { throw POSIXError(.EIO) }
    }

    /// An original that counts what the host does to it.
    final class Counting: Original {
        let inner: PathOriginal
        let counts = Mutex((writes: 0, fail: false, tries: 0))
        init(_ path: String) { inner = PathOriginal(path: path) }
        var token: String { inner.token }
        func version() -> FileVersion? { inner.version() }
        func replace(ifStill expected: FileVersion, from source: Int32, content: FileVersion) -> ReplaceResult {
            if counts.withLock({ counts in
                counts.tries += 1
                return counts.fail
            }) { return .failed }
            let result = inner.replace(ifStill: expected, from: source, content: content)
            if case .written = result { counts.withLock { $0.writes += 1 } }
            return result
        }
        var writes: Int { counts.withLock { $0.writes } }
        var tries: Int { counts.withLock { $0.tries } }
        func failing(_ on: Bool) { counts.withLock { $0.fail = on } }
    }

    /// An original whose replace finds the app writing into the clone meanwhile.
    final class Meddling: Original {
        let inner: PathOriginal
        let clone: URL
        let count = Mutex(0)
        init(_ path: String, clone: URL) {
            inner = PathOriginal(path: path)
            self.clone = clone
        }
        var token: String { inner.token }
        func version() -> FileVersion? { inner.version() }
        func replace(ifStill expected: FileVersion, from source: Int32, content: FileVersion) -> ReplaceResult {
            let n = count.withLock { count in
                count += 1
                return count
            }
            try? Data("still writing \(n)\n".utf8).write(to: clone)
            return inner.replace(ifStill: expected, from: source, content: content)
        }
    }

    /// The open writers of each clone, as apkrt_watch_writers reports them; nil, a clone without a watch.
    final class Writers: Sendable {
        let counts = Mutex([String: Int32]())
        func set(_ clone: String, _ count: Int32?) { counts.withLock { $0[clone] = count } }
        func of(_ clone: String) -> Int32? { counts.withLock { $0[clone] } }
    }

    func writeBack(state folder: URL? = nil, writers: Writers? = nil) -> WriteBack {
        WriteBack(download: download, state: folder ?? state, writers: { writers?.of($0) }) { [forgotten] name in
            forgotten.list.withLock { $0.append(name) }
        }
    }

    /// A write-back that tracks the clone known.txt of source/known.txt.
    func tracked(stateFolder: URL? = nil, writers: Writers? = nil) throws -> (WriteBack, Counting) {
        let original = Counting(source.appending(path: "known.txt").path)
        if let stateFolder {
            try FileManager.default.createDirectory(at: stateFolder, withIntermediateDirectories: true)
        }
        let writeBack = writeBack(state: stateFolder, writers: writers)
        let base = try #require(original.version())
        writeBack.track(clone: "known.txt", original: original, originalNow: base, cloneNow: base)
        return (writeBack, original)
    }

    @Test func anEditedCloneReachesItsOriginalOneSecondAfterTheLastClose() throws {
        let (t, original) = try tracked()
        try write(clone, "edited\n")
        t.closed("known.txt", at: 10000)
        t.flush(at: 10500, force: false)
        #expect(known == "hello\n" && !t.removable("known.txt"))
        t.closed("known.txt", at: 10800)  // the app closes again: the clock restarts
        t.flush(at: 11200, force: false)
        #expect(known == "hello\n")
        t.flush(at: 11800, force: false)
        #expect(known == "edited\n" && original.writes == 1 && t.removable("known.txt"))
        t.flush(at: 20000, force: true)
        #expect(original.writes == 1)  // nothing left to write
        try write(clone, "edited twice\n")  // the same clone, edited again
        t.closed("known.txt", at: 30000)
        t.flush(at: 31000, force: false)
        #expect(known == "edited twice\n" && original.writes == 2)
    }

    @Test func aCloseWithoutAChangeLeavesTheOriginalUntouched() throws {
        let (t, original) = try tracked()
        let before = try FileManager.default.attributesOfItem(atPath: source.appending(path: "known.txt").path)
        t.closed("known.txt", at: 0)
        t.flush(at: 5000, force: false)
        let after = try FileManager.default.attributesOfItem(atPath: source.appending(path: "known.txt").path)
        #expect(original.writes == 0)
        #expect(before[.modificationDate] as? Date == after[.modificationDate] as? Date)
        #expect(t.removable("known.txt"))
    }

    @Test func aWriteThroughAMappingIsFoundAtTheEndOfTheSession() throws {
        let (t, _) = try tracked()
        try FileHandle(forWritingTo: clone).write(contentsOf: Data("HELLO".utf8))  // same size, no close reported
        t.flush(at: 5000, force: false)
        #expect(known == "hello\n")
        t.endSession(at: 6000)
        #expect(known == "HELLO\n" && t.removable("known.txt"))
    }

    /// The scan finds a change that kept the clone's time by its size, and another file renamed over the clone with
    /// the clone's size and time by its identity.
    @Test func aChangeThatKeepsTheClonesTimeIsFoundBySizeOrIdentity() throws {
        let (t, original) = try tracked()
        t.closed("known.txt", at: 0)
        t.flush(at: 5000, force: true)  // nothing changed: the record has the clone's time
        let times = try fileTimes(clone)
        try write(clone, "longer than before\n")
        try setFileTimes(clone, times)
        t.endSession(at: 6000)
        #expect(known == "longer than before\n" && original.writes == 1)

        let (u, again) = try tracked()
        u.closed("known.txt", at: 0)
        u.flush(at: 5000, force: true)
        let other = download.appending(path: "other.tmp")
        try write(other, "longer than ever!!\n")  // the clone's size
        try setFileTimes(other, try fileTimes(clone))
        #expect(rename(other.path, clone.path) == 0)
        u.endSession(at: 6000)
        #expect(u.conflicts == ["known.txt"] && again.writes == 0 && known == "longer than before\n")
    }

    @Test func anOriginalThatChangedMeanwhileIsNotOverwrittenAndTheAppVersionIsKept() throws {
        let (t, original) = try tracked()
        try write(source.appending(path: "known.txt"), "somebody else\n")
        try write(clone, "edited\n")
        t.closed("known.txt", at: 0)
        t.flush(at: 5000, force: false)
        #expect(known == "somebody else\n" && original.writes == 0)
        #expect(!t.removable("known.txt"))
        #expect(t.conflicts == ["known.txt"] && t.replaceable("known.txt"))
        #expect(t.busy("known.txt"))
        try FileManager.default.removeItem(at: clone)  // the guest's cleaning
        #expect(copies.count == 1)  // the host holds the app's version
        #expect(text(try #require(t.appVersion("known.txt"))) == "edited\n")
        #expect(t.resolve("known.txt", replace: true))
        #expect(known == "edited\n" && t.removable("known.txt") && t.conflicts.isEmpty)
        #expect(copies.isEmpty)
    }

    /// the rule in pick_host.h: a name that a record with the app's work holds goes to no new clone, even with
    /// the clone gone; an untouched record whose clone is gone lets the name go, and a new pick under it forgets the
    /// old record, the host told before the new one is made.
    @Test func onlyARecordWithTheAppsWorkHoldsItsName() throws {
        let (t, original) = try tracked()
        #expect(!t.busy("known.txt"))
        t.closed("known.txt", at: 0)
        #expect(t.busy("known.txt"))  // a close not yet taken up
        t.flush(at: 5000, force: false)  // nothing changed: untouched again
        #expect(!t.busy("known.txt"))
        try FileManager.default.removeItem(at: clone)  // the guest's cleaning, before a scan saw it
        try write(outside.appending(path: "other.txt"), "other\n")
        let other = Counting(outside.appending(path: "other.txt").path)
        let otherBase = try #require(other.version())
        try write(clone, "other\n")  // the picker's new clone under the free name
        t.track(clone: "known.txt", original: other, originalNow: otherBase, cloneNow: otherBase)
        #expect(forgotten.list.withLock { $0 } == ["known.txt"])
        try write(clone, "for the other\n")
        t.closed("known.txt", at: 6000)
        t.flush(at: 8000, force: false)
        #expect(text(outside.appending(path: "other.txt")) == "for the other\n" && other.writes == 1)
        #expect(known == "hello\n" && original.writes == 0)
        try write(clone, "edited again\n")
        t.scan(at: 9000)
        try FileManager.default.removeItem(at: clone)
        #expect(t.busy("known.txt"))  // dirty, its clone gone: the edit may still be kept (secure)
    }

    /// guest_interface.h: the user's Replace keeps the clone's identity, so the app's next edit is written back.
    @Test func aReplaceTheUserChoseKeepsTheClonesIdentity() throws {
        let (t, original) = try tracked()
        try write(source.appending(path: "known.txt"), "somebody else\n")
        try write(clone, "edited\n")
        t.closed("known.txt", at: 0)
        t.flush(at: 5000, force: false)
        #expect(t.resolve("known.txt", replace: true))
        try write(clone, "edited again\n")
        t.closed("known.txt", at: 6000)
        t.flush(at: 8000, force: false)
        #expect(known == "edited again\n" && original.writes == 2 && t.conflicts.isEmpty)
    }

    /// A conflict the user discards keeps the original and is forgotten; a later file of that name is the app's own.
    @Test func aConflictTheUserDiscardsIsForgottenAndALaterFileOfThatNameIsTheAppsOwn() throws {
        let (t, _) = try tracked()
        try write(source.appending(path: "known.txt"), "somebody else\n")
        try write(clone, "edited\n")
        t.closed("known.txt", at: 0)
        t.flush(at: 5000, force: false)
        #expect(t.resolve("known.txt", replace: false))
        #expect(known == "somebody else\n" && t.removable("known.txt") && copies.isEmpty)
        #expect(!t.tracked("known.txt") && forgotten.list.withLock { $0 } == ["known.txt"])
        #expect(!t.resolve("known.txt", replace: true))  // no conflict any more
        try FileManager.default.removeItem(at: clone)  // the guest's cleaning
        try write(clone, "the app's own export\n")
        t.closed("known.txt", at: 6000)
        t.endSession(at: 9000)
        #expect(known == "somebody else\n")
    }

    @Test func anOriginalThatIsGoneOrBecameALinkIsAConflictAndNothingIsWrittenThrough() throws {
        let (t, original) = try tracked()
        try write(outside.appending(path: "target"), "outside\n")
        try FileManager.default.removeItem(at: source.appending(path: "known.txt"))
        try FileManager.default.createSymbolicLink(at: source.appending(path: "known.txt"),
                                                   withDestinationURL: outside.appending(path: "target"))
        try write(clone, "edited\n")
        t.closed("known.txt", at: 0)
        t.flush(at: 5000, force: false)
        #expect(text(outside.appending(path: "target")) == "outside\n" && original.writes == 0)
        #expect(t.conflicts == ["known.txt"])
        // Gone: nothing is made where it was.
        try FileManager.default.removeItem(at: source.appending(path: "known.txt"))
        try write(source.appending(path: "known.txt"), "hello\n")
        try FileManager.default.removeItem(at: clone)
        try write(clone, "hello\n")
        let (gone, _) = try tracked(stateFolder: root.appending(path: "gone-state"))
        try FileManager.default.removeItem(at: source.appending(path: "known.txt"))
        try write(clone, "edited\n")
        gone.closed("known.txt", at: 0)
        gone.flush(at: 5000, force: false)
        #expect(known == nil && !gone.removable("known.txt") && gone.conflicts == ["known.txt"])
    }

    /// The rule on the review's point 7: a record goes only when its clone is missing at two looks in a row, or
    /// at the session's end.
    @Test func aCleanCloneSeenMissingIsForgottenBeforeTheAppMakesAnotherFileOfThatName() throws {
        let (t, _) = try tracked()
        try FileManager.default.removeItem(at: clone)
        t.scan(at: 1000)
        #expect(t.tracked("known.txt"))  // missing once
        t.scan(at: 1250)
        #expect(!t.tracked("known.txt") && forgotten.list.withLock { $0 } == ["known.txt"])
        try write(clone, "export.csv under a name the user once picked\n")
        t.closed("known.txt", at: 2000)
        t.endSession(at: 5000)
        #expect(known == "hello\n")
    }

    @Test func aDirtyCloneThatIsGoneWhenItIsWrittenBackIsForgotten() throws {
        let (t, _) = try tracked()
        try write(clone, "edited\n")
        t.closed("known.txt", at: 0)
        try FileManager.default.removeItem(at: clone)
        t.flush(at: 5000, force: false)
        #expect(t.tracked("known.txt") && forgotten.list.withLock { $0 }.isEmpty)  // missing once
        t.flush(at: 5999, force: false)
        #expect(t.tracked("known.txt"))  // looked at again a second later
        t.flush(at: 6000, force: false)
        #expect(!t.tracked("known.txt") && forgotten.list.withLock { $0 } == ["known.txt"])
        #expect(known == "hello\n" && t.conflicts.isEmpty)
    }

    /// Two looks in a row: a clone that is back (renamed away and back, the same file) starts the count again, at a
    /// scan and at a flush alike.
    @Test func aCloneThatIsBackStartsTheCountAgain() throws {
        let (t, _) = try tracked()
        let away = download.appending(path: "away.txt")
        #expect(rename(clone.path, away.path) == 0)
        t.scan(at: 1000)
        #expect(rename(away.path, clone.path) == 0)
        t.scan(at: 1250)
        #expect(rename(clone.path, away.path) == 0)
        t.scan(at: 1500)
        #expect(t.tracked("known.txt"))  // missing once since it was back
        #expect(rename(away.path, clone.path) == 0)
        try write(clone, "edited\n")
        t.closed("known.txt", at: 2000)
        #expect(rename(clone.path, away.path) == 0)
        t.flush(at: 3000, force: false)  // missing once
        #expect(rename(away.path, clone.path) == 0)
        t.flush(at: 4000, force: false)  // back: written
        #expect(known == "edited\n")
        #expect(rename(clone.path, away.path) == 0)
        t.scan(at: 4250)
        #expect(t.tracked("known.txt"))
        #expect(rename(away.path, clone.path) == 0)
        try write(clone, "edited twice\n")
        t.closed("known.txt", at: 5000)
        #expect(rename(clone.path, away.path) == 0)
        t.flush(at: 6000, force: false)
        #expect(t.tracked("known.txt"))  // missing once since it was back at the last write-back
    }

    /// The review's point 7: an app that saves by deleting the clone and renaming a file to its name has the file
    /// back by the next flush; another file there is a conflict, never a silent loss of the save.
    @Test func aSaveByDeletingTheCloneAndRenamingAFileIsAConflictNotALoss() throws {
        let (t, original) = try tracked()
        try write(clone, "edited\n")
        t.closed("known.txt", at: 0)
        try write(download.appending(path: "known.tmp"), "saved by rename\n")
        try FileManager.default.removeItem(at: clone)
        t.flush(at: 5000, force: false)  // between the delete and the rename
        #expect(rename(download.appending(path: "known.tmp").path, clone.path) == 0)
        t.closed("known.txt", at: 5100)
        t.flush(at: 7000, force: false)
        #expect(t.conflicts == ["known.txt"] && original.writes == 0 && known == "hello\n")
        #expect(text(try #require(t.appVersion("known.txt"))) == "saved by rename\n")
    }

    /// At the session's end a clone missing once is gone: no later flush comes.
    @Test func aCloneMissingAtTheSessionsEndIsForgottenAtOnce() throws {
        let (t, _) = try tracked()
        try write(clone, "edited\n")
        t.closed("known.txt", at: 0)
        try FileManager.default.removeItem(at: clone)
        t.endSession(at: 5000)
        #expect(!t.tracked("known.txt") && known == "hello\n")
        try write(clone, "hello\n")
        let (u, _) = try tracked(stateFolder: root.appending(path: "clean-state"))
        try FileManager.default.removeItem(at: clone)
        u.endSession(at: 5000)
        #expect(!u.tracked("known.txt"))
    }

    /// The addendum's point 4: the app's version of a clone not yet written back goes to the host's folder before
    /// the guest starts, whose cleaning may delete the clone; the write-back then takes it from there.
    @Test func aCloneSecuredBeforeTheGuestStartsIsWrittenBackAfterTheGuestDeletedIt() throws {
        let (t, original) = try tracked()
        try write(clone, "edited before the end\n")
        t.closed("known.txt", at: 0)  // the host ended before the write-back
        t.secure(at: 100)
        #expect(copies.count == 1)
        try FileManager.default.removeItem(at: clone)  // the guest's cleaning
        t.flush(at: 5000, force: false)
        #expect(known == "hello\n")  // missing once
        t.flush(at: 6000, force: false)
        #expect(known == "edited before the end\n" && original.writes == 1)
        #expect(!t.tracked("known.txt") && copies.isEmpty)
    }

    /// A file renamed over the clone is not the clone: it is not kept as the app's version before the start, and
    /// nothing of it reaches the user's file once the guest deleted it.
    @Test func aFileThatIsNotTheCloneIsNotSecured() throws {
        let (t, original) = try tracked()
        try write(clone, "edited\n")
        t.closed("known.txt", at: 0)
        try write(download.appending(path: "known.tmp"), "renamed over\n")
        #expect(rename(download.appending(path: "known.tmp").path, clone.path) == 0)
        t.secure(at: 100)
        #expect(copies.isEmpty)
        try FileManager.default.removeItem(at: clone)
        t.flush(at: 5000, force: true)
        #expect(known == "hello\n" && original.writes == 0)
    }

    @Test func aSecuredCloneThatIsStillThereIsWrittenFromTheCloneAndTheCopyGoes() throws {
        let (t, _) = try tracked()
        try write(clone, "edited\n")
        t.closed("known.txt", at: 0)
        t.secure(at: 100)
        try FileHandle(forWritingTo: clone).write(contentsOf: Data("EDITED".utf8))
        t.closed("known.txt", at: 200)  // the app's close after that write
        t.flush(at: 5000, force: true)
        #expect(known == "EDITED\n" && t.tracked("known.txt") && copies.isEmpty)
    }

    @Test func aFileThatIsNotTheCloneAnyMoreIsNeverWrittenBackEvenAfterARestart() throws {
        let (t, _) = try tracked()
        try write(clone, "edited\n")
        t.closed("known.txt", at: 0)
        #expect(t.save())  // dirty, stored, the host is killed
        try FileManager.default.removeItem(at: clone)  // the app made another file of that name
        try write(clone, "another file\n")
        let again = writeBack()
        #expect(again.load { Counting($0) })
        again.endSession(at: 5000)
        #expect(known == "hello\n")
        #expect(again.conflicts == ["known.txt"])  // its version is kept for the user
        #expect(copies.count == 1)
        #expect(text(try #require(again.appVersion("known.txt"))) == "another file\n")
    }

    @Test func aCloneTheAppReplacedByALinkOrAFIFOIsNeverReadThrough() throws {
        let (t, original) = try tracked()
        try write(outside.appending(path: "secret"), "secret\n")
        try FileManager.default.removeItem(at: clone)
        try FileManager.default.createSymbolicLink(at: clone, withDestinationURL: outside.appending(path: "secret"))
        t.closed("known.txt", at: 0)
        t.endSession(at: 5000)
        #expect(known == "hello\n" && original.writes == 0 && !t.removable("known.txt"))
        #expect(t.conflicts == ["known.txt"] && t.appVersion("known.txt") == nil && !t.replaceable("known.txt"))
        try FileManager.default.removeItem(at: clone)
        try write(clone, "hello\n")
        let (fifo, _) = try tracked(stateFolder: root.appending(path: "fifo-state"))
        try FileManager.default.removeItem(at: clone)
        #expect(mkfifo(clone.path, 0o600) == 0)
        fifo.closed("known.txt", at: 0)
        fifo.endSession(at: 5000)  // returns
        #expect(known == "hello\n" && !fifo.removable("known.txt"))
    }

    @Test func anAppThatSavesByRenamingATemporaryFileOverTheCloneIsAConflictNotASilentWrite() throws {
        let (t, original) = try tracked()
        try write(download.appending(path: "known.tmp"), "renamed over\n")
        #expect(rename(download.appending(path: "known.tmp").path, clone.path) == 0)
        t.endSession(at: 5000)  // no close of the clone's identity came
        #expect(known == "hello\n" && original.writes == 0)  // another file now: the user decides
        #expect(t.conflicts == ["known.txt"])
        #expect(t.resolve("known.txt", replace: true))  // the user's choice
        #expect(known == "renamed over\n")
    }

    /// A failed write is tried again after 1, 2, 4, 8 and 16 s; the sixth failure is the user's to decide.
    @Test func aWriteThatFailsIsTriedAgainLaterAndAfterSixTheUserDecides() throws {
        let (t, original) = try tracked()
        try write(clone, "edited\n")
        original.failing(true)
        t.closed("known.txt", at: 0)
        t.flush(at: 1000, force: false)  // 1
        #expect(known == "hello\n" && !t.removable("known.txt") && t.conflicts.isEmpty)
        original.failing(false)
        t.flush(at: 1999, force: false)
        #expect(known == "hello\n")
        t.flush(at: 2000, force: false)
        #expect(known == "edited\n" && t.removable("known.txt"))
        try write(clone, "edited again\n")
        original.failing(true)
        t.closed("known.txt", at: 10000)
        var now: Int64 = 11000
        for wait: Int64 in [1000, 2000, 4000, 8000] {
            let tries = original.tries
            t.flush(at: now, force: false)
            #expect(t.conflicts.isEmpty && original.tries == tries + 1)
            t.flush(at: now + wait - 1, force: false)  // not yet
            #expect(original.tries == tries + 1)
            now += wait
        }
        t.flush(at: now, force: false)  // 5
        #expect(t.conflicts.isEmpty)
        t.flush(at: now + 16000, force: false)  // 6
        #expect(t.conflicts == ["known.txt"] && copies.count == 1 && known == "edited\n")
    }

    /// A due time far ahead is a clock set back: it is due at once.
    @Test func aDueTimeFarAheadIsDueAtOnce() throws {
        let (t, _) = try tracked()
        try write(clone, "edited\n")
        t.closed("known.txt", at: 1_000_000_000)
        t.flush(at: 1000, force: false)
        #expect(known == "edited\n")
    }

    /// A replace that happened before the host ended without storing it is no conflict.
    @Test func aReplaceThatHappenedBeforeTheHostEndedIsNoConflict() throws {
        let (t, original) = try tracked()
        try write(clone, "edited\n")
        t.closed("known.txt", at: 0)
        #expect(t.save())
        try write(source.appending(path: "known.txt"), "edited\n")  // written, then killed
        t.flush(at: 5000, force: false)
        #expect(t.conflicts.isEmpty && t.removable("known.txt") && original.writes == 1)
    }

    /// The review, point 1: a clone the app writes in bursts through an open descriptor is written back only
    /// after its last writer closed, never a snapshot in between, not even at the background's flush; the change goes
    /// on being looked at while it lasts.
    @Test func aCloneWrittenInBurstsIsWrittenOnlyAfterItsLastWriterClosed() throws {
        let (t, original) = try tracked()
        try write(clone, "part one\n")  // a writer open: no close yet
        t.scan(at: 0)
        t.flush(at: 2000, force: false)
        #expect(known == "hello\n")
        try write(clone, "part one, part two\n")
        t.scan(at: 2250)
        t.flush(at: 5000, force: true)  // the background
        #expect(known == "hello\n" && original.writes == 0 && !t.removable("known.txt"))
        t.closed("known.txt", at: 6000)
        t.flush(at: 7000, force: false)
        #expect(known == "part one, part two\n" && original.writes == 1)
    }

    /// A change after the close was taken up waits for the next close or the session's end, also when no scan saw
    /// it before the write-back looked.
    @Test func aChangeAfterTheCloseWaitsForTheNextCloseOrTheEnd() throws {
        let (t, original) = try tracked()
        try write(clone, "saved\n")
        t.closed("known.txt", at: 0)
        t.scan(at: 50)  // the close taken up
        try write(clone, "saved, then more\n")  // opened again
        t.flush(at: 1500, force: false)
        #expect(known == "hello\n")
        t.scan(at: 1750)
        t.flush(at: 4000, force: true)
        #expect(known == "hello\n")
        t.endSession(at: 5000)  // the guest is gone: no writer left
        #expect(known == "saved, then more\n" && original.writes == 1)
    }

    /// A copy that fails because the app wrote meanwhile is no failed write: a writer that never pauses for long
    /// gets no conflict.
    @Test func aCopyTheAppWroteIntoMeanwhileIsNoFailure() throws {
        let original = Meddling(source.appending(path: "known.txt").path, clone: clone)
        let t = writeBack()
        let base = try #require(original.version())
        t.track(clone: "known.txt", original: original, originalNow: base, cloneNow: base)
        try write(clone, "edited\n")
        t.closed("known.txt", at: 0)
        var now: Int64 = 1000
        for _ in 0..<8 {
            t.flush(at: now, force: false)  // the app goes on writing: no close
            now += 20000
        }
        #expect(t.conflicts.isEmpty && known == "hello\n" && original.count.withLock { $0 } == 1)
    }

    /// The second pass, point 2: a change with no close after it (a mapping, a truncate by path) is settled
    /// once the runtime reports no open writer and the clone stood still across a tick; without a watch it waits for
    /// a close or the end, as before.
    @Test func aChangeWithNoCloseIsWrittenOnceNoWriterIsOpenAndItStoodStill() throws {
        let writers = Writers()
        writers.set("known.txt", 0)
        let (t, original) = try tracked(writers: writers)
        try FileHandle(forWritingTo: clone).write(contentsOf: Data("HELLO".utf8))  // a mapping's write
        t.scan(at: 0)
        t.flush(at: 1000, force: false)
        #expect(known == "hello\n")  // seen changing
        t.scan(at: 250)  // still, no writer open
        t.flush(at: 2000, force: false)
        #expect(known == "HELLO\n" && original.writes == 1)

        writers.set("known.txt", nil)  // no watch: the close rule
        try FileHandle(forWritingTo: clone).write(contentsOf: Data("hello".utf8))
        t.scan(at: 3000)
        t.scan(at: 3250)
        t.flush(at: 6000, force: true)
        #expect(known == "HELLO\n")
    }

    /// The second pass, point 3: no copy while a writer is open, whatever the close and the background say.
    @Test func noCopyIsMadeWhileAWriterIsOpen() throws {
        let writers = Writers()
        writers.set("known.txt", 1)
        let (t, original) = try tracked(writers: writers)
        try write(clone, "saved\n")
        t.closed("known.txt", at: 0)  // and opened again since
        t.flush(at: 1000, force: false)
        t.flush(at: 2000, force: true)  // the background
        t.scan(at: 2250)
        t.scan(at: 2500)
        t.flush(at: 4000, force: false)
        #expect(known == "hello\n" && original.writes == 0)
        writers.set("known.txt", 0)
        t.scan(at: 4250)
        t.flush(at: 6000, force: false)
        #expect(known == "saved\n" && original.writes == 1)
    }

    /// The second pass, point 1: after Replace, what the app wrote and closed while the question was open is
    /// written back too, not left waiting for a close that came before (here without a watch's count, which would
    /// settle it by itself).
    @Test func whatTheAppWroteDuringTheQuestionIsWrittenAfterReplace() throws {
        let (t, _) = try tracked()
        try write(source.appending(path: "known.txt"), "somebody else\n")
        try write(clone, "edited\n")
        t.closed("known.txt", at: 0)
        t.flush(at: 1000, force: false)
        #expect(t.conflicts == ["known.txt"])
        try write(clone, "edited during the question\n")
        t.closed("known.txt", at: 1500)  // a close while in conflict
        t.scan(at: 1600)  // taken up by the host's tick before the user answers
        #expect(t.resolve("known.txt", replace: true))
        #expect(known == "edited\n")
        t.scan(at: 2000)
        t.scan(at: 2250)
        t.flush(at: 4000, force: false)
        #expect(known == "edited during the question\n")
    }

    /// The sitting's finding: an original the user deleted is in the trash, where its bookmark still leads; it is gone
    /// for the write-back (a conflict), found by the volume's trash folder or a ".Trash" component.
    @Test func anOriginalInTheTrashIsGone() throws {
        let trash = root.appending(path: "Recently Deleted")  // a volume's trash under a name of its own
        try FileManager.default.createDirectory(at: trash.appending(path: "folder"), withIntermediateDirectories: true)
        #expect(WriteBack.inTrash(trash.appending(path: "a.txt"), trash: trash))
        #expect(WriteBack.inTrash(trash.appending(path: "folder/a.txt"), trash: trash))
        #expect(WriteBack.inTrash(root.appending(path: "x/.Trash/a.txt"), trash: nil))
        #expect(!WriteBack.inTrash(source.appending(path: "known.txt"), trash: trash))
        #expect(!WriteBack.inTrash(root.appending(path: ".Trashed/a.txt"), trash: nil))
    }

    /// The review, point 8: a scan's look and a write-back's look are counted apart; a clone a scan saw missing
    /// once and that the app closed then is not forgotten at the write-back's first look.
    @Test func aScansLookAndAWriteBacksLookAreCountedApart() throws {
        let (t, _) = try tracked()
        try FileManager.default.removeItem(at: clone)
        t.scan(at: 0)  // missing once
        t.closed("known.txt", at: 100)
        t.flush(at: 1100, force: false)
        #expect(t.tracked("known.txt"))
        t.flush(at: 2100, force: false)
        #expect(!t.tracked("known.txt"))
    }

    /// The review, point 5: before the start the app's version is kept for a dirty clone over the limit and a
    /// conflict without a copy too, by a clone of the file, nothing hashed.
    @Test func aCloneOverTheLimitAndAConflictWithoutACopyAreKeptBeforeTheStart() throws {
        let (t, _) = try tracked()
        let handle = try FileHandle(forWritingTo: clone)
        try handle.truncate(atOffset: FileVersion.limit + 1)
        try handle.close()
        t.closed("known.txt", at: 0)
        #expect(t.save())
        let again = writeBack()
        #expect(again.load { Counting($0) })
        let clock = ContinuousClock()
        let start = clock.now
        again.secure(at: 100)
        #expect(clock.now - start < .milliseconds(250))
        #expect(copies.count == 1)
        // A conflict stored without a copy (the host ended before it kept one).
        try FileManager.default.removeItem(at: state.appending(path: try #require(copies.first)))
        let records = state.appending(path: "records.json")
        var stored = try #require(try JSONSerialization.jsonObject(with: Data(contentsOf: records)) as? [[String: Any]])
        stored[0]["state"] = WriteBack.State.conflict.rawValue
        stored[0]["copy"] = ""
        try JSONSerialization.data(withJSONObject: stored).write(to: records)
        let third = writeBack()
        #expect(third.load { Counting($0) })
        third.secure(at: 200)
        #expect(copies.count == 1 && third.appVersion("known.txt") != nil)
    }

    /// The review, point 3: the original is read by its own path, never its folder, which a security scope of
    /// one picked file does not cover.
    @Test func anOriginalWhoseFolderCannotBeReadIsReadAll() throws {
        let path = source.appending(path: "known.txt").path
        #expect(chmod(source.path, 0o100) == 0)  // passable, not readable
        defer { chmod(source.path, 0o755) }
        #expect(FileVersion.read(path: path)?.size == 6)
    }

    /// The second review: a copy takes exactly the version it was given, hashed as it goes, however the app changes
    /// the file meanwhile (a sparse growth copies no more, another content of that size nothing), and never replaces a
    /// file of the name it makes.
    @Test func aCopyTakesExactlyTheVersionItWasGiven() throws {
        let version = try #require(FileVersion.read(path: clone.path))
        let file = Darwin.open(clone.path, O_RDWR | O_CLOEXEC)
        let into = Darwin.open(outside.path, O_RDONLY | O_DIRECTORY | O_CLOEXEC)
        defer { close(file); close(into) }
        try #require(file >= 0 && into >= 0)
        #expect(ftruncate(file, off_t(FileVersion.limit + 1)) == 0)  // grown after its version was taken
        #expect(FileVersion.copyWhole(from: file, to: "grown", in: into, mode: 0o600, content: version))
        #expect(text(outside.appending(path: "grown")) == "hello\n")
        #expect(ftruncate(file, 0) == 0 && pwrite(file, "HELLO\n", 6, 0) == 6)  // the same size, another content
        #expect(!FileVersion.copyWhole(from: file, to: "other", in: into, mode: 0o600, content: version))
        #expect(ftruncate(file, 0) == 0 && pwrite(file, "hello\n", 6, 0) == 6)
        try write(outside.appending(path: "grown"), "kept\n")
        #expect(!FileVersion.copyWhole(from: file, to: "grown", in: into, mode: 0o600, content: version))
        #expect(text(outside.appending(path: "grown")) == "kept\n" && names(outside) == ["grown"])
    }

    /// The second review: the app's version kept is always its latest, never the first kept (a save after the copy
    /// before the sixth failure, a record a session left dirty with its copy).
    @Test func theKeptVersionIsTheAppsLatest() throws {
        let (t, original) = try tracked()
        try write(clone, "first\n")
        t.closed("known.txt", at: 0)
        t.secure(at: 100)
        original.failing(true)
        try write(clone, "second\n")
        t.closed("known.txt", at: 200)
        var now: Int64 = 1200
        for _ in 0..<6 {
            t.flush(at: now, force: true)
            now += 20000
        }
        #expect(t.conflicts == ["known.txt"] && copies.count == 1)
        #expect(text(try #require(t.appVersion("known.txt"))) == "second\n")
    }

    @Test func aRecordLeftDirtyWithItsCopyIsSecuredAgain() throws {
        let (t, original) = try tracked()
        try write(clone, "first\n")
        t.closed("known.txt", at: 0)
        t.secure(at: 100)  // the session's start
        try write(clone, "second\n")
        t.closed("known.txt", at: 200)
        #expect(t.save())  // killed before the write-back
        let again = writeBack()
        #expect(again.load { Counting($0) })
        again.secure(at: 300)  // the next start
        #expect(copies.count == 1)
        try FileManager.default.removeItem(at: clone)  // the guest's cleaning
        again.flush(at: 5000, force: true)
        again.flush(at: 6000, force: true)  // missing at two flushes in a row
        #expect(known == "second\n" && original.writes == 0)  // written by the new record's original
    }

    /// The second review: an error that may pass (Download out of reach for a moment, no descriptor left) is tried
    /// again, never a conflict at once.
    @Test func aFolderOutOfReachForAMomentIsTriedAgain() throws {
        let (t, _) = try tracked()
        try write(clone, "edited\n")
        t.closed("known.txt", at: 0)
        #expect(chmod(download.path, 0) == 0)
        defer { chmod(download.path, 0o755) }
        t.flush(at: 1000, force: false)
        #expect(chmod(download.path, 0o755) == 0)
        #expect(t.conflicts.isEmpty && known == "hello\n" && !t.removable("known.txt"))
        t.flush(at: 2000, force: false)
        #expect(known == "edited\n")
        try write(clone, "edited again\n")
        t.closed("known.txt", at: 3000)
        #expect(chmod(clone.path, 0) == 0)  // the clone itself out of reach for a moment
        defer { chmod(clone.path, 0o644) }
        t.flush(at: 4000, force: false)
        #expect(chmod(clone.path, 0o644) == 0)
        #expect(t.conflicts.isEmpty && known == "edited\n")
        t.flush(at: 5000, force: false)
        #expect(known == "edited again\n")
    }

    /// B3 of the review: no file is read whole into memory, and none over the limit is read at all; a clone the app
    /// made huge (a sparse truncate) is a conflict at once, its version the clone itself.
    @Test func aCloneOverTheLimitIsNeverReadAndTheUserDecides() throws {
        let (t, original) = try tracked()
        let handle = try FileHandle(forWritingTo: clone)
        try handle.truncate(atOffset: FileVersion.limit + 1)
        try handle.close()
        t.closed("known.txt", at: 0)
        let clock = ContinuousClock()
        let start = clock.now
        t.flush(at: 5000, force: false)
        #expect(clock.now - start < .milliseconds(250))  // not a byte hashed (1 GiB takes about a second)
        #expect(t.conflicts == ["known.txt"] && original.writes == 0 && copies.count == 1)  // a clone of the file
        try FileManager.default.removeItem(at: clone)  // the guest's cleaning
        let kept = try #require(t.appVersion("known.txt"))
        #expect(kept != clone && kept.deletingLastPathComponent().lastPathComponent == state.lastPathComponent)
        #expect(!t.replaceable("known.txt"))  // never written over the user's file
    }

    @Test func aVersionIsHashedInStepsAsAWhole() throws {
        let big = Data((0..<300_000).map { UInt8(truncatingIfNeeded: $0 &* 7) })
        try big.write(to: source.appending(path: "big.bin"))
        let version = try #require(FileVersion.read(path: source.appending(path: "big.bin").path))
        #expect(version.size == 300_000)
        #expect(version.digest == SHA256.hash(data: big).map { String(format: "%02x", $0) }.joined())
        #expect(FileVersion.read(path: root.appending(path: "nothing").path) == nil)
    }

    @Test func recordsSurviveTheHostsRestartAndAreNotTakenFromTheAppsFolders() throws {
        let (t, _) = try tracked()
        try write(clone, "edited\n")
        t.closed("known.txt", at: 0)
        #expect(t.save())  // the host's queue saves after a close; then it is killed before any flush
        let again = writeBack()
        #expect(again.load { Counting($0) })
        #expect(again.tracked("known.txt"))
        again.endSession(at: 5000)
        #expect(known == "edited\n" && again.removable("known.txt"))
        // A records file planted in the app's folders is not read.
        try write(download.appending(path: "records.json"), "{}")
        let other = writeBack(state: root.appending(path: "empty-state"))
        #expect(!other.load { Counting($0) } || !other.tracked("known.txt"))
    }

    @Test func aRecordsFileThatIsNotTheHostsIsIgnored() throws {
        try write(state.appending(path: "records.json"), "not json")
        let t = writeBack()
        #expect(!t.load { Counting($0) })
        #expect(text(state.appending(path: "records.json.unreadable")) == "not json")  // not overwritten unread
        try write(state.appending(path: "records.json"), "not json either")
        #expect(!t.load { Counting($0) })
        #expect(text(state.appending(path: "records.json.unreadable-1")) == "not json either")
        try write(state.appending(path: "copy-77"), "a copy no record names\n")
        try write(state.appending(path: "copy-78.partial"), "a copy's leftover\n")
        func entry(_ clone: String, state: Int = 1, digest: String = "", copy: String = "", failures: Int = 0)
            -> String {
            #"{"clone":"\#(clone)","state":\#(state),"token":"t","copy":"\#(copy)","dueMs":0,"#
                + #""failures":\#(failures),"#
                + #""originalBase":{"exists":true,"size":1,"mtimeNs":0,"dev":0,"ino":0,"birthNs":0,"#
                + #""digest":"\#(digest)"},"#
                + #""cloneBase":{"exists":true,"size":1,"mtimeNs":0,"dev":0,"ino":0,"birthNs":0,"digest":""}}"#
        }
        try write(state.appending(path: "records.json"),
                  "[" + [entry("../x"), entry("bad-state", state: 3), entry("bad-digest", digest: "abc"),
                         entry("bad-copy", copy: "../y"), entry("bad-failures", failures: Int.max),
                         entry("good")].joined(separator: ",") + "]")
        #expect(t.load { Counting($0) })
        #expect(t.clones == ["good"])
        #expect(!names(state).contains("copy-77") && !names(state).contains("copy-78.partial"))
    }

    /// The clone-names rule: a clone the app has not touched is used again only while the original has its content,
    /// and the original is read by the caller (under the pick's coordination), never while the records are held.
    @Test func aCleanCloneIsReusableOnlyWhileTheOriginalIsUnchanged() throws {
        let (t, original) = try tracked()
        #expect(t.reusable("known.txt") { original.version() })
        try write(source.appending(path: "known.txt"), "changed elsewhere\n")
        #expect(!t.reusable("known.txt") { original.version() })
        #expect(t.reusable("known.txt") { nil })  // cannot be read: the clone stays
        try write(clone, "app edit\n")
        t.closed("known.txt", at: 1000)
        var asked = false
        #expect(t.reusable("known.txt") {
            asked = true
            return original.version()
        })  // the app's edits stay
        #expect(!asked)
        t.scan(at: 1000)  // the close taken up: dirty
        #expect(t.reusable("known.txt") {
            asked = true
            return original.version()
        })
        #expect(!asked)
        #expect(!t.reusable("untracked.txt") { original.version() })  // no record: the app's own file
    }

    /// B1 of the review: neither the picker's questions nor a new record wait for a write-back under way, which may
    /// wait in a coordinated write for the picker's own coordinated read; nor does the guest's close.
    @Test func thePickerAndTheGuestNeverWaitForAWriteBack() throws {
        let slow = Slow(source.appending(path: "known.txt").path)
        let t = writeBack()
        let base = try #require(slow.version())
        t.track(clone: "known.txt", original: slow, originalNow: base, cloneNow: base)
        try write(clone, "edited\n")
        t.closed("known.txt", at: 0)
        let done = DispatchSemaphore(value: 0)
        Thread.detachNewThread {
            t.flush(at: 5000, force: true)  // the original takes 300 ms
            done.signal()
        }
        try #require(slow.entered.wait(timeout: .now() + 10) == .success)  // the write-back reached the original
        let clock = ContinuousClock()
        let start = clock.now
        t.closed("known.txt", at: 5100)  // the guest's close, during the write
        t.scan(at: 5100)  // and taken up before the write ends
        _ = t.reusable("known.txt") { nil }
        _ = t.busy("other.txt")
        try write(download.appending(path: "other.txt"), "other\n")
        t.track(clone: "other.txt", original: Counting(source.appending(path: "known.txt").path), originalNow: base,
                cloneNow: base)
        _ = t.conflicts
        #expect(clock.now - start < .milliseconds(150))
        try #require(done.wait(timeout: .now() + 10) == .success)
        // The close during the write leaves the clone due again.
        #expect(!t.removable("known.txt"))
    }

    /// A write-back's outcome lands only on the record it began with: one made anew meanwhile keeps its own original.
    @Test func anOutcomeNeverLandsOnARecordMadeMeanwhile() throws {
        let slow = Slow(source.appending(path: "known.txt").path)
        let t = writeBack()
        let base = try #require(slow.version())
        t.track(clone: "known.txt", original: slow, originalNow: base, cloneNow: base)
        try write(clone, "edited\n")
        t.closed("known.txt", at: 0)
        let done = DispatchSemaphore(value: 0)
        Thread.detachNewThread {
            t.flush(at: 5000, force: true)
            done.signal()
        }
        try #require(slow.entered.wait(timeout: .now() + 10) == .success)  // the write-back reached the original
        try write(outside.appending(path: "other.txt"), "other\n")
        let other = Counting(outside.appending(path: "other.txt").path)
        let otherBase = try #require(other.version())
        t.track(clone: "known.txt", original: other, originalNow: otherBase, cloneNow: otherBase)
        try #require(done.wait(timeout: .now() + 10) == .success)
        try write(clone, "for the other\n")
        t.closed("known.txt", at: 6000)
        t.flush(at: 8000, force: false)
        #expect(text(outside.appending(path: "other.txt")) == "for the other\n" && other.writes == 1)
    }

    final class Slow: Original {
        let inner: PathOriginal
        let entered = DispatchSemaphore(value: 0)
        init(_ path: String) { inner = PathOriginal(path: path) }
        var token: String { inner.token }
        func version() -> FileVersion? { inner.version() }
        func replace(ifStill expected: FileVersion, from source: Int32, content: FileVersion) -> ReplaceResult {
            entered.signal()
            usleep(300_000)
            return inner.replace(ifStill: expected, from: source, content: content)
        }
    }
}
