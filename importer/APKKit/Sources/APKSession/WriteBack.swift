import CryptoKit
import Darwin
import Foundation
import Synchronization

/// ADR-0057's addendum on the iPhone: what an app writes into a clone of a user's file reaches that file, as Android's
/// app writes the document through its grant. runtime/tools/pick_host.h's WriteBack is the reference, and
/// guest_interface.h's WRITE-BACK paragraph the contract: a record per clone in the host's own folder, the write one
/// second after the app's last close, at the background and at the end, never over an original that is no longer what
/// the clone was made from and never from a file that is no longer the clone (a conflict: the app's version is kept
/// in the host's folder and the user decides), and no record that outlives its clone.

/// A file's state as the write-back compares it: its content (size and SHA-256) and its identity (device, inode and
/// birth time), which a file made anew under the same name does not share.
public struct FileVersion: Codable, Equatable, Sendable {
    public var exists: Bool
    public var size: UInt64
    public var mtimeNs: Int64
    public var dev: UInt64
    public var ino: UInt64
    public var birthNs: Int64
    public var digest: String

    public init(exists: Bool, size: UInt64, mtimeNs: Int64, dev: UInt64 = 0, ino: UInt64 = 0, birthNs: Int64 = 0,
                digest: String) {
        self.exists = exists
        self.size = size
        self.mtimeNs = mtimeNs
        self.dev = dev
        self.ino = ino
        self.birthNs = birthNs
        self.digest = digest
    }

    /// The most bytes the write-back reads of one file, as the export's limit: a file over it is never hashed or
    /// copied by the host, and the user decides.
    public static let limit: UInt64 = 1 << 30

    /// The same content; a changed time alone is no change.
    public func sameContent(_ other: FileVersion) -> Bool {
        exists == other.exists && size == other.size && digest == other.digest
    }

    /// The same file: one made anew under the name, or renamed over it, is not.
    public func sameFile(_ other: FileVersion) -> Bool {
        dev == other.dev && ino == other.ino && birthNs == other.birthNs
    }

    /// The version of the open file `file` with its status, the content read from offset 0 in steps of 64 KiB.
    static func of(_ file: Int32) -> Result<FileVersion, Errno> {
        var status = stat()
        guard fstat(file, &status) == 0 else { return .failure(Errno(errno)) }
        guard status.st_mode & S_IFMT == S_IFREG else { return .failure(Errno(EINVAL)) }
        guard UInt64(status.st_size) <= limit else { return .failure(Errno(EFBIG)) }
        var hash = SHA256()
        var buffer = [UInt8](repeating: 0, count: 1 << 16)
        var offset: off_t = 0
        while true {
            let count = buffer.withUnsafeMutableBytes { pread(file, $0.baseAddress, $0.count, offset) }
            if count < 0 && errno == EINTR { continue }
            guard count >= 0 else { return .failure(Errno(errno)) }
            if count == 0 { break }
            hash.update(data: buffer[..<count])
            offset += off_t(count)
            guard UInt64(offset) <= limit else { return .failure(Errno(EFBIG)) }
        }
        var version = FileVersion(status: status, digest: hex(hash))
        version.size = UInt64(offset)
        return .success(version)
    }

    static func hex(_ hash: SHA256) -> String { hash.finalize().map { String(format: "%02x", $0) }.joined() }

    /// The status's size, time and identity with `digest`.
    init(status: stat, digest: String) {
        self.init(exists: true, size: UInt64(status.st_size),
                  mtimeNs: Int64(status.st_mtimespec.tv_sec) * 1_000_000_000 + Int64(status.st_mtimespec.tv_nsec),
                  dev: UInt64(UInt32(bitPattern: status.st_dev)), ino: status.st_ino,
                  birthNs: Int64(status.st_birthtimespec.tv_sec) * 1_000_000_000
                      + Int64(status.st_birthtimespec.tv_nsec),
                  digest: digest)
    }

    /// A regular file below the folder `folder` (a descriptor), opened without following a link at its name and
    /// without blocking on a FIFO: its descriptor, which the caller closes, and its version, or the errno.
    static func open(_ name: String, in folder: Int32) -> Result<(Int32, FileVersion), Errno> {
        let file = openat(folder, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)
        guard file >= 0 else { return .failure(Errno(errno)) }
        switch of(file) {
        case .success(let version): return .success((file, version))
        case .failure(let error):
            close(file)
            return .failure(error)
        }
    }

    /// The regular file at `path`, opened by that path itself (a security scope of one picked file need not cover
    /// its folder) without following a link at its name or blocking on a FIFO; nil when it cannot be read.
    public static func read(path: String) -> FileVersion? {
        let file = Darwin.open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)
        guard file >= 0 else { return nil }
        defer { close(file) }
        return try? of(file).get()
    }

    /// Copies exactly `content`'s bytes of the open file `source`, from offset 0 and hashed as they go, to
    /// "<name>.partial" below `folder` (one planted there is unlinked first) and renames it to `name` where no file has
    /// that name (errno EEXIST otherwise). False when the file has another content now (the app wrote meanwhile; what
    /// it grew is not copied), and for more than `limit` bytes or than leaves the volume copyReserve free.
    public static func copyWhole(from source: Int32, to name: String, in folder: Int32, mode: mode_t,
                                 content: FileVersion) -> Bool {
        guard content.size <= limit, !content.digest.isEmpty,
              DocumentClones.fits(off_t(content.size), free: DocumentClones.freeSpace(folder) ?? 0)
        else { return false }
        let partial = name + ".partial"
        unlinkat(folder, partial, 0)
        let file = openat(folder, partial, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode)
        guard file >= 0 else { return false }
        let written = copy(from: source, to: file, content) && fsync(file) == 0
        close(file)
        guard written, renameatx_np(folder, partial, folder, name, UInt32(RENAME_EXCL)) == 0 else {
            let error = errno
            unlinkat(folder, partial, 0)
            errno = error
            return false
        }
        return true
    }

    /// Copies `content.size` bytes of `source` to `target`: true when they have `content`'s digest.
    private static func copy(from source: Int32, to target: Int32, _ content: FileVersion) -> Bool {
        var hash = SHA256()
        var buffer = [UInt8](repeating: 0, count: 1 << 16)
        var offset: UInt64 = 0
        while offset < content.size {
            let want = Int(min(UInt64(buffer.count), content.size - offset))
            let count = buffer.withUnsafeMutableBytes { pread(source, $0.baseAddress, want, off_t(offset)) }
            if count < 0 && errno == EINTR { continue }
            guard count > 0,
                  buffer.withUnsafeBytes({ bytes in
                      DocumentClones.writeAll(target, UnsafeRawBufferPointer(rebasing: bytes[..<count]))
                  })
            else { return false }  // shorter than its version, or not written
            hash.update(data: buffer[..<count])
            offset += UInt64(count)
        }
        return hex(hash) == content.digest
    }
}

public struct Errno: Error, Equatable, Sendable {
    public let code: Int32
    init(_ code: Int32) { self.code = code }
}

/// What a replace of the original did.
public enum ReplaceResult: Sendable, Equatable {
    case written(FileVersion)
    /// It differs from what was expected, is gone or is a link: nothing was written.
    case changed
    /// It could not be tried; the write-back tries again later.
    case failed
}

/// The user's file a clone was made from, reached the way the host can reach it: a security-scoped bookmark under
/// NSFileCoordinator on the iPhone (the app's BookmarkOriginal), a path in the tests. Neither ever holds a file's
/// content in memory: versions are hashed and copies streamed.
public protocol Original: AnyObject, Sendable {
    /// What the host stores to find the file again; it may change (a stale bookmark made anew).
    var token: String { get }
    /// The version now; nil when the file is gone, a link, no regular file, over the limit or out of reach.
    func version() -> FileVersion?
    /// Replaces the file's content with the bytes of `source` (an open regular file, read from offset 0), whose
    /// version is `content`, if the file still has `expected`'s content, all or nothing. A file that has `content`
    /// already counts as written (a replace whose record was not stored before the host ended).
    func replace(ifStill expected: FileVersion, from source: Int32, content: FileVersion) -> ReplaceResult
}

/// An original by path, never read or written through a link at its name.
public final class PathOriginal: Original {
    public let path: String

    public init(path: String) { self.path = path }

    public var token: String { path }

    public func version() -> FileVersion? { FileVersion.read(path: path) }

    public func replace(ifStill expected: FileVersion, from source: Int32, content: FileVersion) -> ReplaceResult {
        guard let now = version() else { return .changed }
        if now.sameContent(content) { return .written(now) }
        guard now.sameContent(expected) else { return .changed }
        let url = URL(fileURLWithPath: path)
        let folder = Darwin.open(url.deletingLastPathComponent().path, O_RDONLY | O_DIRECTORY | O_CLOEXEC)
        guard folder >= 0 else { return .failed }
        defer { close(folder) }
        let name = url.lastPathComponent
        unlinkat(folder, "." + name + ".minato", 0)
        guard FileVersion.copyWhole(from: source, to: "." + name + ".minato", in: folder, mode: 0o644,
                                    content: content),
              renameat(folder, "." + name + ".minato", folder, name) == 0,
              let after = version()
        else { return .failed }
        return .written(after)
    }
}

/// The records of the clones the host made from the user's files (kinds 1 and 2), and what becomes of an edit.
///
/// Locks: the records' lock is never held while a file's content is read or the original is reached (a coordinated
/// read or write may wait for a download or for another process), only while the records change and are stored. The
/// work (scan, flush, secure, resolve, the session's end) runs on one queue of the host's, one call at a time; a
/// write-back reads the record, works without the lock, and stores its outcome unless the record was let go
/// meanwhile, keeping it due when the app closed the clone again. The guest's close only notes itself under a lock of
/// its own (`closed`), and the picker asks `reusable` and `busy` without any file work under the lock. A forgotten
/// clone is announced (`onForget`) with no lock held, as the host's answer, which stops watching the file, waits for a
/// close that may be waiting for a lock.
public final class WriteBack: Sendable {
    public enum State: Int, Codable, Sendable {
        case clean, dirty, conflict
    }

    /// The wait after the last close, as the addendum says: an app that closes and opens again writes once.
    public static let delayMs: Int64 = 1000
    /// A write that fails (the original out of reach) is tried again after 1, 2, 4, 8 and 16 s; the sixth failure
    /// makes a conflict, so that the user is asked instead of a retry each second without end.
    public static let attempts = 6

    private struct Record {
        var original: any Original
        var originalBase: FileVersion
        var cloneBase: FileVersion
        var state = State.clean
        var dueMs: Int64 = 0
        /// The app's version in the host's folder: in a conflict, or kept before the guest starts (`secure`).
        var copy = ""
        var failures = 0
        /// Changes with every close taken up, so that a write-back that ran meanwhile leaves the record due.
        var closes = 0
        /// The record's number, new for each record, so that a write-back's outcome never lands on another.
        var number = 0
        /// The clone was missing at the last scan, or the last write-back (not stored, counted apart): it goes when
        /// it is missing at the next of the same kind too.
        var scanMissing = false
        var flushMissing = false
        /// No writer of the app's may be open (not stored; a loaded record's writers went with their process): false
        /// from a change a scan saw without a close until the next close, which apkrt_watch_writes reports once the
        /// last writing description is gone. Only a settled clone is written back, but at the session's end.
        var settled = true
        /// The clone's size and time at the last look (nil: none yet since the record was made, loaded or written).
        var seen: (size: UInt64, mtimeNs: Int64)?
        /// The app closed the clone while its conflict was open (not stored): after a Replace that version is due too.
        var closedInConflict = false
    }

    /// A record as the host stores it, in its own folder.
    private struct Stored: Codable {
        var clone: String
        var state: Int
        var token: String
        var originalBase: FileVersion
        var cloneBase: FileVersion
        var copy: String
        var dueMs: Int64
        var failures: Int
    }

    private struct Inner {
        var records: [String: Record] = [:]
        var sequence = 0
        var numbers = 0
        /// The clones let go and not yet announced.
        var forgotten: [String] = []
    }

    /// What a write-back makes of a record.
    private enum Outcome {
        case keep(Record)
        case forget
    }

    private let download: URL
    private let stateFolder: URL?
    private let inner = Mutex(Inner())
    /// The guest's closes not yet taken up: the clone's name and the time.
    private let closes = Mutex([String: Int64]())
    private let onForget: @Sendable (String) -> Void
    private let writers: @Sendable (String) -> Int32?

    /// `download` is the folder the guest sees as Download; `state` the host's own folder for the records and the app's
    /// versions, never below the app's folders (nil keeps the records in memory only); `writers` the guest's open
    /// writers of a clone (apkrt_watch_writers; nil without a watch), asked with no lock held; `onForget` gets each
    /// clone whose record goes, with no lock held.
    public init(download: URL, state: URL?, writers: @escaping @Sendable (String) -> Int32? = { _ in nil },
                onForget: @escaping @Sendable (String) -> Void = { _ in }) {
        self.download = download
        stateFolder = state
        self.writers = writers
        self.onForget = onForget
    }

    /// Whether `url` lies in the trash, where the bookmark of a file the user deleted still leads: below `trash`, its
    /// volume's trash folder, or below a folder named ".Trash". Such an original is gone for the write-back.
    public static func inTrash(_ url: URL, trash: URL?) -> Bool {
        let parent = url.deletingLastPathComponent().resolvingSymlinksInPath().standardizedFileURL
        if let trash {
            let folder = trash.resolvingSymlinksInPath().standardizedFileURL.pathComponents
            if parent.pathComponents.starts(with: folder) { return true }
        }
        return parent.pathComponents.contains(".Trash")
    }

    /// The open writers of each clone now, asked before the records' lock is taken (the host's answer takes a lock of
    /// its own, which it may hold while it asks the records).
    private func openWriters() -> [String: Int32] {
        let clones = inner.withLock { Array($0.records.keys) }
        var counts: [String: Int32] = [:]
        for clone in clones {
            if let count = writers(clone) { counts[clone] = count }
        }
        return counts
    }

    /// Starts watching `clone` (a name in Download), made from `original`, which had `originalNow`; `cloneNow` is the
    /// clone's content as it was made. Its identity is taken from the file in Download now, right after it was made. A
    /// record the name had (an untouched clone gone before a scan saw it, as `busy` lets only those go) is forgotten
    /// and announced first, before this returns, so that the host's new watch comes after the old one's end.
    public func track(clone: String, original: any Original, originalNow: FileVersion, cloneNow: FileVersion) {
        var base = cloneNow
        (base.dev, base.ino, base.birthNs) = (0, 0, 0)
        if let folder = openDownload() {
            var status = stat()
            if fstatat(folder, clone, &status, AT_SYMLINK_NOFOLLOW) == 0, status.st_mode & S_IFMT == S_IFREG {
                let made = FileVersion(status: status, digest: "")
                (base.dev, base.ino, base.birthNs) = (made.dev, made.ino, made.birthNs)
            }
            close(folder)
        }
        inner.withLock { inner in
            forget(clone, &inner)
            inner.numbers += 1
            inner.records[clone] = Record(original: original, originalBase: originalNow, cloneBase: base,
                                          number: inner.numbers)
            _ = save(inner)
        }
        announce()
    }

    public func tracked(_ clone: String) -> Bool { inner.withLock { $0.records[clone] != nil } }

    /// The clones that have a record.
    public var clones: [String] { inner.withLock { $0.records.keys.sorted() } }

    /// Whether a new clone must not take the name `clone` (the rule in pick_host.h): a record with the app's
    /// work holds it (dirty, a conflict, or a close not yet taken up), even when the guest removed the clone, so that a
    /// new record never takes its place. An untouched record does not: its clone is gone when the name is free, and
    /// `track` lets it go.
    public func busy(_ clone: String) -> Bool {
        guard let state = inner.withLock({ $0.records[clone]?.state }) else { return false }
        return state != .clean || closes.withLock { $0[clone] != nil }
    }

    /// Whether a file picked again may get `clone` again (guest_interface.h's CLONE NAMES): a clone the app has not
    /// touched only while the original, whose version `originalNow` gives (read by the caller under the pick's
    /// coordination, never under this lock), still has the content it was made from; a clone with the app's edits
    /// stays; an original that cannot be read leaves the clone as it is. A clone without a record (the user let the
    /// app's version go) is the app's own file and never the user's document again.
    public func reusable(_ clone: String, originalNow: () -> FileVersion?) -> Bool {
        let record = inner.withLock { $0.records[clone].map { ($0.state, $0.originalBase) } }
        guard let (state, base) = record else { return false }
        if state != .clean || closes.withLock({ $0[clone] != nil }) { return true }
        guard let now = originalNow() else { return true }
        return now.sameContent(base)
    }

    /// The app closed the last descriptor that wrote `clone` (apkrt_watch_writes' callback, on the guest's thread
    /// inside its close): only noted, without the records' lock or any file work.
    public func closed(_ clone: String, at nowMs: Int64) {
        closes.withLock { $0[clone] = max($0[clone] ?? nowMs, nowMs) }
    }

    /// Whether a close came at or after `sinceMs` and is not taken up yet: the app may still be saving.
    public func closedSince(_ sinceMs: Int64) -> Bool { closes.withLock { $0.values.contains { $0 >= sinceMs } } }

    /// Finds the clones that changed without a close being reported (a write through a mapping, a rename over the
    /// clone); they are written back one second later, as after a close. An untouched clone that is gone at two scans
    /// in a row is forgotten (the rule on the review's point 7): a file the app makes under that name later is
    /// its own, not the user's file, while one that saves by deleting the clone and renaming a file to its name has it
    /// back by the next scan, a file of another identity and so a conflict.
    public func scan(at nowMs: Int64) {
        let counts = openWriters()
        inner.withLock { scan(&$0, at: nowMs, final: false, writers: counts) }
        announce()
    }

    /// Writes back what is due at `nowMs`, or everything dirty when `force` (the session goes to the background); a
    /// clone the app may still be writing (not settled) waits for its close, the session's end or the next start.
    public func flush(at nowMs: Int64, force: Bool) { flush(at: nowMs, force: force, final: false) }

    /// `final` at the session's end: a clone missing then is gone, as no later flush comes.
    private func flush(at nowMs: Int64, force: Bool, final: Bool) {
        let due = inner.withLock { inner -> [(String, Record)] in
            takeCloses(&inner)
            return inner.records.keys.sorted().compactMap { clone in
                let record = inner.records[clone]!
                // A due time far ahead is a clock set back: due now.
                let due = force || record.dueMs <= nowMs || record.dueMs > nowMs + (Self.delayMs << Self.attempts)
                return record.state == .dirty && due && (record.settled || final) ? (clone, record) : nil
            }
        }
        for (clone, record) in due {
            store(clone, record, writeBack(clone, record, at: nowMs, final: final))
        }
        announce()
    }

    /// The session ends: what a mapping changed is found, everything is written, the records are stored.
    public func endSession(at nowMs: Int64) {
        let counts = openWriters()
        inner.withLock { scan(&$0, at: nowMs, final: true, writers: counts) }
        flush(at: nowMs, force: true, final: true)
        inner.withLock { _ = save($0) }
    }

    /// Before the guest starts, whose cleaning deletes the clones it keeps no grant for: the app's version of every
    /// clone not yet written back goes to the host's folder (ADR-0057's addendum, point 4), so that the write-back
    /// still has it when the clone is gone. Local work only (a clone of the file where the volume can), never the
    /// original.
    public func secure(at nowMs: Int64) {
        let pending = inner.withLock { inner in
            scan(&inner, at: nowMs, final: false, writers: [:])  // the guest has not started
            // A dirty one's copy kept before is renewed; a conflict keeps the copy its question shows.
            return inner.records.filter { _, record in
                record.state == .dirty || record.state == .conflict && record.copy.isEmpty
            }
        }
        if let folder = openDownload() {
            defer { close(folder) }
            for (clone, before) in pending.sorted(by: { $0.key < $1.key }) {
                let file = openat(folder, clone, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)
                guard file >= 0 else { continue }
                defer { close(file) }
                var status = stat()
                guard fstat(file, &status) == 0, status.st_mode & S_IFMT == S_IFREG,
                      FileVersion(status: status, digest: "").sameFile(before.cloneBase)
                else { continue }
                var record = before
                keepCopy(&record, file, nil)
                store(clone, before, .keep(record))
            }
        }
        announce()
    }

    /// Whether the guest may delete the clone: it is written back or never changed.
    public func removable(_ clone: String) -> Bool {
        inner.withLock { inner in
            takeCloses(&inner)
            return inner.records[clone].map { $0.state == .clean } ?? true
        }
    }

    public var conflicts: [String] {
        inner.withLock { $0.records.filter { $0.value.state == .conflict }.keys.sorted() }
    }

    /// The app's version of a conflict, for the user to save elsewhere: the copy in the host's folder, never a path
    /// in the app's folders (the app may have put a link there); nil when there is none (a link, a FIFO or a folder
    /// where the clone was) or no conflict of that name.
    public func appVersion(_ clone: String) -> URL? {
        inner.withLock { inner in
            guard let record = inner.records[clone], record.state == .conflict, !record.copy.isEmpty,
                  let stateFolder
            else { return nil }
            return stateFolder.appending(path: record.copy)
        }
    }

    /// Whether the user's Replace can write the app's version of a conflict: a regular file within `limit`, the copy
    /// or else the clone while it is still the clone. Nothing is read.
    public func replaceable(_ clone: String) -> Bool {
        guard let record = inner.withLock({ $0.records[clone] }), record.state == .conflict,
              let folder = record.copy.isEmpty ? openDownload() : openState()
        else { return false }
        defer { close(folder) }
        var status = stat()
        guard fstatat(folder, record.copy.isEmpty ? clone : record.copy, &status, AT_SYMLINK_NOFOLLOW) == 0,
              status.st_mode & S_IFMT == S_IFREG, UInt64(status.st_size) <= FileVersion.limit
        else { return false }
        return !record.copy.isEmpty || FileVersion(status: status, digest: "").sameFile(record.cloneBase)
    }

    /// The user's decision for a conflict: `replace` writes the app's version over what the original is now (the
    /// clone keeps its identity in the record); else the app's version is let go and the record forgotten, the clone
    /// an ordinary file of the app's from then on. False when there is no such conflict or the write did not happen.
    public func resolve(_ clone: String, replace: Bool) -> Bool {
        guard let record = inner.withLock({ $0.records[clone] }), record.state == .conflict else { return false }
        guard replace else {
            inner.withLock { inner in
                guard inner.records[clone]?.number == record.number else { return }
                forget(clone, &inner)
                _ = save(inner)
            }
            announce()
            return true
        }
        guard let (file, version) = appsVersion(clone, record) else { return false }
        defer { close(file) }
        guard let now = record.original.version(),
              case .written(let after) = record.original.replace(ifStill: now, from: file, content: version)
        else { return false }
        var done = record
        done.originalBase = after
        let identity = record.cloneBase
        done.cloneBase = version
        (done.cloneBase.dev, done.cloneBase.ino, done.cloneBase.birthNs) =
            (identity.dev, identity.ino, identity.birthNs)
        done.state = .clean
        done.failures = 0
        // What the app wrote while the question was open is looked at anew, not against a look from before it; one it
        // closed then is due at once, as its close was taken up during the question.
        done.seen = nil
        done.settled = true
        if record.closedInConflict {
            done.state = .dirty
            done.dueMs = 0
        }
        done.closedInConflict = false
        dropCopy(&done)
        store(clone, record, .keep(done))
        return true
    }

    /// Stores the records in the host's own folder.
    @discardableResult
    public func save() -> Bool {
        inner.withLock { inner in
            takeCloses(&inner)
            return save(inner)
        }
    }

    /// Reads the records save() stored, each original made by `factory` from its token; false when there are none.
    public func load(_ factory: (String) -> (any Original)?) -> Bool {
        guard let folder = openState() else { return false }
        defer { close(folder) }
        guard case .success(let (file, _)) = FileVersion.open("records.json", in: folder) else { return false }
        defer { close(file) }
        var text = Data()
        var buffer = [UInt8](repeating: 0, count: 1 << 16)
        while text.count < 64 << 20 {
            let count = Darwin.read(file, &buffer, buffer.count)
            if count < 0 && errno == EINTR { continue }
            guard count > 0 else { break }
            text.append(buffer, count: count)
        }
        guard let stored = try? JSONDecoder().decode([Stored].self, from: text) else {
            // Kept aside under a name of its own, not overwritten unread by a save, with the copies it names.
            for n in 0..<1000 {
                let aside = n == 0 ? "records.json.unreadable" : "records.json.unreadable-\(n)"
                if renameatx_np(folder, "records.json", folder, aside, UInt32(RENAME_EXCL)) == 0 || errno != EEXIST {
                    break
                }
            }
            return false
        }
        inner.withLock { inner in
            for entry in stored {
                guard Self.plain(entry.clone), entry.copy.isEmpty || Self.plain(entry.copy),
                      let state = State(rawValue: entry.state),
                      [entry.originalBase, entry.cloneBase].allSatisfy({ $0.digest.isEmpty || $0.digest.count == 64 }),
                      (0...Self.attempts).contains(entry.failures),
                      let original = factory(entry.token)
                else { continue }
                inner.numbers += 1
                if entry.copy.hasPrefix("copy-"), let number = Int(entry.copy.dropFirst(5)) {
                    inner.sequence = max(inner.sequence, number)  // a new copy never takes a loaded one's name
                }
                inner.records[entry.clone] = Record(original: original, originalBase: entry.originalBase,
                                                    cloneBase: entry.cloneBase, state: state, dueMs: entry.dueMs,
                                                    copy: entry.copy, failures: entry.failures, number: inner.numbers)
            }
        }
        dropOrphanCopies(folder)
        return true
    }

    /// The copies no record names (one kept by a host that ended before it stored the record, a copy's leftover) go.
    private func dropOrphanCopies(_ folder: Int32) {
        let named = inner.withLock { Set($0.records.values.map(\.copy)) }
        let listed = (try? FileManager.default.contentsOfDirectory(atPath: stateFolder?.path ?? "")) ?? []
        for name in listed where name.hasPrefix("copy-") && !named.contains(name) {
            unlinkat(folder, name, 0)
        }
    }

    /// A name of one component.
    private static func plain(_ name: String) -> Bool {
        !name.isEmpty && name != "." && name != ".." && !name.contains("/") && !name.contains("\0")
    }

    private func takeCloses(_ inner: inout Inner) {
        let noted = closes.withLock { closes in
            defer { closes.removeAll() }
            return closes
        }
        let folder = noted.isEmpty ? nil : openDownload()
        defer { if let folder { close(folder) } }
        for (clone, at) in noted {
            guard var record = inner.records[clone] else { continue }
            if record.state == .conflict {
                inner.records[clone]?.closedInConflict = true
                continue
            }
            record.state = .dirty
            record.dueMs = at + Self.delayMs
            record.failures = 0
            record.closes += 1
            // The last writer is gone: the clone as it is now is what the app wrote.
            record.settled = true
            var status = stat()
            if let folder, fstatat(folder, clone, &status, AT_SYMLINK_NOFOLLOW) == 0 {
                let now = FileVersion(status: status, digest: "")
                record.seen = (now.size, now.mtimeNs)
            } else {
                record.seen = nil  // not there now: what is there at the write-back is what the app left
            }
            inner.records[clone] = record
        }
        if !noted.isEmpty { _ = save(inner) }
    }

    /// `writers` the open writers of the clones that have a watch.
    private func scan(_ inner: inout Inner, at nowMs: Int64, final: Bool, writers: [String: Int32]) {
        takeCloses(&inner)
        guard let folder = openDownload() else { return }
        defer { close(folder) }
        var changed = false
        for clone in inner.records.keys.sorted() {
            guard let record = inner.records[clone], record.state != .conflict else { continue }
            var status = stat()
            guard fstatat(folder, clone, &status, AT_SYMLINK_NOFOLLOW) == 0 else {
                // A dirty one missing is the write-back's to count.
                if errno == ENOENT, record.state == .clean {
                    if final || record.scanMissing {
                        forget(clone, &inner)
                        changed = true
                    } else {
                        inner.records[clone]?.scanMissing = true
                    }
                }
                continue
            }
            inner.records[clone]?.scanMissing = false
            let now = FileVersion(status: status, digest: "")
            if status.st_mode & S_IFMT != S_IFREG || !now.sameFile(record.cloneBase) {
                // Another file, or none regular, at the name: the write-back makes it a conflict.
                if record.state == .clean {
                    inner.records[clone]?.state = .dirty
                    inner.records[clone]?.dueMs = nowMs + Self.delayMs
                    changed = true
                }
                continue
            }
            guard let seen = record.seen ?? (record.state == .clean
                ? (record.cloneBase.size, record.cloneBase.mtimeNs) : nil)
            else {
                inner.records[clone]?.seen = (now.size, now.mtimeNs)  // a loaded dirty one's first look
                continue
            }
            if now.size != seen.size || now.mtimeNs != seen.mtimeNs {
                // Written with no close yet (a writer open, or a mapping): it waits for the close or the end, and is
                // looked at again while it changes.
                inner.records[clone]?.state = .dirty
                inner.records[clone]?.settled = false
                inner.records[clone]?.seen = (now.size, now.mtimeNs)
                inner.records[clone]?.dueMs = nowMs + Self.delayMs
                changed = true
            } else if record.state == .dirty, !record.settled, writers[clone] == 0 {
                // Still since the last look and no writer open (a mapping's or a truncate's change, which no close
                // reports): settled, written a second later. Without a watch only a close or the end settles it.
                inner.records[clone]?.settled = true
                inner.records[clone]?.dueMs = nowMs + Self.delayMs
            }
        }
        if changed { _ = save(inner) }
    }

    /// The outcome of `record`'s write-back, worked out without the lock.
    private func writeBack(_ clone: String, _ record: Record, at nowMs: Int64, final: Bool) -> Outcome {
        var record = record
        /// A write that did not happen (the original, Download or a descriptor out of reach for now), tried again
        /// later; the sixth a conflict, with the app's version `file` kept.
        func failed(_ file: Int32?, _ version: FileVersion?) -> Outcome {
            record.failures += 1
            if record.failures >= Self.attempts {
                if let file { keepCopy(&record, file, version) }
                record.state = .conflict
            } else {
                record.dueMs = nowMs + (Self.delayMs << (record.failures - 1))
            }
            return .keep(record)
        }
        if let count = writers(clone), count > 0 {
            // A writer is open again: no copy of a file it may be writing, also at the background and the end.
            record.dueMs = nowMs + Self.delayMs
            return .keep(record)
        }
        guard let folder = openDownload() else { return failed(nil, nil) }
        defer { close(folder) }
        let file: Int32
        let version: FileVersion
        var fromCopy = false
        switch FileVersion.open(clone, in: folder) {
        case .success(let opened):
            (file, version) = opened
            record.flushMissing = false
        case .failure(let error) where error.code == ENOENT:
            // Missing once, it is looked at again a second later: an app that saves by deleting the clone and renaming
            // a file to its name has it back by then (another identity, a conflict).
            guard final || record.flushMissing else {
                record.flushMissing = true
                record.dueMs = nowMs + Self.delayMs
                return .keep(record)
            }
            // The guest's cleaning took the clone: the version kept before its start, else nothing to write.
            guard !record.copy.isEmpty, let state = openState() else { return .forget }
            defer { close(state) }
            guard case .success(let opened) = FileVersion.open(record.copy, in: state) else { return .forget }
            (file, version) = opened
            fromCopy = true
        case .failure(let error) where [ELOOP, EINVAL, EFBIG, EOPNOTSUPP, ENXIO].contains(error.code):
            // A link, a folder, a FIFO or a file over the limit where the clone was: never read through. One over the
            // limit that is still the clone is kept by a clone of the file, no byte read.
            if error.code == EFBIG { keepUnread(&record, clone, in: folder) }
            record.state = .conflict
            return .keep(record)
        case .failure:
            return failed(nil, nil)  // no descriptor left, or out of reach for a moment
        }
        defer { close(file) }
        if !fromCopy && !version.sameFile(record.cloneBase) {
            // Another file has this name now (the app made one, or renamed one over the clone): it is not what was
            // cloned, so it is never written over the user's file; the user decides.
            keepCopy(&record, file, version)
            record.state = .conflict
            return .keep(record)
        }
        if !fromCopy, !final, let seen = record.seen, version.size != seen.size || version.mtimeNs != seen.mtimeNs {
            // Changed since the close was taken up: the app writes again, and its next close says when it is done.
            unsettle(&record, version, at: nowMs)
            return .keep(record)
        }
        if version.sameContent(record.cloneBase) {
            if !fromCopy { (record.cloneBase.size, record.cloneBase.mtimeNs) = (version.size, version.mtimeNs) }
            record.seen = nil
            record.state = .clean
            dropCopy(&record)
            return fromCopy ? .forget : .keep(record)
        }
        switch record.original.replace(ifStill: record.originalBase, from: file, content: version) {
        case .written(let after):
            if fromCopy { return .forget }
            record.originalBase = after
            (record.cloneBase.size, record.cloneBase.mtimeNs, record.cloneBase.digest) =
                (version.size, version.mtimeNs, version.digest)
            record.state = .clean
            record.failures = 0
            record.seen = nil
            dropCopy(&record)
        case .changed:
            if !fromCopy { keepCopy(&record, file, version) }
            record.state = .conflict
        case .failed:
            // A copy the app wrote into meanwhile took nothing: the next look sees the change and waits for the close,
            // whose take-up starts the count of failures again.
            return failed(fromCopy ? nil : file, version)
        }
        return .keep(record)
    }

    /// The app is writing `record`'s clone, now at `now`: written back after its next close (or at the end).
    private func unsettle(_ record: inout Record, _ now: FileVersion, at nowMs: Int64) {
        record.settled = false
        record.seen = (now.size, now.mtimeNs)
        record.dueMs = nowMs + Self.delayMs
    }

    /// Stores `outcome` for the record `before` was, unless it was let go meanwhile; a close taken up meanwhile keeps
    /// it due.
    private func store(_ clone: String, _ before: Record, _ outcome: Outcome) {
        inner.withLock { inner in
            guard let now = inner.records[clone], now.number == before.number else {
                if case .keep(var record) = outcome, record.copy != before.copy { dropCopy(&record) }
                return
            }
            switch outcome {
            case .forget:
                forget(clone, &inner)
            case .keep(var record):
                record.closes = now.closes
                if now.closes != before.closes, record.state == .clean {
                    record.state = .dirty
                    record.dueMs = now.dueMs
                }
                inner.records[clone] = record
            }
            _ = save(inner)
        }
    }

    /// Lets the record of `clone` go, with the app's version it kept.
    private func forget(_ clone: String, _ inner: inout Inner) {
        guard var record = inner.records.removeValue(forKey: clone) else { return }
        dropCopy(&record)
        inner.forgotten.append(clone)
    }

    private func announce() {
        let gone = inner.withLock { inner in
            defer { inner.forgotten.removeAll() }
            return inner.forgotten
        }
        gone.forEach(onForget)
    }

    /// The app's version of a conflict, opened: the copy, else the clone while it is still the clone.
    private func appsVersion(_ clone: String, _ record: Record) -> (Int32, FileVersion)? {
        if !record.copy.isEmpty, let state = openState() {
            defer { close(state) }
            return try? FileVersion.open(record.copy, in: state).get()
        }
        guard let folder = openDownload() else { return nil }
        defer { close(folder) }
        guard let (file, version) = try? FileVersion.open(clone, in: folder).get() else { return nil }
        guard version.sameFile(record.cloneBase) else {
            close(file)
            return nil
        }
        return (file, version)
    }

    /// Keeps the app's version, the open file `file`, in the host's folder under a new name, the copy kept before
    /// going: the guest's cleaning may delete the clone, and the kept version is always the app's latest. A clone of
    /// the file where the volume can (the host's folder and Download share it; no byte read), else a copy of
    /// `version` (none without it). Only the name is taken under the lock.
    private func keepCopy(_ record: inout Record, _ file: Int32, _ version: FileVersion?) {
        guard let folder = openState() else { return }
        defer { close(folder) }
        for _ in 0..<10000 {
            let name = inner.withLock { inner in
                inner.sequence += 1
                return "copy-\(inner.sequence)"
            }
            let kept: Bool
            if fclonefileat(file, folder, name, UInt32(CLONE_NOFOLLOW)) == 0 {
                fchmodat(folder, name, 0o600, AT_SYMLINK_NOFOLLOW)
                kept = true
            } else {
                kept = errno != EEXIST && (version ?? (try? FileVersion.of(file).get())).map {
                    FileVersion.copyWhole(from: file, to: name, in: folder, mode: 0o600, content: $0)
                } == true
            }
            guard kept else {
                if errno == EEXIST { continue }  // a name taken (a copy left by a host that ended)
                return
            }
            dropCopy(&record)
            record.copy = name
            return
        }
    }

    /// Keeps the clone `clone`, over the limit, by a clone of the file while it is still the clone.
    private func keepUnread(_ record: inout Record, _ clone: String, in folder: Int32) {
        let file = openat(folder, clone, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)
        guard file >= 0 else { return }
        defer { close(file) }
        var status = stat()
        guard fstat(file, &status) == 0, status.st_mode & S_IFMT == S_IFREG,
              FileVersion(status: status, digest: "").sameFile(record.cloneBase)
        else { return }
        keepCopy(&record, file, nil)
    }

    private func dropCopy(_ record: inout Record) {
        if !record.copy.isEmpty, let folder = openState() {
            unlinkat(folder, record.copy, 0)
            close(folder)
        }
        record.copy = ""
    }

    private func save(_ inner: Inner) -> Bool {
        guard let folder = openState() else { return stateFolder == nil }
        defer { close(folder) }
        let stored = inner.records.keys.sorted().map { clone in
            let record = inner.records[clone]!
            return Stored(clone: clone, state: record.state.rawValue, token: record.original.token,
                          originalBase: record.originalBase, cloneBase: record.cloneBase, copy: record.copy,
                          dueMs: record.dueMs, failures: record.failures)
        }
        guard let text = try? JSONEncoder().encode(stored) else { return false }
        let partial = "records.json.partial"
        unlinkat(folder, partial, 0)
        let file = openat(folder, partial, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0o600)
        guard file >= 0 else { return false }
        let written = text.withUnsafeBytes { DocumentClones.writeAll(file, $0) } && fsync(file) == 0
        close(file)
        guard written, renameat(folder, partial, folder, "records.json") == 0 else {
            unlinkat(folder, partial, 0)
            return false
        }
        fsync(folder)
        return true
    }

    private func openDownload() -> Int32? {
        let folder = Darwin.open(download.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        return folder >= 0 ? folder : nil
    }

    private func openState() -> Int32? {
        guard let stateFolder else { return nil }
        let folder = Darwin.open(stateFolder.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        return folder >= 0 ? folder : nil
    }
}
