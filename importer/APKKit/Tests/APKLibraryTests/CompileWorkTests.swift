import APKKit
import APKTestSupport
import Foundation
import Testing

@testable import APKLibrary

/// ADR-0053, M6 c (E5): compile mode's folder in Caches, which the library takes at its next start, as compile mode
/// cannot sign: the odex of a compile that ended with 0, a failure for one that failed or that iOS ended, nothing for
/// one the user cancelled.
@Suite("Compile work")
struct CompileWorkTests {
    typealias Sandbox = LibraryTests.Sandbox

    static func work(_ sandbox: Sandbox, _ entry: LibraryEntry, status: String?,
                     odex: Bool = true) throws -> CompileWork {
        let work = CompileWork(caches: sandbox.paths.caches, package: entry.id)
        try work.prepare()
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

        try work.prepare()

        #expect(sandbox.exists(work.folder.appendingPathComponent("data/dalvik-cache/arm64")))
        #expect(sandbox.exists(work.folder.appendingPathComponent("data/local/tmp")))
        #expect(!sandbox.exists(work.folder.appendingPathComponent("base.odex")))
        #expect(work.folder.path.hasPrefix(sandbox.paths.caches.path))
    }

    @Test(arguments: [
        ("0\n", CompileWork.Outcome.compiled),
        ("1\n", .failed),
        ("137\n", .failed),  // a kill, also of the process
        ("", .failed),  // nothing written: iOS ended the process, as for using too much memory
    ])
    func theOutcomeIsTheExitStatus(status: String, outcome: CompileWork.Outcome) throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let work = CompileWork(caches: sandbox.paths.caches, package: "org.example.app")
        try work.prepare()
        try Data(status.utf8).write(to: work.statusFile)

        #expect(work.outcome == outcome)
    }

    @Test func aCancelledCompileIsCancelledWhateverItsStatus() throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let work = CompileWork(caches: sandbox.paths.caches, package: "org.example.app")
        try work.prepare()

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

    @Test(arguments: [("1\n", true), ("", true), ("0\n", false)])
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
        try work.prepare()
        try Data("0\n".utf8).write(to: work.statusFile)

        try await library.takeCompileWork(bootClassPath: "bcp-1", converter: NativeCodeTests.Converter())

        #expect(!sandbox.exists(work.folder))
    }
}
