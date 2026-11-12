import APKLibrary
import APKRuntime
import Darwin
import Foundation
import Synchronization

/// Game mode's part of the kept session log (ADR-0070, the App's part 3): the guest's log, its stderr in logcat's tag
/// format, which the runtime hands it as descriptor 2, goes through a pipe that a thread of its own copies to the
/// session's raw log and on to where descriptor 2 went before, the debugger's console or session-check's file. At the
/// program's end, a crash's too, the runtime's log drain lets the thread copy what is left; at the session's end the
/// last 200 lines are kept, after a crash at the library's next start.
enum SessionLogTap {
    private static let lock = NSLock()
    nonisolated(unsafe) private static var package: String?
    nonisolated(unsafe) private static var log: SessionLog?
    nonisolated(unsafe) private static var writer: SessionLog.Writer?
    /// Where descriptor 2 went before the tap, -1 without one.
    private static let original = Atomic<Int32>(-1)
    /// The pipe's read end, while the thread reads it.
    private static let reading = Atomic<Int32>(-1)
    /// The thread has bytes from the pipe that it has not written out yet.
    private static let copying = Atomic<Bool>(false)
    /// The thread has copied the pipe to its end.
    private static let copied = Atomic<Bool>(false)

    /// Before the program starts.
    static func begin(package: String) {
        lock.lock()
        defer { lock.unlock() }
        guard writer == nil, let paths = try? LibraryPaths.standard() else { return }
        let log = SessionLog(folder: paths.sessionLogDirectory)
        try? log.keepLeftovers()
        var ends: [Int32] = [-1, -1]
        guard let writer = try? log.writer(for: package), pipe(&ends) == 0 else { return }
        _ = fcntl(ends[0], F_SETFD, FD_CLOEXEC)
        _ = fcntl(ends[1], F_SETFD, FD_CLOEXEC)
        let before = dup(2)
        original.store(before, ordering: .releasing)
        copied.store(false, ordering: .releasing)
        reading.store(ends[0], ordering: .releasing)
        dup2(ends[1], 2)
        close(ends[1])
        self.package = package
        self.log = log
        self.writer = writer
        let readEnd = ends[0]
        let reader = Thread { copy(from: readEnd, to: writer, tee: before) }
        reader.name = "MinatoSessionLog"
        reader.start()
        apkrt_drain_log_at_end { SessionLogTap.drain() }
    }

    /// The session ends through game mode: once the thread has copied what was in the pipe, the last lines are kept.
    static func end() {
        lock.lock()
        defer { lock.unlock() }
        guard let package, let log, let writer else { return }
        apkrt_drain_log_at_end(nil)
        // The thread writes to the original until the pipe's end, which another write end may hold off: then it
        // stays open.
        let before = original.load(ordering: .acquiring)
        drain()
        if copied.load(ordering: .acquiring) { close(before) }
        original.store(-1, ordering: .releasing)
        writer.close()
        try? log.keep(package: package)
        self.writer = nil
    }

    /// One of Minato's own lines (SessionLog.ownTag) into the session log, in one write from any thread, as the
    /// runtime's memory lines go, so that a line written stays when iOS ends the process.
    static func own(_ line: String) {
        var text = line + "\n"
        text.withUTF8 { _ = write(2, $0.baseAddress, $0.count) }
    }

    /// Descriptor 2 goes back, and the thread copies what is in the pipe, 1 s at most: until the pipe's end, or until
    /// for 10 ms the pipe has been empty and the thread has written out what it read. The runtime keeps a write end of
    /// its own, so the end may not come. The runtime calls this at the program's end, a crash's in its signal
    /// handler: so only dup2, fstat, nanosleep and atomics.
    private static func drain() {
        let before = original.load(ordering: .acquiring)
        if before >= 0 { dup2(before, 2) }
        var millisecond = timespec(tv_sec: 0, tv_nsec: 1_000_000)
        var quiet = 0
        for _ in 0..<1000 {
            if copied.load(ordering: .acquiring) || quiet >= 10 { return }
            // A pipe's size is the bytes in it.
            var status = stat()
            let empty = fstat(reading.load(ordering: .acquiring), &status) == 0 && status.st_size == 0
            quiet = empty && !copying.load(ordering: .acquiring) ? quiet + 1 : 0
            nanosleep(&millisecond, nil)
        }
    }

    private static func copy(from pipe: Int32, to writer: SessionLog.Writer, tee: Int32) {
        var buffer = [UInt8](repeating: 0, count: 16 * 1024)
        while true {
            let count = buffer.withUnsafeMutableBytes { read(pipe, $0.baseAddress, $0.count) }
            if count < 0, errno == EINTR { continue }
            guard count > 0 else { break }
            copying.store(true, ordering: .releasing)
            writer.append(Data(buffer[0..<count]))
            if tee >= 0 { _ = buffer.withUnsafeBytes { write(tee, $0.baseAddress, count) } }
            copying.store(false, ordering: .releasing)
        }
        copied.store(true, ordering: .releasing)
        reading.store(-1, ordering: .releasing)
        close(pipe)
    }
}
