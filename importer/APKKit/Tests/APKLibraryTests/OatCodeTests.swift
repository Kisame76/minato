import APKKit
import APKTestSupport
import Foundation
import Testing

@testable import APKLibrary

/// ADR-0053, M6 c: the library keeps the oat that dex2oat made for an app, as its signed container beside it, and says
/// whether it is still the one for the boot class path the runtime starts with.
@Suite("Compiled code in the library")
struct OatCodeTests {
    typealias Sandbox = LibraryTests.Sandbox

    static func installed(_ sandbox: Sandbox, _ library: Library) async throws -> LibraryEntry {
        let file = try sandbox.file(SampleAPK.make(nativeActivity: true), named: "game.apk")
        return try await library.importFile(file, signing: nil).entry
    }

    /// dex2oat's two files for the app, where the compile start left them.
    static func compiled(_ sandbox: Sandbox) throws -> (odex: URL, vdex: URL) {
        let odex = sandbox.root.appendingPathComponent("made.odex")
        let vdex = sandbox.root.appendingPathComponent("made.vdex")
        try Data("ODEX".utf8).write(to: odex)
        try Data("VDEX".utf8).write(to: vdex)
        return (odex, vdex)
    }

    @Test func keepsTheOdexTheVdexAndTheSignedContainer() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try await Self.installed(sandbox, library)
        let made = try Self.compiled(sandbox)

        try await library.installCompiledCode(
            id: entry.id, odex: made.odex, vdex: made.vdex, bootClassPath: "bcp-1",
            converter: NativeCodeTests.Converter())

        let code = sandbox.paths.oatDirectory(for: entry), containers = sandbox.paths.oatContainerDirectory(for: entry)
        #expect(try Data(contentsOf: code.appendingPathComponent("base.odex")) == Data("ODEX".utf8))
        #expect(try Data(contentsOf: code.appendingPathComponent("base.vdex")) == Data("VDEX".utf8))
        #expect(try Data(contentsOf: containers.appendingPathComponent("base.odex.dylib")) == Data("aa:ODEX".utf8))
        #expect(await library.entries.first?.compiled == CompiledCode(bootClassPath: "bcp-1", containerVersion: 3))
        // It survives a relaunch.
        #expect(try await Library(paths: sandbox.paths).entries.first?.compiled?.bootClassPath == "bcp-1")
    }

    @Test func theFoldersAreForTheBootClassPathItWasMadeFor() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try await Self.installed(sandbox, library)
        let made = try Self.compiled(sandbox)
        #expect(await library.oatFolders(id: entry.id, bootClassPath: "bcp-1") == nil)  // nothing compiled yet

        try await library.installCompiledCode(
            id: entry.id, odex: made.odex, vdex: made.vdex, bootClassPath: "bcp-1",
            converter: NativeCodeTests.Converter())

        let folders = await library.oatFolders(id: entry.id, bootClassPath: "bcp-1")
        #expect(folders?.code == sandbox.paths.oatDirectory(for: entry))
        #expect(folders?.containers == sandbox.paths.oatContainerDirectory(for: entry))
        // A framework change moves the boot class path: the odex is stale, not used (ART would reject it anyway).
        #expect(await library.oatFolders(id: entry.id, bootClassPath: "bcp-2") == nil)
    }

    @Test func aRefusedContainerLeavesNothingBehind() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try await Self.installed(sandbox, library)
        let made = try Self.compiled(sandbox)
        var converter = NativeCodeTests.Converter()
        converter.refuses = "made.odex"

        await #expect(throws: ConversionRefused.self) {
            try await library.installCompiledCode(
                id: entry.id, odex: made.odex, vdex: made.vdex, bootClassPath: "bcp-1", converter: converter)
        }

        #expect(await library.entries.first?.compiled == nil)
        #expect(!sandbox.exists(sandbox.paths.oatDirectory(for: entry)))
        #expect(!sandbox.exists(sandbox.paths.oatContainerDirectory(for: entry)))
    }

    @Test func adoptsTheOutputOfACompileStartAndRemovesIt() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try await Self.installed(sandbox, library)
        let made = try Self.compiled(sandbox)
        let output = sandbox.root.appendingPathComponent("oat.work")
        // dex2oat's /data/local/tmp, where the compile start has it write.
        let written = output.appendingPathComponent("data/local/tmp")
        try FileManager.default.createDirectory(at: written, withIntermediateDirectories: true)
        try FileManager.default.copyItem(at: made.odex, to: written.appendingPathComponent("base.odex"))
        try FileManager.default.copyItem(at: made.vdex, to: written.appendingPathComponent("base.vdex"))

        try await library.adoptCompiledOutput(
            id: entry.id, output: output, bootClassPath: "bcp-1", converter: NativeCodeTests.Converter())

        #expect(await library.oatFolders(id: entry.id, bootClassPath: "bcp-1") != nil)
        #expect(!sandbox.exists(output))
    }

    @Test func aCompileStartThatWroteNothingChangesNothingAndLeavesNoOutput() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try await Self.installed(sandbox, library)
        let output = sandbox.root.appendingPathComponent("oat.work")
        let written = output.appendingPathComponent("data/local/tmp")
        try FileManager.default.createDirectory(at: written, withIntermediateDirectories: true)
        try Data("half".utf8).write(to: written.appendingPathComponent("base.odex"))  // no vdex: dex2oat was killed

        await #expect(throws: (any Error).self) {
            try await library.adoptCompiledOutput(
                id: entry.id, output: output, bootClassPath: "bcp-1", converter: NativeCodeTests.Converter())
        }

        #expect(await library.entries.first?.compiled == nil)
        #expect(await library.oatFolders(id: entry.id, bootClassPath: "bcp-1") == nil)
        #expect(!sandbox.exists(output))
    }

    /// A Java app, whose code compile mode compiles (E5).
    static func javaApp(_ sandbox: Sandbox, _ library: Library, versionCode: Int32 = 1) async throws -> LibraryEntry {
        let file = try sandbox.file(SampleAPK.make(versionCode: versionCode, nativeActivity: false),
                                    named: "app-\(versionCode).apk")
        return try await library.importFile(file, signing: nil).entry
    }

    @Test func aJavaAppNeedsCompilingUntilItIsCompiledForTheBootClassPath() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try await Self.javaApp(sandbox, library)
        #expect(sandbox.paths.needsCompiling(entry, bootClassPath: "bcp-1"))
        let made = try Self.compiled(sandbox)

        try await library.installCompiledCode(
            id: entry.id, odex: made.odex, vdex: made.vdex, bootClassPath: "bcp-1",
            converter: NativeCodeTests.Converter())

        let compiled = try #require(await library.entries.first)
        #expect(!sandbox.paths.needsCompiling(compiled, bootClassPath: "bcp-1"))
        // A framework change makes it stale: compiled again.
        #expect(sandbox.paths.needsCompiling(compiled, bootClassPath: "bcp-2"))
    }

    @Test func aNativeActivityAppNeedsNoCompiling() async throws {
        // It has no Java code that ART would run.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try await Self.installed(sandbox, library)

        #expect(!sandbox.paths.needsCompiling(entry, bootClassPath: "bcp-1"))
    }

    @Test func aFailedCompileIsNotTriedAgainForTheSameBootClassPath() async throws {
        // A compile that fails or that iOS ends (dex2oat's memory) would otherwise follow every session of the app.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try await Self.javaApp(sandbox, library)

        try await library.recordCompileFailure(id: entry.id, bootClassPath: "bcp-1")

        let failed = try #require(await library.entries.first)
        #expect(failed.compileFailed == "bcp-1")
        #expect(!sandbox.paths.needsCompiling(failed, bootClassPath: "bcp-1"))
        // Another framework may compile it.
        #expect(sandbox.paths.needsCompiling(failed, bootClassPath: "bcp-2"))
        // It survives a relaunch.
        #expect(try await Library(paths: sandbox.paths).entries.first?.compileFailed == "bcp-1")
    }

    @Test func compiledCodeClearsAnEarlierFailure() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try await Self.javaApp(sandbox, library)
        try await library.recordCompileFailure(id: entry.id, bootClassPath: "bcp-1")
        let made = try Self.compiled(sandbox)

        try await library.installCompiledCode(
            id: entry.id, odex: made.odex, vdex: made.vdex, bootClassPath: "bcp-2",
            converter: NativeCodeTests.Converter())

        #expect(await library.entries.first?.compileFailed == nil)
    }

    @Test func anUpdateIsNeitherCompiledNorFailed() async throws {
        // A new version has other code: it is compiled again.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try await Self.javaApp(sandbox, library)
        let made = try Self.compiled(sandbox)
        try await library.installCompiledCode(
            id: entry.id, odex: made.odex, vdex: made.vdex, bootClassPath: "bcp-1",
            converter: NativeCodeTests.Converter())
        try await library.recordCompileFailure(id: entry.id, bootClassPath: "bcp-2")

        let updated = try await Self.javaApp(sandbox, library, versionCode: 2)

        #expect(updated.report?.versionCode == 2)
        #expect(updated.compiled == nil)
        #expect(updated.compileFailed == nil)
        #expect(updated.compileInterrupted == nil)
        #expect(sandbox.paths.needsCompiling(updated, bootClassPath: "bcp-1"))
    }
}
