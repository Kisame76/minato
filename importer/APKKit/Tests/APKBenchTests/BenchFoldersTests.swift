import Foundation
import Testing

@testable import APKBench

/// The run folders under the app's tmp/benchmark: a run removes the others at its start, as nothing else deletes them
/// on the iPhone (about 10,600 entries in tmp, 2026-11-11), and keeps its own.
@Suite("BenchFolders")
struct BenchFoldersTests {
    static func folder(_ base: URL, _ name: String, files: Int = 2) throws {
        let folder = base.appending(path: name, directoryHint: .isDirectory)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        for index in 0..<files {
            try Data("frame \(index)\n".utf8).write(to: folder.appending(path: "frames-\(index).csv"))
        }
    }

    @Test func aRunRemovesTheOtherRunsFoldersAndKeepsItsOwn() throws {
        let base = FileManager.default.temporaryDirectory.appending(path: "bench-folders-\(UUID())")
        defer { try? FileManager.default.removeItem(at: base) }
        try Self.folder(base, "vulkan-120-heavy-apk-01")
        try Self.folder(base, "vulkan-120-heavy-apk-02")
        try Self.folder(base, "session-io.github.kisame76.minato.test.example")
        try Data("stray".utf8).write(to: base.appending(path: "stray.txt"))

        let removed = BenchFolders.removeOthers(in: base, keeping: "vulkan-120-heavy-apk-02")

        #expect(removed == 3)
        let left = try FileManager.default.contentsOfDirectory(atPath: base.path)
        #expect(left == ["vulkan-120-heavy-apk-02"])
        let kept = try FileManager.default.contentsOfDirectory(atPath: base.appending(path: left[0]).path)
        #expect(kept.sorted() == ["frames-0.csv", "frames-1.csv"])
    }

    @Test func aMissingFolderRemovesNothing() {
        let base = FileManager.default.temporaryDirectory.appending(path: "bench-folders-absent-\(UUID())")
        #expect(BenchFolders.removeOthers(in: base, keeping: "gles-60-light-native-01") == 0)
    }
}
