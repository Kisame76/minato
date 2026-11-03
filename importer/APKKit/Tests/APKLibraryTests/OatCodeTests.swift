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
        try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
        try FileManager.default.copyItem(at: made.odex, to: output.appendingPathComponent("base.odex"))
        try FileManager.default.copyItem(at: made.vdex, to: output.appendingPathComponent("base.vdex"))

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
        try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
        try Data("half".utf8).write(to: output.appendingPathComponent("base.odex"))  // no vdex: dex2oat was killed

        await #expect(throws: (any Error).self) {
            try await library.adoptCompiledOutput(
                id: entry.id, output: output, bootClassPath: "bcp-1", converter: NativeCodeTests.Converter())
        }

        #expect(await library.entries.first?.compiled == nil)
        #expect(await library.oatFolders(id: entry.id, bootClassPath: "bcp-1") == nil)
        #expect(!sandbox.exists(output))
    }
}
