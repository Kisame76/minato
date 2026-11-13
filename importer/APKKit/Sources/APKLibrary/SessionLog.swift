import Darwin
import Foundation

/// The kept session log (ADR-0070, part 3): game mode writes the session's guest log to
/// `<package>.log` in Library/Caches/session-logs, and keeps its last 200 lines in `<package>.txt` at the session's
/// end and, after a crash, at the next start, for a report. The raw file is rotated to `<package>.log.old` past
/// `rotateBytes`, so a talkative app fills at most two of them. Minato's own lines (`ownTag`) outlast the 200.
/// Clearing the app's data or deleting the app removes its log. Caches is no backup's, and iOS may empty it: the log
/// is a help, not a record.
public struct SessionLog: Sendable {
    public static let keptLines = 200
    /// The tag of Minato's own lines in the log (the background trips' times and memory, BackgroundTrace): the latest
    /// `ownLinesKept` of them stay in the kept file before its last lines, also once the guest's lines pushed them out.
    public static let ownTag = "I/minato   : "
    public static let ownLinesKept = 40

    public let folder: URL
    let rotateBytes: Int

    public init(folder: URL, rotateBytes: Int = 1 << 20) {
        self.folder = folder
        self.rotateBytes = rotateBytes
    }

    /// The last session's kept lines.
    public func keptFile(for package: String) -> URL {
        folder.appending(path: "\(package).txt")
    }

    func rawFile(for package: String) -> URL {
        folder.appending(path: "\(package).log")
    }

    func oldFile(for package: String) -> URL {
        folder.appending(path: "\(package).log.old")
    }

    /// A new session's raw log, empty; the previous session's kept lines stay until this one's end.
    public func writer(for package: String) throws -> Writer {
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        try? FileManager.default.removeItem(at: oldFile(for: package))
        return try Writer(file: rawFile(for: package), old: oldFile(for: package), rotateBytes: rotateBytes)
    }

    /// The session ended: its last lines replace the kept ones, and the raw files go. Nothing without a raw file.
    public func keep(package: String) throws {
        let parts = [oldFile(for: package), rawFile(for: package)].compactMap { try? Data(contentsOf: $0) }
        guard !parts.isEmpty else { return }
        var lines = String(decoding: parts.reduce(Data(), +), as: UTF8.self)
            .split(separator: "\n", omittingEmptySubsequences: false)
        if lines.last?.isEmpty == true { lines.removeLast() }
        let own = lines.dropLast(Self.keptLines).filter { $0.hasPrefix(Self.ownTag) }.suffix(Self.ownLinesKept)
        let tail = (own + lines.suffix(Self.keptLines)).map { $0 + "\n" }.joined()
        try Data(tail.utf8).write(to: keptFile(for: package), options: .atomic)
        try? FileManager.default.removeItem(at: oldFile(for: package))
        try? FileManager.default.removeItem(at: rawFile(for: package))
    }

    /// At the start after a crash, which ran no end: every raw log left is kept.
    public func keepLeftovers() throws {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? []
        let packages = Set(names.compactMap { name -> String? in
            if name.hasSuffix(".log.old") { return String(name.dropLast(".log.old".count)) }
            if name.hasSuffix(".log") { return String(name.dropLast(".log".count)) }
            return nil
        })
        for package in packages.sorted() { try keep(package: package) }
    }

    /// Clear App Data and Delete: the app's log goes, kept and raw.
    public func remove(package: String) {
        for file in [keptFile(for: package), rawFile(for: package), oldFile(for: package)] {
            try? FileManager.default.removeItem(at: file)
        }
    }

    /// Appends the guest's log as it comes, from any one thread, and rotates the file past `rotateBytes`.
    public final class Writer: @unchecked Sendable {
        private let file: URL
        private let old: URL
        private let rotateBytes: Int
        private let lock = NSLock()
        private var descriptor: Int32
        private var written = 0

        init(file: URL, old: URL, rotateBytes: Int) throws {
            self.file = file
            self.old = old
            self.rotateBytes = rotateBytes
            descriptor = Self.open(file)
            guard descriptor >= 0 else { throw CocoaError(.fileWriteUnknown, userInfo: [NSURLErrorKey: file]) }
        }

        deinit {
            close()
        }

        private static func open(_ file: URL) -> Int32 {
            Darwin.open(file.path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0o600)
        }

        public func append(_ data: Data) {
            lock.lock()
            defer { lock.unlock() }
            guard descriptor >= 0, !data.isEmpty else { return }
            if written > 0, written + data.count > rotateBytes {
                Darwin.close(descriptor)
                _ = rename(file.path, old.path)
                descriptor = Self.open(file)
                written = 0
                guard descriptor >= 0 else { return }
            }
            data.withUnsafeBytes { bytes in
                var offset = 0
                while offset < bytes.count {
                    let count = write(descriptor, bytes.baseAddress! + offset, bytes.count - offset)
                    if count <= 0 { break }
                    offset += count
                }
            }
            written += data.count
        }

        public func close() {
            lock.lock()
            defer { lock.unlock() }
            if descriptor >= 0 { Darwin.close(descriptor) }
            descriptor = -1
        }
    }
}
