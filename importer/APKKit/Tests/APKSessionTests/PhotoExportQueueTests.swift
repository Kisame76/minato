import Darwin
import Foundation
import Synchronization
import Testing

@testable import APKSession

/// ADR-0057 step e: the queue that copies each taken file into the host's folder and hands Photos one copy at a time
/// (the app's PhotoKit adds them), with Photos' answer standing in.
@Suite("PhotoExportQueue")
final class PhotoExportQueueTests {
    let root: URL
    let data: URL
    let folder: URL

    init() throws {
        root = FileManager.default.temporaryDirectory.appending(path: "photo-export-queue-\(UUID().uuidString)")
        data = root.appending(path: "data")
        folder = root.appending(path: "photo-exports")
        try FileManager.default.createDirectory(at: data, withIntermediateDirectories: true)
    }

    deinit { try? FileManager.default.removeItem(at: root) }

    /// What Photos was given, and how many at once at most.
    final class Photos: Sendable {
        struct State {
            var added: [(kind: MediaExportJob.Kind, content: String, name: String)] = []
            var running = 0
            var most = 0
        }

        let state = Mutex(State())
        let answer: PhotoExportQueue.Outcome
        let delay: Duration

        init(answer: PhotoExportQueue.Outcome = .added, delay: Duration = .milliseconds(20)) {
            self.answer = answer
            self.delay = delay
        }

        func add(_ kind: MediaExportJob.Kind, _ copy: URL, _ name: String) async -> PhotoExportQueue.Outcome {
            let content = (try? String(contentsOf: copy, encoding: .utf8)) ?? "<none>"
            state.withLock {
                $0.running += 1
                $0.most = max($0.most, $0.running)
            }
            try? await Task.sleep(for: delay)
            state.withLock {
                $0.running -= 1
                $0.added.append((kind, content, name))
            }
            return answer
        }
    }

    func job(_ name: String, _ text: String, kind: String = "1") throws -> MediaExportJob {
        try Data(text.utf8).write(to: data.appending(path: name))
        return try #require(MediaExports(dataFolder: data).take("\(kind)\t\(name)"))
    }

    func queue(_ photos: Photos, declined: Bool = false) -> PhotoExportQueue {
        PhotoExportQueue(folder: folder, declined: declined) { await photos.add($0, $1, $2) }
    }

    func copies() -> [String] { ((try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? []).sorted() }

    @Test func eachExportReachesPhotosOnceAndItsCopyGoes() async throws {
        let photos = Photos()
        let queue = queue(photos)
        queue.export(try job("a.png", "first"))
        queue.export(try job("b.mp4", "second", kind: "2"))
        let clock = ContinuousClock()
        let start = clock.now
        await queue.settle(within: 5)
        #expect(clock.now - start < .seconds(3))  // done when both are, not at the limit
        let added = photos.state.withLock { $0.added }
        #expect(added.map(\.content).sorted() == ["first", "second"])
        #expect(added.first { $0.content == "second" }?.kind == .video)
        #expect(added.first { $0.content == "first" }?.name == "a.png")
        #expect(copies().isEmpty)
        #expect(photos.state.withLock { $0.most } == 1)  // one at a time
    }

    @Test func aRefusedCopyGoesToo() async throws {
        let photos = Photos(answer: .refused)
        let queue = queue(photos)
        queue.export(try job("a.png", "not a picture"))
        await queue.settle(within: 5)
        #expect(photos.state.withLock { $0.added.count } == 1)
        #expect(copies().isEmpty)
        #expect(!queue.declined)
    }

    @Test func aDeclineIsKeptAndTheCopyGoes() async throws {
        let photos = Photos(answer: .declined)
        let queue = queue(photos)
        #expect(!queue.declined)
        queue.export(try job("a.png", "a"))
        await queue.settle(within: 5)
        #expect(queue.declined)
        #expect(copies().isEmpty)
        #expect(self.queue(Photos(), declined: true).declined)
    }

    @Test func settleWaitsForThoseUnderWayAndNoLonger() async throws {
        let slow = Photos(delay: .seconds(30))
        let queue = queue(slow)
        queue.export(try job("a.png", "a"))
        let clock = ContinuousClock()
        let start = clock.now
        await queue.settle(within: 0.3)
        let waited = clock.now - start
        #expect(waited >= .milliseconds(250) && waited < .seconds(5))
        let quick = self.queue(Photos())
        let begun = clock.now
        await quick.settle(within: 5)  // nothing under way
        #expect(clock.now - begun < .seconds(1))
    }

    @Test func aSecondQueueKeepsTheCopiesOfTheFirst() throws {
        try FileManager.default.createDirectory(at: folder.appending(path: "under-way"),
                                                withIntermediateDirectories: true)
        _ = queue(Photos())
        _ = queue(Photos())
        #expect(copies() == ["under-way"])
    }

    @Test func theCopiesOfAnEarlierLaunchGoAtTheNext() throws {
        try FileManager.default.createDirectory(at: folder.appending(path: "left"), withIntermediateDirectories: true)
        try Data("4 GiB".utf8).write(to: folder.appending(path: "left/big.mp4"))
        PhotoExportQueue.removeEarlierCopies(folder)
        #expect(!FileManager.default.fileExists(atPath: folder.path))
        // The removal runs in the background, under another name.
        let clock = ContinuousClock()
        let deadline = clock.now + .seconds(5)
        func left() -> [String] {
            let names = (try? FileManager.default.contentsOfDirectory(atPath: root.path)) ?? []
            return names.filter { $0.hasPrefix("photo-exports") }
        }
        while !left().isEmpty && clock.now < deadline { usleep(20_000) }
        #expect(left().isEmpty)
        _ = queue(Photos())
        #expect(FileManager.default.fileExists(atPath: folder.path))
    }
}
