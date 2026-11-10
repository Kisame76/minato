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
    }

    /// "<path>\n<package>", both parts non-empty; nil for any other shape.
    public static func parse(_ text: String) -> Request? {
        let parts = text.split(separator: "\n", omittingEmptySubsequences: false)
        guard parts.count == 2, !parts[0].isEmpty, !parts[1].isEmpty else { return nil }
        return Request(path: String(parts[0]), package: String(parts[1]))
    }

    /// The components of guest `path` below the app's own data folder, /data/user/0/<package> or /data/data/<package>;
    /// nil for a path elsewhere, the folder itself, or one with an empty, "." or ".." component.
    public static func components(of path: String, package: String) -> [String]? {
        for root in ["/data/user/0/\(package)/", "/data/data/\(package)/"] where path.hasPrefix(root) {
            let components = path.dropFirst(root.count).split(separator: "/", omittingEmptySubsequences: false)
                .map(String.init)
            guard !components.isEmpty, components.allSatisfy({ !$0.isEmpty && $0 != "." && $0 != ".." }) else {
                return nil
            }
            return components
        }
        return nil
    }

    /// Copies the regular file at `components` below `folder` to `destination`, which must not exist; a link at any
    /// level, a file that is not a regular one or a failed read throws, and leaves no copy.
    public static func copy(_ components: [String], below folder: URL, to destination: URL) throws {
        guard let name = components.last else { throw Failure.open(ENOENT) }
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
        let target = open(destination.path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0o600)
        guard target >= 0 else { throw Failure.copy(errno) }
        var copied = false
        defer {
            close(target)
            if !copied { unlink(destination.path) }
        }
        var buffer = [UInt8](repeating: 0, count: 1 << 20)
        while true {
            let read = buffer.withUnsafeMutableBytes { Darwin.read(file, $0.baseAddress, $0.count) }
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
        }
        copied = true
    }
}
