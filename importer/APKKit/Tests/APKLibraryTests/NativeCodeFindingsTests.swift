import APKKit
import CryptoKit
import APKTestSupport
import Foundation
import Testing

@testable import APKLibrary

/// ADR-0055, step a2: the import checks every native library to its end with the converter's check mode, whether or
/// not a certificate is at hand. A library on the start path that cannot load blocks the app with all its reasons; any
/// other degrades: the app stays ready, the library is left out of lib/ (so that System.loadLibrary finds none, as on
/// Android for a missing file), and the report names it with its reasons.
@Suite("Native code findings")
struct NativeCodeFindingsTests {
    typealias Sandbox = LibraryTests.Sandbox
    typealias Native = NativeCodeTests

    /// Gives the reasons chosen for a library by its file name.
    struct Checker: ELFRefusing {
        var reasons: [String: [String]] = [:]
        /// The libraries whose check fails, as a read error or a bad answer of the converter would.
        var failing: Set<String> = []

        struct Broke: Error, CustomStringConvertible {
            var description: String { "the file could not be read" }
        }

        func refusals(of elf: URL) throws -> [String] {
            if failing.contains(elf.lastPathComponent) { throw Broke() }
            return reasons[elf.lastPathComponent] ?? []
        }
    }

    static let two = ["the file has text relocations", "svc #0x80 at 0x218 is no Linux syscall"]
    static let twoText = two.joined(separator: "\n")

    @Test func aCheckThatFailsIsReportedAsInformsAndTheLibraryStays() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        await library.use(checker: Checker(failing: ["libhelper.so"]))

        let result = try await library.importFile(
            sandbox.file(Native.madeAPK(extractNativeLibs: false), named: "activity-test.apk"),
            signing: Native.signing())

        #expect(result.entry.state == .ready)
        #expect(result.entry.report?.compatibility.issues.contains(
            .libraryNotChecked(library: "libhelper.so", reason: "the file could not be read")) == true)
        #expect(Native.names(in: sandbox.paths.libraryDirectory(for: result.entry)).contains("libhelper.so"))
    }

    @Test func aCheckThatFailsForOneLibraryDoesNotStopTheOthers() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        await library.use(checker: Checker(reasons: ["libhelper.so": Self.two], failing: ["libactivity-test.so"]))

        let result = try await library.importFile(
            sandbox.file(Native.madeAPK(extractNativeLibs: false), named: "activity-test.apk"),
            signing: Native.signing())

        let issues = result.entry.report?.compatibility.issues ?? []
        #expect(result.entry.state == .ready)
        #expect(issues.contains(.libraryNotChecked(
            library: "libactivity-test.so", reason: "the file could not be read")))
        #expect(issues.contains(.libraryDegraded(library: "libhelper.so", reason: Self.twoText)))
    }

    @Test func aFileWhoseSymbolsCannotBeReadIsNotCheckedForThem() async throws {
        // The fixtures' libraries have no section headers, which the reader of symbols refuses.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        try await library.use(exportTable: "# minato export table 1\n@ libc.so\nneeds\nmalloc\n")

        let result = try await library.importFile(
            sandbox.file(Native.madeAPK(extractNativeLibs: false), named: "activity-test.apk"),
            signing: Native.signing())

        let notChecked = (result.entry.report?.compatibility.issues ?? []).compactMap { issue -> String? in
            if case .libraryNotChecked(let name, let reason) = issue { "\(name): \(reason)" } else { nil }
        }
        #expect(notChecked.count == 2)
        #expect(notChecked.allSatisfy { $0.contains("section headers") })
        #expect(result.entry.state == .ready)
    }

    @Test func nothingIsReportedAsNotCheckedWhenNothingWasAskedFor() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)

        let result = try await library.importFile(
            sandbox.file(Native.madeAPK(extractNativeLibs: false), named: "activity-test.apk"),
            signing: Native.signing())

        let notChecked = (result.entry.report?.compatibility.issues ?? []).filter {
            if case .libraryNotChecked = $0 { true } else { false }
        }
        #expect(notChecked.isEmpty)
    }

    @Test func aLibraryOutsideTheStartPathDegrades() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        await library.use(checker: Checker(reasons: ["libhelper.so": Self.two]))

        let result = try await library.importFile(
            sandbox.file(Native.madeAPK(extractNativeLibs: false), named: "activity-test.apk"),
            signing: Native.signing())

        #expect(result.entry.state == .ready)
        #expect(result.entry.report?.compatibility.status == .ready)
        #expect(result.entry.report?.compatibility.issues.contains(
            .libraryDegraded(library: "libhelper.so", reason: Self.twoText)) == true)
        #expect(Native.names(in: sandbox.paths.libraryDirectory(for: result.entry))
            == ["libactivity-test.so", "libactivity-test.so.dylib"])
        #expect(result.entry.signedWith == "aa")
    }

    @Test func theNativeActivitysLibraryBlocksWithEveryReason() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        await library.use(checker: Checker(reasons: ["libactivity-test.so": Self.two, "libhelper.so": ["not AArch64"]]))

        let result = try await library.importFile(
            sandbox.file(Native.madeAPK(extractNativeLibs: false), named: "activity-test.apk"),
            signing: Native.signing())

        #expect(result.entry.state == .incompatible)
        let issues = result.entry.report?.compatibility.issues ?? []
        #expect(issues.contains(.conversionFailed(library: "libactivity-test.so", reason: Self.twoText)))
        // The other finding is named too, though the app is blocked already.
        #expect(issues.contains(.libraryDegraded(library: "libhelper.so", reason: "not AArch64")))
        #expect(result.entry.signedWith == nil)
    }

    @Test func aCertificateIsNotNeededToFindThem() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        await library.use(checker: Checker(reasons: ["libhelper.so": Self.two]))

        let result = try await library.importFile(
            sandbox.file(Native.madeAPK(extractNativeLibs: false), named: "activity-test.apk"))

        #expect(result.entry.state == .ready)
        #expect(result.entry.report?.compatibility.issues.contains(
            .libraryDegraded(library: "libhelper.so", reason: Self.twoText)) == true)
        // The originals of the others wait for the first tap, as without a certificate always.
        #expect(Native.names(in: sandbox.paths.libraryDirectory(for: result.entry)) == ["libactivity-test.so"])
    }

    @Test func aJavaAppsRefusedLibraryDegrades() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        await library.use(checker: Checker(reasons: ["libgame.so": Self.two]))

        let result = try await library.importFile(sandbox.file(SampleAPK.make(), named: "game.apk"),
                                                  signing: Native.signing())

        #expect(result.entry.report?.runtime == .art)
        #expect(result.entry.state == .ready)
        #expect(result.entry.report?.compatibility.issues.contains(
            .libraryDegraded(library: "libgame.so", reason: Self.twoText)) == true)
        #expect(!sandbox.exists(sandbox.paths.libraryDirectory(for: result.entry).appendingPathComponent("libgame.so")))
    }

    @Test func anEngineCoreLibraryBlocks() async throws {
        // Unity's libunity.so and libmain.so are its start path (ADR-0055 decision 2), whatever the manifest says.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        await library.use(checker: Checker(reasons: ["libunity.so": Self.two]))
        let apk = try Native.handMadeAPK(libraries: ["libactivity-test.so", "libunity.so", "libextra.so"])

        let result = try await library.importFile(sandbox.file(apk, named: "unity.apk"), signing: Native.signing())

        #expect(result.entry.state == .incompatible)
        #expect(result.entry.report?.compatibility.issues.contains(
            .conversionFailed(library: "libunity.so", reason: Self.twoText)) == true)
    }

    @Test func everyBlockerIsNamed() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        await library.use(checker: Checker(reasons: ["libactivity-test.so": ["a"], "libunity.so": Self.two]))
        let apk = try Native.handMadeAPK(libraries: ["libactivity-test.so", "libunity.so"])

        let result = try await library.importFile(sandbox.file(apk, named: "unity.apk"), signing: Native.signing())

        let issues = result.entry.report?.compatibility.issues ?? []
        #expect(issues.contains(.conversionFailed(library: "libactivity-test.so", reason: "a")))
        #expect(issues.contains(.conversionFailed(library: "libunity.so", reason: Self.twoText)))
    }

    @Test func aDegradedLibraryStaysOutWhileTheCheckRefusesIt() async throws {
        // An entry whose lib/ is gone gets its originals again at the first tap; the check runs again, so the library
        // stays out and the finding is renewed, not added.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        await library.use(checker: Checker(reasons: ["libhelper.so": Self.two]))
        let entry = try await library.importFile(
            sandbox.file(Native.madeAPK(extractNativeLibs: false), named: "activity-test.apk")).entry
        try FileManager.default.removeItem(at: sandbox.paths.libraryDirectory(for: entry))

        let count = try await library.prepareNativeCode(id: entry.id)

        #expect(count == 1)
        #expect(Native.names(in: sandbox.paths.libraryDirectory(for: entry)) == ["libactivity-test.so"])
        let issues = await library.entries.first { $0.id == entry.id }?.report?.compatibility.issues ?? []
        #expect(issues.filter { if case .libraryDegraded = $0 { true } else { false } }.count == 1)
    }

    @Test func withoutACheckerNothingChanges() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)

        let result = try await library.importFile(
            sandbox.file(Native.madeAPK(extractNativeLibs: false), named: "activity-test.apk"),
            signing: Native.signing())

        #expect(result.entry.report?.compatibility.issues.contains {
            if case .libraryDegraded = $0 { return true }
            return false
        } == false)
        #expect(Native.names(in: sandbox.paths.libraryDirectory(for: result.entry)).count == 4)
    }
}

/// ADR-0055 a3: NEEDED names, and the start path closed over them.
@Suite("Native code findings, NEEDED")
struct NativeCodeNeededTests {
    typealias Sandbox = LibraryTests.Sandbox
    typealias Native = NativeCodeTests

    static let publicLibraries = "libc.so\nliblog.so\nlibm.so\nlibdl.so\n"

    /// The degrading findings of an entry's report.
    static func degraded(_ entry: LibraryEntry) -> [Compatibility.Issue] {
        (entry.report?.compatibility.issues ?? []).filter { if case .libraryDegraded = $0 { true } else { false } }
    }

    /// A NativeActivity app (libactivity-test.so) whose libraries need the given names, as real dynamic sections.
    static func apk(_ libraries: [String: [String]]) throws -> Data {
        var zip = ZipBuilder()
        zip.add("AndroidManifest.xml", try Native.manifest(extractNativeLibs: true))
        for (name, needed) in libraries.sorted(by: { $0.key < $1.key }) {
            zip.add("lib/arm64-v8a/\(name)", DynamicELF.make(needed: needed))
        }
        return zip.build().data
    }

    @Test func aStartLibraryThatNeedsARefusedOneBlocksAndBothAreNamed() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        await library.use(checker: NativeCodeFindingsTests.Checker(reasons: ["libfmod.so": ["text relocations"]]))
        await library.use(publicLibraries: Self.publicLibraries)
        let apk = try Self.apk(["libactivity-test.so": ["libc.so", "libfmod.so"], "libfmod.so": ["libm.so"]])

        let result = try await library.importFile(sandbox.file(apk, named: "a.apk"), signing: Native.signing())

        #expect(result.entry.state == .incompatible)
        let issues = result.entry.report?.compatibility.issues ?? []
        #expect(issues.contains(.conversionFailed(library: "libfmod.so", reason: "text relocations")))
        #expect(issues.contains(.conversionFailed(library: "libactivity-test.so",
                                                  reason: "it needs libfmod.so, which cannot load: text relocations")))
    }

    @Test func aMissingNameBlocksTheStartLibraryAndDegradesAnother() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        await library.use(publicLibraries: Self.publicLibraries)
        let apk = try Self.apk(["libactivity-test.so": ["libmissing.so"], "libother.so": ["libamidi.so"]])

        let result = try await library.importFile(sandbox.file(apk, named: "a.apk"), signing: Native.signing())

        let issues = result.entry.report?.compatibility.issues ?? []
        #expect(result.entry.state == .incompatible)
        #expect(issues.contains(.conversionFailed(
            library: "libactivity-test.so",
            reason: "it needs libmissing.so, which is not in the app and not one of Minato's libraries")))
        #expect(issues.contains(.libraryDegraded(
            library: "libother.so", reason: "it needs libamidi.so, an NDK library Minato does not have yet")))
    }

    @Test func anAppThatFindsEveryNameStaysReady() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        await library.use(publicLibraries: Self.publicLibraries)
        let apk = try Self.apk(["libactivity-test.so": ["libc.so", "libhelper.so"], "libhelper.so": ["liblog.so"]])

        let result = try await library.importFile(sandbox.file(apk, named: "a.apk"), signing: Native.signing())

        #expect(result.entry.state == .ready)
        #expect(Self.degraded(result.entry).isEmpty)
    }

    @Test func aNewCheckReplacesTheEarlierFindings() async throws {
        // A degraded library is not named for good; the next check starts from the files again.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        await library.use(checker: NativeCodeFindingsTests.Checker(reasons: ["libhelper.so": ["svc"]]))
        let entry = try await library.importFile(
            sandbox.file(Native.madeAPK(extractNativeLibs: false), named: "a.apk")).entry
        try FileManager.default.removeItem(at: sandbox.paths.libraryDirectory(for: entry))
        await library.use(checker: NativeCodeFindingsTests.Checker(reasons: [:]))  // a newer converter takes it

        _ = try await library.prepareNativeCode(id: entry.id)

        let current = await library.entries.first { $0.id == entry.id }
        #expect(current.map(Self.degraded)?.isEmpty == true)
        #expect(Native.names(in: sandbox.paths.libraryDirectory(for: entry)) == ["libactivity-test.so", "libhelper.so"])
    }
}

/// ADR-0055 a4: undefined symbols against the guest's export table.
@Suite("Native code findings, symbols")
struct NativeCodeSymbolTests {
    typealias Sandbox = LibraryTests.Sandbox
    typealias Native = NativeCodeTests

    static let publicLibraries = "libc.so\nlibandroid.so\nliblog.so\n"
    static let table = """
        # minato export table 1
        @ libc.so
        needs
        malloc
        free
        memcpy!
        @ libandroid.so
        needs libc.so
        AAsset_close@@LIBANDROID
        ASensorManager_getInstance@@LIBANDROID!
        @ liblog.so
        needs libc.so
        __android_log_print
        """

    /// A NativeActivity app (libactivity-test.so) whose libraries are real symbol tables.
    static func apk(_ libraries: [String: Data]) throws -> Data {
        var zip = ZipBuilder()
        zip.add("AndroidManifest.xml", try Native.manifest(extractNativeLibs: true))
        for (name, contents) in libraries.sorted(by: { $0.key < $1.key }) {
            zip.add("lib/arm64-v8a/\(name)", contents)
        }
        return zip.build().data
    }

    static func library(_ sandbox: Sandbox) throws -> Library {
        try Library(paths: sandbox.paths)
    }

    @Test func aLibraryOutsideTheStartPathWithAMissingSymbolDegrades() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        await library.use(publicLibraries: Self.publicLibraries)
        try await library.use(exportTable: Self.table)
        let apk = try Self.apk([
            "libactivity-test.so": SymbolELF.make(
                needed: ["libc.so"], defined: ["entry"], undefined: [.init("malloc")]),
            "libother.so": SymbolELF.make(needed: ["libc.so"], undefined: [.init("malloc"), .init("not_there")]),
        ])

        let result = try await library.importFile(sandbox.file(apk, named: "a.apk"), signing: Native.signing())

        #expect(result.entry.state == .ready)
        let issues = result.entry.report?.compatibility.issues ?? []
        #expect(issues.contains(.libraryDegraded(
            library: "libother.so", reason: "it needs the symbol not_there that nothing it needs defines")))
    }

    @Test func theStartLibrarysMissingSymbolBlocks() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        await library.use(publicLibraries: Self.publicLibraries)
        try await library.use(exportTable: Self.table)
        let apk = try Self.apk([
            "libactivity-test.so": SymbolELF.make(needed: ["libc.so"], undefined: [.init("not_there")])])

        let result = try await library.importFile(sandbox.file(apk, named: "a.apk"), signing: Native.signing())

        #expect(result.entry.state == .incompatible)
        #expect(result.entry.report?.compatibility.issues.contains(.conversionFailed(
            library: "libactivity-test.so",
            reason: "it needs the symbol not_there that nothing it needs defines")) == true)
    }

    @Test func aStubIsAFindingNotARefusal() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        await library.use(publicLibraries: Self.publicLibraries)
        try await library.use(exportTable: Self.table)
        let apk = try Self.apk([
            "libactivity-test.so": SymbolELF.make(
                needed: ["libc.so", "libandroid.so"],
                undefined: [.init("memcpy"), .init("ASensorManager_getInstance", version: "LIBANDROID"),
                            .init("free")]),
        ])

        let result = try await library.importFile(sandbox.file(apk, named: "a.apk"), signing: Native.signing())

        #expect(result.entry.state == .ready)
        #expect(result.entry.report?.compatibility.issues.contains(.libraryCallsStubs(
            library: "libactivity-test.so",
            reason: "calls 2 functions that Minato does not have yet (they log once and answer nothing): memcpy, "
                + "ASensorManager_getInstance")) == true)
        // The library stays: it loads.
        #expect(Native.names(in: sandbox.paths.libraryDirectory(for: result.entry)).contains("libactivity-test.so"))
    }

    @Test func aLibraryWhoseSymbolsCannotBeReadMakesNoOtherLookBroken() async throws {
        // libbroken.so has no section headers (the converter would say so), so what it defines is unknown: a symbol
        // that only it could define is not reported missing.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        await library.use(publicLibraries: Self.publicLibraries)
        try await library.use(exportTable: Self.table)
        let apk = try Self.apk([
            "libactivity-test.so": SymbolELF.make(needed: ["libbroken.so"], undefined: [.init("from_broken")]),
            "libbroken.so": DynamicELF.make(needed: ["libc.so"]),
        ])

        let result = try await library.importFile(sandbox.file(apk, named: "a.apk"), signing: Native.signing())

        #expect(result.entry.state == .ready)
    }

    @Test func theReportRecordsWhatTheCheckUsed() async throws {
        // Decision 4's second check, after a Minato update, sees by these digests that a table changed.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        await library.use(publicLibraries: Self.publicLibraries)
        try await library.use(exportTable: Self.table)
        let apk = try Self.apk(["libactivity-test.so": SymbolELF.make(needed: ["libc.so"], undefined: [.init("malloc")])])

        let result = try await library.importFile(sandbox.file(apk, named: "a.apk"), signing: Native.signing())

        func digest(_ text: String) -> String {
            SHA256.hash(data: Data(text.utf8)).map { String(format: "%02x", $0) }.joined()
        }
        let checks = result.entry.report?.compatibility.checks
        #expect(checks?.exportTable == digest(Self.table))
        #expect(checks?.publicLibraries == digest(Self.publicLibraries))
    }

    @Test func aReportWithoutTheCheckRecordsNone() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)

        let result = try await library.importFile(
            sandbox.file(Native.madeAPK(extractNativeLibs: false), named: "a.apk"), signing: Native.signing())

        #expect(result.entry.report?.compatibility.checks == nil)
    }

    @Test func withoutATableNoSymbolIsChecked() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        await library.use(publicLibraries: Self.publicLibraries)
        let apk = try Self.apk([
            "libactivity-test.so": SymbolELF.make(needed: ["libc.so"], undefined: [.init("not_there")])])

        let result = try await library.importFile(sandbox.file(apk, named: "a.apk"), signing: Native.signing())

        #expect(result.entry.state == .ready)
    }

    @Test func aBadTableIsAnErrorForTheCaller() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)

        await #expect(throws: ExportTable.FormatError.self) { try await library.use(exportTable: "garbage") }
    }
}
