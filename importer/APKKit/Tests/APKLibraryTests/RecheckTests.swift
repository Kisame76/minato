import APKKit
import APKTestSupport
import Foundation
import Synchronization
import Testing

@testable import APKLibrary

/// ADR-0055 decision 5: each report records the Minato that checked it, and after an update the library checks every
/// entry again from its installed APKs, analyzer and converter check both, so that an old import gets the new findings
/// without being imported again.
@Suite("Re-check after an update")
struct RecheckTests {
    typealias Sandbox = LibraryTests.Sandbox
    typealias Native = NativeCodeTests
    typealias Checker = NativeCodeFindingsTests.Checker

    /// Counts the libraries it is asked about, and refuses none.
    final class CountingChecker: ELFRefusing {
        let asked = Mutex(0)

        func refusals(of elf: URL) throws -> [String] {
            asked.withLock { $0 += 1 }
            return []
        }
    }

    static let reasons = ["the file has text relocations"]

    static func imported(_ library: Library, in sandbox: Sandbox) async throws -> LibraryEntry {
        try await library.importFile(sandbox.file(Native.madeAPK(extractNativeLibs: false), named: "activity-test.apk"),
                                     signing: Native.signing()).entry
    }

    static func current(_ library: Library, _ id: String) async throws -> LibraryEntry {
        try #require(await library.entries.first { $0.id == id })
    }

    @Test func anOldReadyWithoutTurnsReadyWhenItsLibraryPassesNow() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        await library.use(checkStamp: "old")
        await library.use(checker: Checker(reasons: ["libhelper.so": Self.reasons]))
        let entry = try await Self.imported(library, in: sandbox)
        #expect(entry.degradedLibraries == ["libhelper.so"])
        #expect(entry.report?.checkedBy == "old")

        await library.use(checker: Checker())
        await library.use(checkStamp: "new")
        let summary = await library.recheckAll()

        let now = try await Self.current(library, entry.id)
        #expect(summary.checked == [entry.id])
        #expect(now.state == .ready)
        #expect(now.degradedLibraries.isEmpty)
        #expect(now.report?.checkedBy == "new")
        // The library is back in lib/, unsigned: the next tap signs it, as after a certificate change.
        #expect(Native.names(in: sandbox.paths.libraryDirectory(for: now)).contains("libhelper.so"))
        #expect(now.signedWith == nil)
    }

    @Test func anOldReadyTurnsIncompatibleWhenItsStartLibraryIsRefusedNow() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        await library.use(checkStamp: "old")
        await library.use(checker: Checker())
        let entry = try await Self.imported(library, in: sandbox)
        #expect(entry.state == .ready)

        await library.use(checker: Checker(reasons: ["libactivity-test.so": Self.reasons]))
        await library.use(checkStamp: "new")
        _ = await library.recheckAll()

        let now = try await Self.current(library, entry.id)
        #expect(now.state == .incompatible)
        #expect(now.report?.compatibility.issues.contains(
            .conversionFailed(library: "libactivity-test.so", reason: Self.reasons[0])) == true)
        #expect(now.report?.checkedBy == "new")
    }

    /// A Minato that checks wrongly must not cost the user the app: a re-check keeps the installed APKs, so the next
    /// Minato checks it again and it is Ready, without an import.
    @Test func aWrongRefusalIsUndoneByTheNextMinatoWithoutAnImport() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        await library.use(checkStamp: "good")
        await library.use(checker: Checker())
        let entry = try await Self.imported(library, in: sandbox)
        #expect(entry.state == .ready)

        await library.use(checker: Checker(reasons: ["libactivity-test.so": Self.reasons]))
        await library.use(checkStamp: "bad")
        _ = await library.recheckAll()
        let refused = try await Self.current(library, entry.id)
        #expect(refused.state == .incompatible)
        #expect(refused.base == entry.base)
        let version = sandbox.paths.versionDirectory(for: refused)
        #expect(FileManager.default.fileExists(atPath: version.appendingPathComponent("base.apk").path))
        // Its code goes, as an incompatible app holds none.
        #expect(Native.names(in: sandbox.paths.libraryDirectory(for: refused)).isEmpty)

        await library.use(checker: Checker())
        await library.use(checkStamp: "fixed")
        let summary = await library.recheckAll()

        let now = try await Self.current(library, entry.id)
        #expect(summary.checked == [entry.id])
        #expect(now.state == .ready)
        #expect(now.report?.checkedBy == "fixed")
        #expect(!(now.report?.compatibility.issues.contains(
            .conversionFailed(library: "libactivity-test.so", reason: Self.reasons[0])) ?? true))
        #expect(Native.names(in: sandbox.paths.libraryDirectory(for: now)).contains("libactivity-test.so"))
    }

    @Test func anEntryCheckedByThisMinatoIsLeftAlone() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        await library.use(checkStamp: "same")
        let entry = try await Self.imported(library, in: sandbox)

        let counting = CountingChecker()
        await library.use(checker: counting)
        let waiting = await library.entriesToRecheck()
        let checkedAlone = try await library.recheck(id: entry.id)
        let summary = await library.recheckAll()

        #expect(waiting.isEmpty)
        #expect(!checkedAlone)
        #expect(summary.checked.isEmpty)
        #expect(counting.asked.withLock { $0 } == 0)
        #expect(try await Self.current(library, entry.id) == entry)
    }

    @Test func aReportFromBeforeStampsCountsAsOld() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try await Self.imported(library, in: sandbox)
        #expect(entry.report?.checkedBy == nil)

        await library.use(checkStamp: "new")
        let summary = await library.recheckAll()

        #expect(summary.checked == [entry.id])
        #expect(try await Self.current(library, entry.id).report?.checkedBy == "new")
    }

    @Test func anUnreadableAPKKeepsTheOldReportForTheNextLaunch() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        await library.use(checkStamp: "old")
        let entry = try await Self.imported(library, in: sandbox)
        let apk = sandbox.paths.versionDirectory(for: entry).appendingPathComponent("base.apk")
        try FileManager.default.setAttributes([.posixPermissions: 0o644], ofItemAtPath: apk.path)
        try Data("damaged".utf8).write(to: apk)

        await library.use(checkStamp: "new")
        let summary = await library.recheckAll()

        #expect(summary.checked.isEmpty)
        #expect(summary.failures.count == 1)
        #expect(try await Self.current(library, entry.id) == entry)
    }
}
