import APKSigning
import APKTestSupport
import Foundation
import Testing

@testable import APKLibrary

@Suite("Re-signing")
struct ResigningTests {
    /// Writes "<fingerprint>:<ELF contents>" where the converter would write a signed container,
    /// and fails on one chosen file.
    struct RecordingConverter: ContainerConverting {
        let fingerprint: String
        var failOn: String?

        func convert(_ elf: URL, to container: URL) throws {
            if elf.lastPathComponent == failOn { throw CocoaError(.fileWriteUnknown) }
            try (Data("\(fingerprint):".utf8) + Data(contentsOf: elf)).write(to: container)
        }
    }

    /// An installed app with the ELF files `names` in its lib/ folder, as the import in M3 leaves them.
    static func installedApp(in sandbox: LibraryTests.Sandbox, library: Library, libraries names: [String]) async throws -> LibraryEntry {
        let entry = try await library.importFile(sandbox.file(SampleAPK.make(), named: "game.apk")).entry
        let lib = sandbox.paths.libraryDirectory(for: entry)
        try FileManager.default.createDirectory(at: lib, withIntermediateDirectories: true)
        for name in names { try Data(name.utf8).write(to: lib.appendingPathComponent(name)) }
        return entry
    }

    @Test func resignsEveryLibraryAndRecordsTheCertificate() async throws {
        let sandbox = try LibraryTests.Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try await Self.installedApp(in: sandbox, library: library, libraries: ["libmain.so", "libfoo.so"])

        #expect(try await library.resign(id: entry.id, fingerprint: "aa", with: RecordingConverter(fingerprint: "aa")) == 2)

        let lib = sandbox.paths.libraryDirectory(for: entry)
        #expect(try Data(contentsOf: lib.appendingPathComponent("libfoo.so.dylib")) == Data("aa:libfoo.so".utf8))
        #expect(try Data(contentsOf: lib.appendingPathComponent("libmain.so.dylib")) == Data("aa:libmain.so".utf8))
        #expect(await library.entries.first?.signedWith == "aa")
        #expect(try await Library(paths: sandbox.paths).entries.first?.signedWith == "aa")
    }

    @Test func aFailureKeepsTheOldRecordSoTheNextLaunchRetries() async throws {
        let sandbox = try LibraryTests.Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try await Self.installedApp(in: sandbox, library: library, libraries: ["liba.so", "libb.so"])

        await #expect(throws: CocoaError.self) {
            try await library.resign(id: entry.id, fingerprint: "aa", with: RecordingConverter(fingerprint: "aa", failOn: "libb.so"))
        }
        #expect(await library.entries.first?.signedWith == nil)
    }

    @Test func candidatesCountTheConvertedLibraries() async throws {
        let sandbox = try LibraryTests.Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try await Self.installedApp(in: sandbox, library: library, libraries: ["liba.so", "libb.so", "notes.txt"])
        try await library.resign(id: entry.id, fingerprint: "bb", with: RecordingConverter(fingerprint: "bb"))

        #expect(await library.resignCandidates() == [ResignCandidate(id: entry.id, signedWith: "bb", containerCount: 2)])
    }

    @Test func resignAllSignsWhatTheCurrentCertificateDidNotSign() async throws {
        let sandbox = try LibraryTests.Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let old = try await library.importFile(sandbox.file(SampleAPK.make(package: "org.example.old"), named: "old.apk")).entry
        let unrecorded = try await library.importFile(sandbox.file(SampleAPK.make(package: "org.example.new"), named: "new.apk")).entry
        let current = try await library.importFile(sandbox.file(SampleAPK.make(package: "org.example.current"), named: "current.apk")).entry
        _ = try await library.importFile(sandbox.file(SampleAPK.make(package: "org.example.plain"), named: "plain.apk"))
        for (entry, names) in [(old, ["liba.so", "libb.so"]), (unrecorded, ["libc.so"]), (current, ["libd.so"])] {
            let lib = sandbox.paths.libraryDirectory(for: entry)
            try FileManager.default.createDirectory(at: lib, withIntermediateDirectories: true)
            for name in names { try Data(name.utf8).write(to: lib.appendingPathComponent(name)) }
        }
        try await library.resign(id: old.id, fingerprint: "bb", with: RecordingConverter(fingerprint: "bb"))
        try await library.resign(id: current.id, fingerprint: "aa", with: RecordingConverter(fingerprint: "aa"))

        let started = Recorder()
        let summary = await library.resignAll(fingerprint: "aa", with: RecordingConverter(fingerprint: "aa")) { started.add($0) }

        #expect(summary == ResignSummary(entries: 2, files: 3, failures: []))
        #expect(started.values.sorted() == [old.id, unrecorded.id].sorted())
        let signed = Dictionary(uniqueKeysWithValues: await library.entries.map { ($0.id, $0.signedWith) })
        #expect(signed[old.id] == "aa")
        #expect(signed[unrecorded.id] == "aa")
        #expect(try Data(contentsOf: sandbox.paths.libraryDirectory(for: current).appendingPathComponent("libd.so.dylib"))
            == Data("aa:libd.so".utf8))
    }

    @Test func resignAllGoesOnAfterAFailureAndReportsIt() async throws {
        let sandbox = try LibraryTests.Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let broken = try await library.importFile(sandbox.file(SampleAPK.make(package: "org.example.broken"), named: "broken.apk")).entry
        let fine = try await library.importFile(sandbox.file(SampleAPK.make(package: "org.example.fine"), named: "fine.apk")).entry
        for (entry, name) in [(broken, "libbroken.so"), (fine, "libfine.so")] {
            let lib = sandbox.paths.libraryDirectory(for: entry)
            try FileManager.default.createDirectory(at: lib, withIntermediateDirectories: true)
            try Data(name.utf8).write(to: lib.appendingPathComponent(name))
        }

        let summary = await library.resignAll(fingerprint: "aa", with: RecordingConverter(fingerprint: "aa", failOn: "libbroken.so"))

        #expect(summary.entries == 1)
        #expect(summary.files == 1)
        #expect(summary.failures.count == 1)
        #expect(summary.failures.first?.hasPrefix("\(broken.id): ") == true)
        let signed = Dictionary(uniqueKeysWithValues: await library.entries.map { ($0.id, $0.signedWith) })
        #expect(signed[broken.id] == .some(nil))
        #expect(signed[fine.id] == "aa")
    }

    @Test func resignAllWorksEntryByEntry() async throws {
        let sandbox = try LibraryTests.Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let first = try await library.importFile(sandbox.file(SampleAPK.make(package: "org.example.a"), named: "a.apk")).entry
        let second = try await library.importFile(sandbox.file(SampleAPK.make(package: "org.example.b"), named: "b.apk")).entry
        for (entry, names) in [(first, ["liba.so", "libb.so"]), (second, ["libc.so"])] {
            let lib = sandbox.paths.libraryDirectory(for: entry)
            try FileManager.default.createDirectory(at: lib, withIntermediateDirectories: true)
            for name in names { try Data(name.utf8).write(to: lib.appendingPathComponent(name)) }
        }
        let events = Recorder()
        struct LoggingConverter: ContainerConverting {
            let events: Recorder
            func convert(_ elf: URL, to container: URL) throws {
                events.add("convert \(elf.lastPathComponent)")
                try Data().write(to: container)
            }
        }

        _ = await library.resignAll(fingerprint: "aa", with: LoggingConverter(events: events)) { id in
            events.add("start \(id)")
        }

        #expect(events.values == ["start org.example.a", "convert liba.so", "convert libb.so",
                                  "start org.example.b", "convert libc.so"])
    }

    @Test func forgettingTheSignaturesMakesEverythingDueAgain() async throws {
        let sandbox = try LibraryTests.Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try await Self.installedApp(in: sandbox, library: library, libraries: ["liba.so"])
        try await library.resign(id: entry.id, fingerprint: "aa", with: RecordingConverter(fingerprint: "aa"))
        #expect(entriesToResign(await library.resignCandidates(), current: "aa").isEmpty)

        try await library.forgetSignatures()

        #expect(entriesToResign(await library.resignCandidates(), current: "aa") == [entry.id])
        #expect(try await Library(paths: sandbox.paths).entries.first?.signedWith == nil)
    }

    /// Collects values reported from the library actor.
    final class Recorder: @unchecked Sendable {
        private let lock = NSLock()
        private var collected: [String] = []
        func add(_ value: String) { lock.withLock { collected.append(value) } }
        var values: [String] { lock.withLock { collected } }
    }

    @Test func anAppWithoutConvertedCodeHasNothingToResign() async throws {
        let sandbox = try LibraryTests.Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try await library.importFile(sandbox.file(SampleAPK.make(), named: "game.apk")).entry

        #expect(await library.resignCandidates() == [ResignCandidate(id: entry.id, signedWith: nil, containerCount: 0)])
        #expect(try await library.resign(id: entry.id, fingerprint: "aa", with: RecordingConverter(fingerprint: "aa")) == 0)
        #expect(await library.entries.first?.signedWith == nil)
    }
}
