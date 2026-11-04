import APKKit
import APKTestSupport
import Foundation
import Testing

@testable import APKLibrary

/// ADR-0053, M6 c (E5): compile mode's folder in Caches, which the library takes at its next start, as compile mode
/// cannot sign: the odex of a compile that ended with 0, a failure for one that failed and for one that iOS ended the
/// second time, nothing for one the user cancelled or one made for another boot class path.
@Suite("Compile work")
struct CompileWorkTests {
    typealias Sandbox = LibraryTests.Sandbox

    static func work(_ sandbox: Sandbox, _ entry: LibraryEntry, status: String?, odex: Bool = true,
                     bootClassPath: String = "bcp-1") throws -> CompileWork {
        let work = CompileWork(caches: sandbox.paths.caches, package: entry.id)
        try work.prepare(bootClassPath: bootClassPath)
        if odex {
            try Data("ODEX".utf8).write(to: work.folder.appendingPathComponent("data/local/tmp/base.odex"))
            try Data("VDEX".utf8).write(to: work.folder.appendingPathComponent("data/local/tmp/base.vdex"))
        }
        if let status { try Data(status.utf8).write(to: work.statusFile) }
        return work
    }

    @Test func aFreshFolderHasDex2oatsDataFolders() throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let work = CompileWork(caches: sandbox.paths.caches, package: "org.example.app")
        try FileManager.default.createDirectory(at: work.folder, withIntermediateDirectories: true)
        try Data("old".utf8).write(to: work.folder.appendingPathComponent("base.odex"))

        try work.prepare(bootClassPath: "bcp-1")

        #expect(work.bootClassPath == "bcp-1")

        #expect(sandbox.exists(work.folder.appendingPathComponent("data/dalvik-cache/arm64")))
        #expect(sandbox.exists(work.folder.appendingPathComponent("data/local/tmp")))
        #expect(!sandbox.exists(work.folder.appendingPathComponent("base.odex")))
        #expect(work.folder.path.hasPrefix(sandbox.paths.caches.path))
    }

    @Test(arguments: [
        ("0\n", CompileWork.Outcome.compiled),
        ("1\n", .failed),
        ("137\n", .failed),  // a kill, also of the process
        ("", .endedByIOS),  // nothing written: iOS ended the process, as for using too much memory
        (nil, .endedByIOS),
    ] as [(String?, CompileWork.Outcome)])
    func theOutcomeIsTheExitStatus(status: String?, outcome: CompileWork.Outcome) throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let work = CompileWork(caches: sandbox.paths.caches, package: "org.example.app")
        try work.prepare(bootClassPath: "bcp-1")
        if let status { try Data(status.utf8).write(to: work.statusFile) }

        #expect(work.outcome == outcome)
    }

    @Test func aCancelledCompileIsCancelledWhateverItsStatus() throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let work = CompileWork(caches: sandbox.paths.caches, package: "org.example.app")
        try work.prepare(bootClassPath: "bcp-1")

        try work.cancel()

        #expect(work.outcome == .cancelled)
    }

    @Test func theLibraryKeepsTheCodeOfACompileThatEndedWithZero() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try await OatCodeTests.javaApp(sandbox, library)
        let work = try Self.work(sandbox, entry, status: "0\n")

        try await library.takeCompileWork(bootClassPath: "bcp-1", converter: NativeCodeTests.Converter())

        #expect(await library.oatFolders(id: entry.id, bootClassPath: "bcp-1") != nil)
        #expect(!sandbox.exists(work.folder))
    }

    @Test(arguments: [("1\n", true), ("0\n", false)])
    func aCompileThatFailedIsRecordedAndNotKept(status: String, odex: Bool) async throws {
        // "0" without base.vdex: dex2oat said it ended well but left no whole oat.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try await OatCodeTests.javaApp(sandbox, library)
        let work = try Self.work(sandbox, entry, status: status, odex: odex)

        try await library.takeCompileWork(bootClassPath: "bcp-1", converter: NativeCodeTests.Converter())

        let taken = try #require(await library.entries.first)
        #expect(taken.compiled == nil)
        #expect(taken.compileFailed == "bcp-1")
        #expect(!sandbox.exists(work.folder))
    }

    @Test func aCancelledCompileRecordsNothingSoItIsAskedAgain() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try await OatCodeTests.javaApp(sandbox, library)
        let work = try Self.work(sandbox, entry, status: nil)
        try work.cancel()

        try await library.takeCompileWork(bootClassPath: "bcp-1", converter: NativeCodeTests.Converter())

        let taken = try #require(await library.entries.first)
        #expect(taken.compileFailed == nil)
        #expect(sandbox.paths.needsCompiling(taken, bootClassPath: "bcp-1"))
        #expect(!sandbox.exists(work.folder))
    }

    @Test func withoutASignerACompiledOutputWaits() async throws {
        // Nothing signs yet (the certificate's first look has not ended): the odex stays for a later start.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try await OatCodeTests.javaApp(sandbox, library)
        let work = try Self.work(sandbox, entry, status: "0\n")

        try await library.takeCompileWork(bootClassPath: "bcp-1", converter: nil)

        #expect(sandbox.exists(work.folder.appendingPathComponent("data/local/tmp/base.odex")))
        #expect(await library.entries.first?.compileFailed == nil)
    }

    @Test func theWorkOfAnAppNoLongerThereGoes() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let work = CompileWork(caches: sandbox.paths.caches, package: "org.example.gone")
        try work.prepare(bootClassPath: "bcp-1")
        try Data("0\n".utf8).write(to: work.statusFile)

        try await library.takeCompileWork(bootClassPath: "bcp-1", converter: NativeCodeTests.Converter())

        #expect(!sandbox.exists(work.folder))
    }

    @Test func aCompileThatIOSEndedIsRecordedOnlyTheSecondTime() async throws {
        // One end by iOS (its memory, or a swipe away while suspended) tries again after the next session; a second for
        // the same version and boot class path stops it, so that it cannot loop.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try await OatCodeTests.javaApp(sandbox, library)

        _ = try Self.work(sandbox, entry, status: nil, odex: false)
        try await library.takeCompileWork(bootClassPath: "bcp-1", converter: NativeCodeTests.Converter())

        let once = try #require(await library.entries.first)
        #expect(once.compileFailed == nil)
        #expect(once.compileInterrupted == CompileInterruption(bootClassPath: "bcp-1", count: 1))
        #expect(sandbox.paths.needsCompiling(once, bootClassPath: "bcp-1"))

        _ = try Self.work(sandbox, entry, status: nil, odex: false)
        try await library.takeCompileWork(bootClassPath: "bcp-1", converter: NativeCodeTests.Converter())

        let twice = try #require(await library.entries.first)
        #expect(twice.compileFailed == "bcp-1")
        #expect(!sandbox.paths.needsCompiling(twice, bootClassPath: "bcp-1"))
    }

    @Test func anEndForAnotherBootClassPathCountsAgainFromOne() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try await OatCodeTests.javaApp(sandbox, library)
        _ = try Self.work(sandbox, entry, status: nil, odex: false, bootClassPath: "bcp-1")
        try await library.takeCompileWork(bootClassPath: "bcp-1", converter: NativeCodeTests.Converter())

        _ = try Self.work(sandbox, entry, status: nil, odex: false, bootClassPath: "bcp-2")
        try await library.takeCompileWork(bootClassPath: "bcp-2", converter: NativeCodeTests.Converter())

        let taken = try #require(await library.entries.first)
        #expect(taken.compileFailed == nil)
        #expect(taken.compileInterrupted == CompileInterruption(bootClassPath: "bcp-2", count: 1))
    }

    @Test(arguments: ["0\n", "1\n", ""])
    func aCompileForAnotherBootClassPathIsDroppedWithoutARecord(status: String) async throws {
        // Made by an earlier Minato, whose framework this one does not run: its odex is stale, and its end is not
        // this framework's.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try await OatCodeTests.javaApp(sandbox, library)
        let work = try Self.work(sandbox, entry, status: status, bootClassPath: "bcp-0")

        try await library.takeCompileWork(bootClassPath: "bcp-1", converter: NativeCodeTests.Converter())

        let taken = try #require(await library.entries.first)
        #expect(taken.compiled == nil)
        #expect(taken.compileFailed == nil)
        #expect(taken.compileInterrupted == nil)
        #expect(!sandbox.exists(work.folder))
    }

    @Test func compiledCodeClearsTheCountOfEnds() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try await OatCodeTests.javaApp(sandbox, library)
        _ = try Self.work(sandbox, entry, status: nil, odex: false)
        try await library.takeCompileWork(bootClassPath: "bcp-1", converter: NativeCodeTests.Converter())

        _ = try Self.work(sandbox, entry, status: "0\n")
        try await library.takeCompileWork(bootClassPath: "bcp-1", converter: NativeCodeTests.Converter())

        let taken = try #require(await library.entries.first)
        #expect(taken.compiled?.bootClassPath == "bcp-1")
        #expect(taken.compileInterrupted == nil)
    }
}
