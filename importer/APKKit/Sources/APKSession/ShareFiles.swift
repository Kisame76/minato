import Darwin
import Foundation

/// A share's files (ADR-0062, decision 3), which the guest put into ".minato/share/<ID>" below its data folder. The app
/// can write that folder and leave links, folders and FIFOs there, so the host walks it through descriptors only: the
/// data folder from its own records, then each of ".minato", "share" and "<ID>" with O_NOFOLLOW, each file with
/// O_NOFOLLOW and O_NONBLOCK, taken only as a regular file of one link within the limit. It copies the files into a folder of its
/// own for the share sheet and never writes into the share folder.
public enum ShareFiles {
    /// The most bytes of one file.
    public static let fileLimit: Int64 = 256 << 20
    /// The most bytes of a share's files together.
    public static let totalLimit: Int64 = 1 << 30

    /// Copies `files` from the share folder of request `id` into `destination`, an empty folder of the host's own,
    /// under their names. The copies in the request's order, none without files, or nil when the share is refused: a link, a folder, a FIFO
    /// or a missing file, one over the limit, or two names the file system takes for one; nothing stays copied then.
    public static func copy(dataFolder: URL, id: UInt64, files: [ShareFile], into destination: URL,
                            fileLimit: Int64 = fileLimit, totalLimit: Int64 = totalLimit) -> [URL]? {
        if files.isEmpty { return [] }  // the guest makes no share folder for a share of text alone
        let data = Darwin.open(dataFolder.path, O_RDONLY | O_DIRECTORY | O_CLOEXEC)
        guard data >= 0 else { return nil }
        defer { close(data) }
        var folder = data
        var opened: [Int32] = []
        defer { opened.forEach { close($0) } }
        for component in [".minato", "share", String(id)] {
            let next = openat(folder, component, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
            guard next >= 0 else { return nil }
            opened.append(next)
            folder = next
        }
        let target = Darwin.open(destination.path, O_RDONLY | O_DIRECTORY | O_CLOEXEC)
        guard target >= 0 else { return nil }
        defer { close(target) }
        var made: [String] = []
        var total: Int64 = 0
        for file in files {
            guard let copied = copyFile(file.name, from: folder, to: target, limit: min(fileLimit, totalLimit - total))
            else {
                made.forEach { unlinkat(target, $0, 0) }
                return nil
            }
            made.append(file.name)
            total += copied
        }
        return made.map { destination.appending(path: $0) }
    }

    /// Copies one regular file of one link and at most `limit` bytes; its size, or nil. A clone first, which costs
    /// neither time nor room, else a copy; a name already made in `target` (also one the file system takes for the
    /// same) refuses it, as does a file that grows past the limit meanwhile.
    static func copyFile(_ name: String, from folder: Int32, to target: Int32, limit: Int64) -> Int64? {
        let source = openat(folder, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)
        guard source >= 0 else { return nil }
        defer { close(source) }
        var status = stat()
        guard fstat(source, &status) == 0, status.st_mode & S_IFMT == S_IFREG, status.st_nlink == 1,
              status.st_size <= limit
        else { return nil }
        if fclonefileat(source, target, name, UInt32(CLONE_NOFOLLOW)) == 0 {
            var cloned = stat()
            guard fstatat(target, name, &cloned, AT_SYMLINK_NOFOLLOW) == 0, cloned.st_size <= limit else {
                unlinkat(target, name, 0)
                return nil
            }
            return Int64(cloned.st_size)
        }
        guard errno != EEXIST else { return nil }
        let copy = openat(target, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0o600)
        guard copy >= 0 else { return nil }
        var done: Int64 = 0
        var buffer = [UInt8](repeating: 0, count: 1 << 16)
        let fine: Bool = buffer.withUnsafeMutableBytes { bytes in
            while true {
                let count = read(source, bytes.baseAddress, bytes.count)
                if count == 0 { return true }
                if count < 0 {
                    if errno == EINTR { continue }
                    return false
                }
                done += Int64(count)
                if done > limit { return false }
                var written = 0
                while written < count {
                    let step = write(copy, bytes.baseAddress! + written, count - written)
                    if step < 0 {
                        if errno == EINTR { continue }
                        return false
                    }
                    written += step
                }
            }
        }
        close(copy)
        guard fine else {
            unlinkat(target, name, 0)
            return nil
        }
        return done
    }
}
