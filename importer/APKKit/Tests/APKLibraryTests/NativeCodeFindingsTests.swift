import APKKit
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

        func refusals(of elf: URL) throws -> [String] {
            reasons[elf.lastPathComponent] ?? []
        }
    }

    static let two = ["the file has text relocations", "svc #0x80 at 0x218 is no Linux syscall"]
    static let twoText = two.joined(separator: "\n")

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
        // stays out and the finding is renewed, not added (App's review).
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

/// ADR-0055 a3: NEEDED names, and the start path closed over them (App's review).
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
                                                  reason: "it needs libfmod.so, which cannot load")))
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
            library: "libother.so", reason: "it needs libamidi.so, an NDK library Minato does not have yet (gap 14)")))
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
        // App's review: a degraded library is not named for good; the next check starts from the files again.
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
