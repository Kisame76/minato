import Darwin
import Foundation
import Synchronization

/// The images and videos an app finished in MediaStore, for Photos (ADR-0057 step e, APKRT_REQUEST_EXPORT_MEDIA). The
/// request handler runs on the guest's thread under the runtime's pick lock, so it only takes the file: walked from
/// the host's own record of the data folder with O_NOFOLLOW at every name, opened without blocking (a FIFO the app made
/// must not hold it), a regular file of one link within the limits. The copy for Photos is made later, off that
/// thread, from the descriptor alone (MediaExportJob), never from the path, which the app could swap for a link.
public final class MediaExports: Sendable {
    /// The most files a session exports, the largest one, and the most bytes in all (apk-run's model): an app must
    /// not fill the storage or the library.
    public static let countLimit = 1000
    public static let fileLimit: Int64 = 1 << 30
    public static let totalLimit: Int64 = 4 << 30
    /// The most files taken and not yet copied: each holds a descriptor of the process, which the host shares with
    /// the guest.
    public static let takenLimit = 32

    private struct Spent {
        var count = 0
        var bytes: Int64 = 0
    }

    private let dataFolder: URL?
    private let countLimit: Int
    private let fileLimit: Int64
    private let totalLimit: Int64
    private let spent = Mutex(Spent())
    private let taken = Taken()

    /// `dataFolder` is the app's data folder from the host's records (ApplicationInfo.dataDir); without one nothing
    /// is taken.
    public init(dataFolder: URL?, countLimit: Int = countLimit, fileLimit: Int64 = fileLimit,
                totalLimit: Int64 = totalLimit) {
        self.dataFolder = dataFolder
        self.countLimit = countLimit
        self.fileLimit = fileLimit
        self.totalLimit = totalLimit
    }

    /// The request's text, "<kind>\t<path>" (kind 1 an image, 2 a video; the path below the data folder): the file
    /// taken, or nil when it is refused. The runtime checked the text; this checks its shape again.
    public func take(_ text: String) -> MediaExportJob? {
        let bytes = Array(text.utf8)
        guard bytes.count > 2, bytes[1] == UInt8(ascii: "\t"),
              let kind: MediaExportJob.Kind = bytes[0] == UInt8(ascii: "1") ? .image
                  : bytes[0] == UInt8(ascii: "2") ? .video : nil
        else { return nil }
        let names = bytes[2...].split(separator: UInt8(ascii: "/"), omittingEmptySubsequences: false)
            .map { String(decoding: $0, as: UTF8.self) }
        guard names.count <= 16, names.allSatisfy({ !$0.isEmpty && $0 != "." && $0 != ".." }),
              let folder = dataFolder, taken.reserve()
        else { return nil }
        guard let (source, size) = open(names, below: folder) else {
            taken.release()
            return nil
        }
        return MediaExportJob(source: source, kind: kind, name: names.last!, size: size, taken: taken)
    }

    /// The file's descriptor and size, charged to the session's limits, or nil.
    private func open(_ names: [String], below folder: URL) -> (Int32, Int64)? {
        let data = Darwin.open(folder.path, O_RDONLY | O_DIRECTORY | O_CLOEXEC)
        guard data >= 0 else { return nil }
        var opened = [data]
        defer { opened.forEach { close($0) } }
        for name in names.dropLast() {
            // A FIFO here does not block either: O_DIRECTORY refuses it before it opens.
            let next = openat(opened.last!, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
            guard next >= 0 else { return nil }
            opened.append(next)
        }
        let source = openat(opened.last!, names.last!, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)
        guard source >= 0 else { return nil }
        var status = stat()
        let size = fstat(source, &status) == 0 ? Int64(status.st_size) : -1
        let within = size >= 0 && status.st_mode & S_IFMT == S_IFREG && status.st_nlink == 1 && size <= fileLimit
            && spent.withLock { spent in
                guard spent.count < countLimit, spent.bytes + size <= totalLimit else { return false }
                spent.count += 1
                spent.bytes += size
                return true
            }
        guard within else {
            close(source)
            return nil
        }
        return (source, size)
    }
}

/// The count of the jobs whose descriptor is open.
final class Taken: Sendable {
    private let count = Mutex(0)

    func reserve() -> Bool {
        count.withLock { count in
            guard count < MediaExports.takenLimit else { return false }
            count += 1
            return true
        }
    }

    func release() { count.withLock { $0 -= 1 } }
}

/// A file taken for Photos: its descriptor, open until the copy is made or the job goes, and its name and size when
/// it was taken.
public final class MediaExportJob: Sendable {
    public enum Kind: Sendable {
        case image
        case video
    }

    /// Advisory: the host does not trust the file to be a picture, Photos refuses one that is not.
    public let kind: Kind
    public let name: String
    public let size: Int64
    private let source: Mutex<Int32>
    private let taken: Taken

    init(source: Int32, kind: Kind, name: String, size: Int64, taken: Taken) {
        self.source = Mutex(source)
        self.kind = kind
        self.name = name
        self.size = size
        self.taken = taken
    }

    deinit { end() }

    /// The descriptor, -1 once the job has ended.
    var descriptor: Int32 { source.withLock { $0 } }

    /// The name Photos shows, the app's name without control, format, separator, private-use and unassigned
    /// characters, which could make it read as another; "Export" with the extension when only that is left.
    public var shownName: String {
        let hidden: Set<Unicode.GeneralCategory> = [.control, .format, .lineSeparator, .paragraphSeparator,
                                                   .privateUse, .surrogate, .unassigned]
        let shown = String(String.UnicodeScalarView(name.unicodeScalars.filter {
            !hidden.contains($0.properties.generalCategory)
        }))
        guard shown.isEmpty || shown.hasPrefix(".") else { return shown }
        let last = shown.contains(".") ? shown.split(separator: ".", omittingEmptySubsequences: false).last ?? "" : ""
        let suffix = String(last.filter { $0.isASCII && ($0.isLetter || $0.isNumber) })
        return suffix.isEmpty ? "Export" : "Export.\(suffix)"
    }

    /// Copies the file taken into a new folder of its own below `folder` (the host's), under its name: the copy's
    /// URL, or nil, and then nothing stays. The copy is the file's content now, cut to its size when it was taken;
    /// one that shrank meanwhile is not copied. A clone first, which costs neither time nor room, else a copy
    /// (`cloning` false: the copy alone). Ends the job: its descriptor is closed.
    public func copy(into folder: URL, cloning: Bool = true) -> URL? {
        defer { end() }
        let source = descriptor
        guard source >= 0 else { return nil }
        let parent = Darwin.open(folder.path, O_RDONLY | O_DIRECTORY | O_CLOEXEC)
        guard parent >= 0 else { return nil }
        defer { close(parent) }
        let own = UUID().uuidString
        guard mkdirat(parent, own, 0o700) == 0 else { return nil }
        let target = openat(parent, own, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard target >= 0 else {
            unlinkat(parent, own, AT_REMOVEDIR)
            return nil
        }
        defer { close(target) }
        guard cloning && clone(source, into: target) || write(source, into: target) else {
            unlinkat(target, name, 0)
            unlinkat(parent, own, AT_REMOVEDIR)
            return nil
        }
        return folder.appending(path: own).appending(path: name)
    }

    private func end() {
        let closed = source.withLock { source -> Bool in
            guard source >= 0 else { return false }
            close(source)
            source = -1
            return true
        }
        if closed { taken.release() }
    }

    /// The clone, cut to the size taken; false, and no clone, when there is none or it is too short. A clone keeps
    /// the source's mode, which the app may have made read-only.
    private func clone(_ source: Int32, into target: Int32) -> Bool {
        guard fclonefileat(source, target, name, UInt32(CLONE_NOFOLLOW)) == 0 else { return false }
        var status = stat()
        guard fstatat(target, name, &status, AT_SYMLINK_NOFOLLOW) == 0, Int64(status.st_size) >= size else {
            unlinkat(target, name, 0)
            return false
        }
        if Int64(status.st_size) == size { return true }
        let made = fchmodat(target, name, 0o600, AT_SYMLINK_NOFOLLOW) == 0
            ? openat(target, name, O_WRONLY | O_NOFOLLOW | O_CLOEXEC) : -1
        let cut = made >= 0 && ftruncate(made, off_t(size)) == 0
        if made >= 0 { close(made) }
        if !cut { unlinkat(target, name, 0) }
        return cut
    }

    /// The copy read from the descriptor's start, exactly `size` bytes.
    private func write(_ source: Int32, into target: Int32) -> Bool {
        let copy = openat(target, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0o600)
        guard copy >= 0 else { return false }
        defer { close(copy) }
        var buffer = [UInt8](repeating: 0, count: 1 << 16)
        return buffer.withUnsafeMutableBytes { bytes in
            var done: Int64 = 0
            while done < size {
                let count = pread(source, bytes.baseAddress, min(bytes.count, Int(size - done)), off_t(done))
                if count < 0 && errno == EINTR { continue }
                guard count > 0 else { return false }
                var written = 0
                while written < count {
                    let step = Darwin.write(copy, bytes.baseAddress! + written, count - written)
                    if step < 0 && errno == EINTR { continue }
                    guard step > 0 else { return false }
                    written += step
                }
                done += Int64(count)
            }
            return true
        }
    }
}
