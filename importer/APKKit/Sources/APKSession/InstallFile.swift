import Darwin
import Foundation

/// An app installer's handed-over APK (APKRT_REQUEST_CONFIRM_INSTALL): the framework copies the file below the app's
/// cache folder and names its guest path. The host takes it only inside the app's own data folder (the guest's
/// /data/user/0/<package>, which is /data/data/<package>), and reads it without following a link at any level: each
/// folder below the data folder and the file itself are opened with O_NOFOLLOW, the file is checked to be a regular
/// one on the descriptor it is copied from, so nothing the app swaps in between can lead the copy elsewhere.
public enum InstallFile {
    public struct Request: Sendable, Equatable {
        public let path: String
        public let package: String
    }

    public enum Failure: Error, Sendable, Equatable {
        /// A folder or the file could not be opened (a link, a missing one): the errno.
        case open(Int32)
        /// The file is not a regular one.
        case notRegular
        /// Reading the file or writing the copy failed: the errno.
        case copy(Int32)
        /// The file's size is above the limit: the size.
        case tooLarge(Int64)
        /// A component is not a single name.
        case badComponent
    }

    /// Whether `name` is one name of a folder: not empty, not "." or "..", no slash and no NUL, by its bytes (a mark
    /// that joins the slash next to it into one character would hide it from a split by characters).
    static func isName(_ name: String) -> Bool {
        let bytes = Array(name.utf8)
        return !bytes.isEmpty && bytes != [0x2E] && bytes != [0x2E, 0x2E] && !bytes.contains(0x2F)
            && !bytes.contains(0)
    }

    /// "<path>\n<package>", both parts non-empty; nil for any other shape. Split by bytes, as the runtime checked it.
    public static func parse(_ text: String) -> Request? {
        let parts = Array(text.utf8).split(separator: 0x0A, omittingEmptySubsequences: false)
        guard parts.count == 2, !parts[0].isEmpty, !parts[1].isEmpty else { return nil }
        return Request(path: String(decoding: parts[0], as: UTF8.self),
                       package: String(decoding: parts[1], as: UTF8.self))
    }

    /// The components of guest `path` below the app's own data folder, /data/user/0/<package> or /data/data/<package>;
    /// nil for a path elsewhere, the folder itself, or one with an empty, "." or ".." component. By bytes, as the
    /// kernel reads the path.
    public static func components(of path: String, package: String) -> [String]? {
        let bytes = Array(path.utf8)
        for root in ["/data/user/0/\(package)/", "/data/data/\(package)/"] where bytes.starts(with: root.utf8) {
            let components = bytes.dropFirst(root.utf8.count).split(separator: 0x2F, omittingEmptySubsequences: false)
                .map { String(decoding: $0, as: UTF8.self) }
            guard !components.isEmpty, components.allSatisfy(isName) else { return nil }
            return components
        }
        return nil
    }

    /// Copies the regular file at `components` below `folder` to `destination`, which must not exist; a component that
    /// is no single name, a link at any level, a file that is not a regular one, one larger than `limit` bytes (the
    /// room the copy may take) or a failed read throws, and leaves no copy. At most the size fstat gave is read, so a
    /// file that grows meanwhile cannot fill the disk.
    public static func copy(_ components: [String], below folder: URL, to destination: URL, limit: Int64) throws {
        guard let name = components.last else { throw Failure.badComponent }
        guard components.allSatisfy(isName) else { throw Failure.badComponent }
        var directory = open(folder.path, O_RDONLY | O_DIRECTORY | O_CLOEXEC)
        guard directory >= 0 else { throw Failure.open(errno) }
        for component in components.dropLast() {
            let next = openat(directory, component, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
            let error = errno
            close(directory)
            guard next >= 0 else { throw Failure.open(error) }
            directory = next
        }
        // O_NONBLOCK: a FIFO in its place opens at once instead of waiting for a writer, and is refused below.
        let file = openat(directory, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)
        let error = errno
        close(directory)
        guard file >= 0 else { throw Failure.open(error) }
        defer { close(file) }
        var status = stat()
        guard fstat(file, &status) == 0 else { throw Failure.open(errno) }
        guard status.st_mode & S_IFMT == S_IFREG else { throw Failure.notRegular }
        guard status.st_size <= limit else { throw Failure.tooLarge(status.st_size) }
        var remaining = status.st_size
        let target = open(destination.path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0o600)
        guard target >= 0 else { throw Failure.copy(errno) }
        var copied = false
        defer {
            close(target)
            if !copied { unlink(destination.path) }
        }
        var buffer = [UInt8](repeating: 0, count: 1 << 20)
        while remaining > 0 {
            let read = buffer.withUnsafeMutableBytes {
                Darwin.read(file, $0.baseAddress, min($0.count, Int(clamping: remaining)))
            }
            if read < 0, errno == EINTR { continue }
            guard read >= 0 else { throw Failure.copy(errno) }
            if read == 0 { break }
            var written = 0
            while written < read {
                let count = buffer.withUnsafeBytes { write(target, $0.baseAddress! + written, read - written) }
                if count < 0, errno == EINTR { continue }
                guard count > 0 else { throw Failure.copy(errno) }
                written += count
            }
            remaining -= Int64(read)
        }
        copied = true
    }
}
