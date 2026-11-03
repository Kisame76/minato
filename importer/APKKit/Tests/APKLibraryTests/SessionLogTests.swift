import Foundation
import Testing

@testable import APKLibrary

/// ADR-0070, the App's part 3: game mode keeps the last 200 lines of a session's guest log in
/// Library/Caches/session-logs/<package>.txt, at the session's end and, after a crash, at the next start.
@Suite("Session log")
struct SessionLogTests {
    let folder: URL
    let log: SessionLog

    init() throws {
        folder = FileManager.default.temporaryDirectory.appending(path: "session-logs-\(UUID().uuidString)")
        log = SessionLog(folder: folder)
    }

    func lines(_ range: ClosedRange<Int>) -> String {
        range.map { "I/tag: line \($0)\n" }.joined()
    }

    func kept(_ package: String) throws -> String {
        try String(contentsOf: log.keptFile(for: package), encoding: .utf8)
    }

    func files() -> [String] {
        ((try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? []).sorted()
    }

    @Test func theSessionsEndKeepsItsLast200Lines() throws {
        let writer = try log.writer(for: "org.example.app")
        writer.append(Data(lines(1...250).utf8))
        writer.close()
        try log.keep(package: "org.example.app")
        #expect(try kept("org.example.app") == lines(51...250))
        #expect(files() == ["org.example.app.txt"])
    }

    @Test func anUnfinishedLastLineIsKeptToo() throws {
        let writer = try log.writer(for: "org.example.app")
        writer.append(Data((lines(1...3) + "F/libc: Fatal signal 11").utf8))
        writer.close()
        try log.keep(package: "org.example.app")
        #expect(try kept("org.example.app") == lines(1...3) + "F/libc: Fatal signal 11\n")
    }

    @Test func aLongSessionRotatesItsFileAndStillKeepsTheLastLines() throws {
        let small = SessionLog(folder: folder, rotateBytes: 1000)
        let writer = try small.writer(for: "org.example.app")
        for line in 1...500 { writer.append(Data("I/tag: line \(line)\n".utf8)) }
        writer.close()
        // Never more than the current file and the one before it.
        let sizes = try files().map {
            try FileManager.default.attributesOfItem(atPath: folder.appending(path: $0).path)[.size] as? Int ?? 0
        }
        #expect(sizes.allSatisfy { $0 <= 1000 + 20 } && sizes.count <= 2)
        try small.keep(package: "org.example.app")
        let tail = try kept("org.example.app")
        #expect(tail.hasSuffix(lines(491...500)))
        #expect(tail.split(separator: "\n").count <= SessionLog.keptLines)
    }

    @Test func aCrashedSessionsLogIsKeptAtTheNextStart() throws {
        let writer = try log.writer(for: "org.example.crashed")
        writer.append(Data(lines(1...5).utf8))
        // No close and no keep: the process ended.
        try log.keepLeftovers()
        #expect(try kept("org.example.crashed") == lines(1...5))
        #expect(files() == ["org.example.crashed.txt"])
    }

    @Test func aNewSessionKeepsThePreviousLogUntilItsOwnEnd() throws {
        let first = try log.writer(for: "org.example.app")
        first.append(Data(lines(1...2).utf8))
        first.close()
        try log.keep(package: "org.example.app")
        let second = try log.writer(for: "org.example.app")
        second.append(Data(lines(3...4).utf8))
        #expect(try kept("org.example.app") == lines(1...2))
        second.close()
        try log.keep(package: "org.example.app")
        #expect(try kept("org.example.app") == lines(3...4))
    }

    @Test func clearingOrDeletingTheAppRemovesItsLog() throws {
        let writer = try log.writer(for: "org.example.app")
        writer.append(Data(lines(1...2).utf8))
        writer.close()
        try log.keep(package: "org.example.app")
        let other = try log.writer(for: "org.example.other")
        other.append(Data(lines(1...2).utf8))
        other.close()
        try log.keep(package: "org.example.other")
        log.remove(package: "org.example.app")
        #expect(files() == ["org.example.other.txt"])
    }
}
