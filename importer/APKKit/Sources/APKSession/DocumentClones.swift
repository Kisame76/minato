import Darwin
import Foundation
import Synchronization

/// The answer to a document request (ADR-0057 step b, kinds 1 and 2) on the iPhone: each file the user picked is cloned
/// into the folder the guest sees as /storage/emulated/0/Download, and the clones' names go to the manifest
/// ".minato/picks/<ID>" below the app's data folder (guest_interface.h, APKRT_EVENT_PICK_RESULT). The app can write
/// both folders and leave links, files and loops there, so the host opens them once from its own records and works
/// only from their descriptors: folders with O_DIRECTORY|O_NOFOLLOW and mkdirat, clones made new (O_EXCL, or a
/// failing fclonefileat), a name busy only when that fails with EEXIST, the manifest written to "<ID>.partial" and
/// renamed on the same descriptor. runtime/tools/pick_host.h is the Mac's same host and the reference.
public final class DocumentClones: Sendable {
    /// A file the user picked: `key` names it as the same document when it is picked again (its path), `name` is
    /// its file name, `file` where the host reads it (its own path, under the file's coordination).
    public struct Pick: Sendable {
        public let key: String
        public let name: String
        public let file: URL

        public init(key: String, name: String, file: URL) {
            self.key = key
            self.name = name
            self.file = file
        }
    }

    /// The most picks one answer names (guest_interface.h).
    public static let pickLimit = 1000

    /// One pick's clone: its name, and whether this answer made it (else it is the earlier clone of a document picked
    /// again), so that a cancel removes only what it made.
    public struct Clone: Sendable, Equatable {
        public let name: String
        public let made: Bool
        /// The version of a clone just made, read from the clone itself: what its write-back record starts from (the
        /// original read again may have changed meanwhile). Nil for a clone used again, and one over the write-back's
        /// limit.
        public var version: FileVersion? = nil
    }

    private let dataFolder: URL
    private let download: URL
    /// A clone of the file, else a copy of its bytes; false in a test of the copy alone.
    private let cloning: Bool
    /// The bytes free on the Download folder's volume, which a copy must leave room on.
    private let freeSpace: @Sendable (Int32) -> UInt64?
    /// The clone of each document picked so far; the answers run one after the other.
    private let clones = Mutex([String: String]())
    /// Whether an earlier clone may serve a document picked again, given the picked file's version, which it asks
    /// only when it needs it (WriteBack.reusable: not after the user changed the file elsewhere, unless the app has
    /// edits; never a clone whose record the user let go).
    private let reusable: @Sendable (String, () -> FileVersion?) -> Bool
    /// Whether a name must not be taken by a new clone (WriteBack.busy: a record holds it).
    private let busy: @Sendable (String) -> Bool

    /// What a copy leaves free on the device at least.
    static let copyReserve: UInt64 = 256 << 20

    public convenience init(dataFolder: URL, download: URL,
                            reusable: @escaping @Sendable (String, () -> FileVersion?) -> Bool = { _, _ in true },
                            busy: @escaping @Sendable (String) -> Bool = { _ in false }) {
        self.init(dataFolder: dataFolder, download: download, cloning: true, reusable: reusable, busy: busy)
    }

    init(dataFolder: URL, download: URL, cloning: Bool,
         freeSpace: @escaping @Sendable (Int32) -> UInt64? = DocumentClones.freeSpace,
         reusable: @escaping @Sendable (String, () -> FileVersion?) -> Bool = { _, _ in true },
         busy: @escaping @Sendable (String) -> Bool = { _ in false }) {
        self.dataFolder = dataFolder
        self.download = download
        self.cloning = cloning
        self.freeSpace = freeSpace
        self.reusable = reusable
        self.busy = busy
    }

    /// An answer: the number of picks its manifest names, nil for a cancel, and the clones it made.
    public struct Answer: Sendable {
        public let picked: Int?
        let made: [Clone]
    }

    /// Clones `picks` and writes the manifest of request `id`: the number of picks, or nil for a cancel (none, more
    /// than 1000, a file that cannot be read or cloned, a link where a folder of the manifest should be), which leaves
    /// none of the clones it made.
    public func answer(id: UInt64, picks: [Pick]) -> Int? {
        answer(id: id, count: picks.count, most: Self.pickLimit, cancelled: { false }) { clone(picks[$0]) }.picked
    }

    /// The answer to request `id` with `count` files, which `cloning` clones one by one (with clone(_:), under each
    /// file's coordination): a cancel before any clone when there are none or more than `most`, and a cancel that
    /// leaves none of the clones it made when one fails or `cancelled` says the guest no longer wants it, which it
    /// asks before each clone and before the manifest.
    public func answer(id: UInt64, count: Int, most: Int, cancelled: () -> Bool,
                       cloning: (Int) -> Clone?) -> Answer {
        guard (1...min(most, Self.pickLimit)).contains(count) else { return Answer(picked: nil, made: []) }
        var made: [Clone] = []
        for i in 0..<count {
            guard !cancelled(), let clone = cloning(i) else {
                discard(made)
                return Answer(picked: nil, made: [])
            }
            made.append(clone)
        }
        guard !cancelled() else {
            discard(made)
            return Answer(picked: nil, made: [])
        }
        return Answer(picked: finish(id: id, clones: made), made: made)
    }

    /// Takes back `answer` to request `id`, for a cancel that came once its manifest was written: the manifest and the
    /// clones it made go, the earlier clones of documents picked again stay.
    public func withdraw(_ answer: Answer, id: UInt64) {
        guard answer.picked != nil else { return }
        discard(answer.made)
        let data = Darwin.open(dataFolder.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard data >= 0 else { return }
        defer { close(data) }
        let minato = openat(data, ".minato", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard minato >= 0 else { return }
        defer { close(minato) }
        let picks = openat(minato, "picks", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard picks >= 0 else { return }
        defer { close(picks) }
        unlinkat(picks, String(id), 0)
    }

    /// The manifest of request `id` naming `clones` (clone's): their number, or nil for a cancel, which removes the
    /// clones it made.
    public func finish(id: UInt64, clones: [Clone]) -> Int? {
        guard !clones.isEmpty, clones.count <= Self.pickLimit,
              writeManifest(id: id, clones.map { $0.name + "\n" }.joined())
        else {
            discard(clones)
            return nil
        }
        return clones.count
    }

    /// Removes the clones an answer that ends as a cancel made, which no manifest names and so no cleaning of the
    /// guest's would find; the earlier clones of documents picked again stay.
    public func discard(_ clones: [Clone]) {
        let folder = Darwin.open(download.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard folder >= 0 else { return }
        defer { close(folder) }
        for clone in clones where clone.made {
            unlinkat(folder, clone.name, 0)
            self.clones.withLock { map in map = map.filter { $0.value != clone.name } }
        }
    }

    /// The clone of one pick, for a caller that reads each file under its own coordination: the earlier one for a
    /// document picked again while that is still a regular file of that name, else a new one; nil when the file
    /// cannot be read or cloned. The file is opened without following a link or blocking, and must be a regular file.
    public func clone(_ pick: Pick) -> Clone? {
        let folder = Darwin.open(download.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard folder >= 0 else { return nil }
        defer { close(folder) }
        let source = Darwin.open(pick.file.path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)
        guard source >= 0 else { return nil }
        defer { close(source) }
        var status = stat()
        guard fstat(source, &status) == 0, status.st_mode & S_IFMT == S_IFREG else { return nil }
        if let earlier = clones.withLock({ $0[pick.key] }) {
            var clone = stat()
            if fstatat(folder, earlier, &clone, AT_SYMLINK_NOFOLLOW) == 0, clone.st_mode & S_IFMT == S_IFREG,
               reusable(earlier, { try? FileVersion.of(source).get() }) {
                return Clone(name: earlier, made: false)
            }
        }
        let plain = Self.plainName(Array(pick.name.utf8))
        for n in 0..<10000 {
            let name = Self.numbered(plain, n)
            if busy(name) { continue }
            // CLONE_NOFOLLOW: a dangling link at the name is not followed (without it the clone is made at the link's
            // target); the clone fails with EEXIST for any name that is there.
            if cloning, fclonefileat(source, folder, name, UInt32(CLONE_NOFOLLOW)) == 0 {
                if Self.makePlain(name, in: folder) { return remember(pick, name, in: folder) }
                unlinkat(folder, name, 0)
            } else if cloning, errno == EEXIST {
                continue
            }
            guard Self.fits(status.st_size, free: freeSpace(folder) ?? 0) else { return nil }
            let copy = openat(folder, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0o644)
            if copy < 0 {
                if errno == EEXIST { continue }
                return nil
            }
            let copied = Self.copy(from: source, to: copy) && fsync(copy) == 0
            close(copy)
            guard copied else {
                unlinkat(folder, name, 0)
                return nil
            }
            return remember(pick, name, in: folder)
        }
        return nil
    }

    /// A clone keeps the user's file's mode and flags: it is made a plain file the app can edit and remove, as
    /// Android's document, through its own descriptor (a regular file of one link, the one just made). False when it
    /// cannot be, and the caller copies the bytes instead.
    static func makePlain(_ name: String, in folder: Int32) -> Bool {
        let made = openat(folder, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)
        guard made >= 0 else { return false }
        defer { close(made) }
        var status = stat()
        return fstat(made, &status) == 0 && status.st_mode & S_IFMT == S_IFREG && status.st_nlink == 1
            && fchflags(made, 0) == 0 && fchmod(made, 0o644) == 0
    }

    /// Names `name` as the clone of `pick`'s document, and of none other: a name the app freed may have been taken by
    /// another document's clone since (pick_host.h's rule).
    private func remember(_ pick: Pick, _ name: String, in folder: Int32) -> Clone {
        clones.withLock { map in
            map = map.filter { $0.value != name }
            map[pick.key] = name
        }
        let made = try? FileVersion.open(name, in: folder).get()
        if let made { close(made.0) }
        return Clone(name: name, made: true, version: made?.1)
    }

    /// The manifest to "<id>.partial" in <data>/.minato/picks, renamed to "<id>", so that the guest never reads half
    /// of it; a planted file or link of the temporary name is unlinked, never followed or written to.
    private func writeManifest(id: UInt64, _ manifest: String) -> Bool {
        let data = Darwin.open(dataFolder.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard data >= 0 else { return false }
        defer { close(data) }
        guard let minato = Self.openSub(data, ".minato") else { return false }
        defer { close(minato) }
        guard let picks = Self.openSub(minato, "picks") else { return false }
        defer { close(picks) }
        let target = String(id)
        var partial = target + ".partial"
        var file = openat(picks, partial, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0o644)
        if file < 0 && errno == EEXIST {
            unlinkat(picks, partial, 0)
            file = openat(picks, partial, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0o644)
        }
        if file < 0 && errno == EEXIST {
            // Something the app put there that is no file (a folder): a name at random (guest_interface.h).
            partial = "\(target).\(UUID().uuidString).partial"
            file = openat(picks, partial, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0o644)
        }
        guard file >= 0 else { return false }
        let bytes = Array(manifest.utf8)
        let written = bytes.withUnsafeBytes { Self.writeAll(file, $0) } && fsync(file) == 0
        close(file)
        guard written, renameat(picks, partial, picks, target) == 0 else {
            unlinkat(picks, partial, 0)
            return false
        }
        return true
    }

    /// The folder `name` below `parent`, made when missing; never through a link.
    static func openSub(_ parent: Int32, _ name: String) -> Int32? {
        for _ in 0..<2 {
            let folder = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
            if folder >= 0 { return folder }
            guard errno == ENOENT else { return nil }
            guard mkdirat(parent, name, 0o755) == 0 || errno == EEXIST else { return nil }
        }
        return nil
    }

    /// Whether a copy of `size` bytes leaves copyReserve of `free` bytes.
    static func fits(_ size: off_t, free: UInt64) -> Bool {
        size >= 0 && free >= copyReserve && UInt64(size) <= free - copyReserve
    }

    /// The bytes free for the app on the volume of `folder`.
    @Sendable static func freeSpace(_ folder: Int32) -> UInt64? {
        var volume = statfs()
        guard fstatfs(folder, &volume) == 0 else { return nil }
        return UInt64(volume.f_bavail) * UInt64(volume.f_bsize)
    }

    static func copy(from source: Int32, to target: Int32) -> Bool {
        var buffer = [UInt8](repeating: 0, count: 1 << 16)
        return buffer.withUnsafeMutableBytes { bytes in
            var offset: off_t = 0
            while true {
                let count = pread(source, bytes.baseAddress, bytes.count, offset)
                if count < 0 && errno == EINTR { continue }
                if count == 0 { return true }
                guard count > 0, writeAll(target, UnsafeRawBufferPointer(rebasing: bytes[..<count])) else {
                    return false
                }
                offset += off_t(count)
            }
        }
    }

    static func writeAll(_ file: Int32, _ bytes: UnsafeRawBufferPointer) -> Bool {
        var done = 0
        while done < bytes.count {
            let step = write(file, bytes.baseAddress! + done, bytes.count - done)
            if step < 0 && errno == EINTR { continue }
            guard step > 0 else { return false }
            done += step
        }
        return true
    }

    /// A file name the guest takes as plain (guest_interface.h, pick_host.h's plain_name): a byte that is no UTF-8, a
    /// control, a separator, a bidirectional, zero-width or other formatting character, and "/" become "_"; at most
    /// `limit` bytes, cut between code points with the extension (up to 16 bytes) kept; "", "." and ".." become
    /// "Untitled".
    public static func plainName(_ name: [UInt8], limit: Int = 255) -> String {
        var clean: [UInt8] = []
        var i = 0
        while i < name.count {
            let lead = name[i]
            let length = lead < 0x80 ? 1 : lead >> 5 == 6 ? 2 : lead >> 4 == 14 ? 3 : lead >> 3 == 30 ? 4 : 0
            var point = UInt32(lead < 0x80 ? lead : length == 2 ? lead & 0x1F : length == 3 ? lead & 0x0F : lead & 0x07)
            var valid = length != 0 && i + length <= name.count
            var k = 1
            while valid && k < length {
                valid = name[i + k] & 0xC0 == 0x80
                point = point << 6 | UInt32(name[i + k] & 0x3F)
                k += 1
            }
            valid = valid && (length != 2 || point >= 0x80) && (length != 3 || point >= 0x800)
                && (length != 4 || (0x10000...0x10FFFF).contains(point)) && !(0xD800...0xDFFF).contains(point)
            let plain = valid && point >= 0x20 && point != 0x7F && !(0x80...0x9F).contains(point)
                && point != UInt32(UInt8(ascii: "/")) && !(0x200B...0x200F).contains(point)
                && !(0x2028...0x202E).contains(point) && !(0x2060...0x206F).contains(point) && point != 0xFEFF
                && !(0xFFF9...0xFFFB).contains(point)
            if plain {
                clean += name[i..<i + length]
                i += length
            } else {
                clean.append(UInt8(ascii: "_"))
                i += valid ? length : 1
            }
        }
        if clean.isEmpty || clean == [0x2E] || clean == [0x2E, 0x2E] { return "Untitled" }
        if clean.count <= limit { return String(decoding: clean, as: UTF8.self) }
        let dot = clean.lastIndex(of: 0x2E)
        let ext = dot.map { $0 > 0 && clean.count - $0 <= 16 ? Array(clean[$0...]) : [] } ?? []
        var keep = limit - ext.count
        while keep > 0 && clean[keep] & 0xC0 == 0x80 { keep -= 1 }
        return String(decoding: clean[..<keep] + ext, as: UTF8.self)
    }

    /// Candidate `n` for a plain name: the name itself for 0, else "<stem> (<n>)<extension>", at most 255 bytes.
    public static func numbered(_ name: String, _ n: Int) -> String {
        if n == 0 { return name }
        let bytes = Array(name.utf8)
        let dot = bytes.lastIndex(of: 0x2E)
        let hasExtension = dot.map { $0 > 0 && bytes.count - $0 <= 16 } ?? false
        let suffix = Array(" (\(n))".utf8)
        let ext = hasExtension ? Array(bytes[dot!...]) : []
        let stem = hasExtension ? Array(bytes[..<dot!]) : bytes
        return plainName(stem, limit: 255 - ext.count - suffix.count) + String(decoding: suffix + ext, as: UTF8.self)
    }
}
