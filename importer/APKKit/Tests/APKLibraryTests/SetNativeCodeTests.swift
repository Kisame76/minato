import APKFormats
import APKKit
import APKTestSupport
import Foundation
import Testing

@testable import APKLibrary

/// A NativeActivity set's native code comes from the base and from every chosen split, one copy of each library name:
/// the one Android installs (ADR-0048, decision 8; step b6).
@Suite("Native code of a set")
struct SetNativeCodeTests {
    typealias Sandbox = LibraryTests.Sandbox
    typealias Sets = SetImportTests
    typealias Native = NativeCodeTests

    /// lib/arm64-v8a/ with these libraries, each an AArch64 ELF whose bytes name it.
    static func libraries(_ names: [String]) -> [String: Data] {
        Dictionary(uniqueKeysWithValues: names.map { ("lib/arm64-v8a/\($0)", Native.library($0)) })
    }

    /// A NativeActivity base that loads libmain.so, with `libraries` of its own.
    static func base(_ libraries: [String] = [], extractNativeLibs: Bool? = nil) -> Data {
        SplitAPK.make(nativeActivity: "main", extractNativeLibs: extractNativeLibs, files: Self.libraries(libraries))
    }

    static func split(_ name: String, _ libraries: [String], isFeatureSplit: Bool = false) -> Data {
        SplitAPK.make(split: name, isFeatureSplit: isFeatureSplit, files: Self.libraries(libraries))
    }

    static let arm64 = split("config.arm64_v8a", ["libmain.so"])

    /// An APK whose libmain.so is the copy `copy`, an ELF with other bytes than the other copies: the base without
    /// `split`, which loads it.
    static func withLibmain(_ copy: String, split: String? = nil, isFeatureSplit: Bool = false,
                            extractNativeLibs: Bool? = nil) -> Data {
        SplitAPK.make(split: split, isFeatureSplit: isFeatureSplit, nativeActivity: split == nil ? "main" : nil,
                      extractNativeLibs: extractNativeLibs, files: ["lib/arm64-v8a/libmain.so": Native.library(copy)])
    }

    /// The libmain.so that a set of `apks` gets, imported into a new library.
    static func libmain(of apks: [String: Data]) async throws -> Data {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try #require(try await library.importUnit(Sets.unit(apks, in: sandbox.paths), signing: Native.signing())
            .first?.entry)
        #expect(entry.state == .ready)
        return try Data(contentsOf: sandbox.paths.libraryDirectory(for: entry).appendingPathComponent("libmain.so"))
    }

    /// An installed set of a NativeActivity base with `libraries` and an arm64 split with libmain.so, signed at import.
    static func installedSet(_ library: Library, _ paths: LibraryPaths, base libraries: [String] = []) async throws
        -> LibraryEntry {
        try #require(try await library.importUnit(Sets.unit(["base.apk": Self.base(libraries), "arm64.apk": Self.arm64],
                                                            in: paths), signing: Native.signing()).first?.entry)
    }

    @Test func librariesComeFromTheBaseAndEveryChosenSplit() async throws {
        // A density split the choice skips gives none, whatever it holds.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let unit = try Sets.unit(["base.apk": Self.base(["libhelper.so"]), "arm64.apk": Self.arm64,
                                  "f.apk": Self.split("dynamic", ["libfeature.so"], isFeatureSplit: true),
                                  "xxhdpi.apk": Self.split("config.xxhdpi", []),
                                  "xxxhdpi.apk": Self.split("config.xxxhdpi", ["libskipped.so"])], in: sandbox.paths)

        let entry = try #require(try await library.importUnit(unit, signing: Native.signing()).first?.entry)

        #expect(entry.state == .ready)
        #expect(entry.signedWith == "aa")
        let lib = sandbox.paths.libraryDirectory(for: entry)
        #expect(Native.names(in: lib) == ["libfeature.so", "libfeature.so.dylib", "libhelper.so", "libhelper.so.dylib",
                                         "libmain.so", "libmain.so.dylib"])
        let main = try Data(contentsOf: lib.appendingPathComponent("libmain.so"))
        #expect(main == Native.library("libmain.so"))
    }

    @Test func theSameLibraryInTwoAPKsIsInstalledOnce() async throws {
        // bundletool builds such sets where two modules hold the same file (EntryClashValidator.java:90-99).
        #expect(try await Self.libmain(of: ["base.apk": Self.base(["libmain.so"]), "arm64.apk": Self.arm64])
            == Native.library("libmain.so"))
    }

    @Test func ofDifferentCopiesTheLastAPKInAndroidsOrderIsExtracted() async throws {
        // Android extracts the base, then the splits by name, into one lib/, a later copy replacing an earlier one
        // (NativeLibraryHelper.java:196-205, NativeLibraryHelper.cpp:211-218, PackageLite.java:163-170). By file name,
        // dynamic's a.apk would come first.
        let libmain = try await Self.libmain(of: [
            "base.apk": Self.withLibmain("the base's"),
            "z.apk": Self.withLibmain("config.arm64_v8a's", split: "config.arm64_v8a"),
            "a.apk": Self.withLibmain("dynamic's", split: "dynamic", isFeatureSplit: true),
        ])
        #expect(libmain == Native.library("dynamic's"))
    }

    @Test func withExtractNativeLibsFalseTheFirstAPKsCopyIsUsed() async throws {
        // Android then extracts nothing and loads from the first APK on its search path, the base and then the splits
        // by name (LoadedApk.java:485-489, 565-566): the base's copy, or else the first split's.
        let fromTheBase = try await Self.libmain(of: [
            "base.apk": Self.withLibmain("the base's", extractNativeLibs: false),
            "arm64.apk": Self.withLibmain("config.arm64_v8a's", split: "config.arm64_v8a"),
        ])
        #expect(fromTheBase == Native.library("the base's"))
        let fromTheFirstSplit = try await Self.libmain(of: [
            "base.apk": Self.base(extractNativeLibs: false),
            "z.apk": Self.withLibmain("config.arm64_v8a's", split: "config.arm64_v8a"),
            "a.apk": Self.withLibmain("dynamic's", split: "dynamic", isFeatureSplit: true),
        ])
        #expect(fromTheFirstSplit == Native.library("config.arm64_v8a's"))
    }

    /// The issues of a set with these libraries in its NativeActivity base and its arm64 split.
    static func issues(base: [String], arm64: [String]) async throws -> [Compatibility.Issue] {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let unit = try Sets.unit(["base.apk": Self.base(base), "arm64.apk": Self.split("config.arm64_v8a", arm64)],
                                 in: sandbox.paths)
        let entry = try #require(try await library.importUnit(unit, signing: Native.signing()).first?.entry)
        #expect(entry.state == .incompatible)
        return entry.report?.compatibility.issues ?? []
    }

    @Test func libraryNamesThatDifferOnlyInCaseRefuseTheSet() async throws {
        // Android keeps both, and so does the iPhone's case-sensitive volume, but one lib/ on the Mac's or the
        // Simulator's, which ignore case, cannot: refused everywhere, as split names are (decision 3), and within
        // one APK too.
        let across = try await Self.issues(base: ["libHelper.so"], arm64: ["libmain.so", "libhelper.so"])
        #expect(across.contains(.conversionFailed(library: "libhelper.so", reason: "its name differs from libHelper.so only in case")))
        let within = try await Self.issues(base: [], arm64: ["libMain.so", "libmain.so"])
        #expect(within.contains(.conversionFailed(library: "libmain.so", reason: "its name differs from libMain.so only in case")))
    }

    @Test func oneAPKWhoseLibraryNamesDifferOnlyInCaseIsRefusedToo() async throws {
        // The same check runs for one APK, through importFile (the lead's decision).
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)

        let result = try await library.importFile(sandbox.file(Self.base(["libMain.so", "libmain.so"]), named: "app.apk"),
                                                  signing: Native.signing())

        #expect(result.entry.state == .incompatible)
        #expect(result.entry.report?.compatibility.issues.contains(
            .conversionFailed(library: "libmain.so", reason: "its name differs from libMain.so only in case")) == true)
    }

    @Test func aLibraryNameLongerThan236BytesIsRefused() async throws {
        // lib/ holds <name>.dylib next to each library, which the converter writes through <name>.dylib.<8 hex
        // digits>.tmp (c_api.cpp, replace_file): 19 bytes more than the name, within APFS's 255. Android extracts
        // names of up to 255 bytes.
        func name(_ bytes: Int) -> String { "lib" + String(repeating: "a", count: bytes - 6) + ".so" }
        for (bytes, refused) in [(236, false), (237, true), (256, true)] {
            let sandbox = try Sandbox()
            defer { sandbox.remove() }
            let library = try Library(paths: sandbox.paths)

            let result = try await library.importFile(sandbox.file(Self.base(["libmain.so", name(bytes)]), named: "app.apk"),
                                                      signing: Native.signing())

            #expect(result.entry.state == (refused ? .incompatible : .ready), "\(bytes) bytes")
            #expect(result.entry.report?.compatibility.issues.contains(
                .conversionFailed(library: name(bytes), reason: "its name is longer than 236 bytes")) == refused)
        }
    }

    @Test func aJoinTakesTheInstalledAPKsLibraries() async throws {
        // The joined version's native code comes from all of its APKs, the installed ones too (decision 7).
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await Self.installedSet(library, sandbox.paths, base: ["libhelper.so"])

        let joined = try #require(try await library.importUnit(Sets.unit(
            ["f.apk": Self.split("dynamic", ["libfeature.so"], isFeatureSplit: true)], in: sandbox.paths),
            signing: Native.signing()).first?.entry)

        #expect(joined.state == .ready && joined.splits.count == 2)
        #expect(Native.names(in: sandbox.paths.libraryDirectory(for: joined)) == [
            "libfeature.so", "libfeature.so.dylib", "libhelper.so", "libhelper.so.dylib", "libmain.so", "libmain.so.dylib",
        ])
    }

    @Test func aJoiningSplitsCopyReplacesTheInstalledOneAsAndroidExtractsIt() async throws {
        // Inherit mode extracts the whole joined set again, in its order (PackageInstallerSession.java:2994-3010,
        // 4361-4380): dynamic comes after config.arm64_v8a.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await Self.installedSet(library, sandbox.paths)

        let joined = try #require(try await library.importUnit(Sets.unit(
            ["f.apk": Self.withLibmain("dynamic's", split: "dynamic", isFeatureSplit: true)], in: sandbox.paths),
            signing: Native.signing()).first?.entry)

        #expect(joined.state == .ready)
        let libmain = try Data(contentsOf: sandbox.paths.libraryDirectory(for: joined).appendingPathComponent("libmain.so"))
        #expect(libmain == Native.library("dynamic's"))
    }

    @Test func aSetIsSignedOnTheFirstTap() async throws {
        // Imported without a certificate, only the originals stay, and the first tap signs them (ADR-0023, ADR-0032).
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try #require(try await library.importUnit(
            Sets.unit(["base.apk": Self.base(), "arm64.apk": Self.arm64], in: sandbox.paths)).first?.entry)
        #expect(entry.state == .ready && entry.signedWith == nil)

        #expect(try await library.prepareNativeCode(id: entry.id) == 1)
        #expect(try await library.resign(id: entry.id, fingerprint: "bb", with: Native.Converter(fingerprint: "bb")) == 1)

        let container = try Data(contentsOf: sandbox.paths.libraryDirectory(for: entry).appendingPathComponent("libmain.so.dylib"))
        #expect(container == Data("bb:".utf8) + Native.library("libmain.so"))
        #expect(await library.entries.first?.signedWith == "bb")
    }

    @Test func theFirstTapExtractsASetsLibrariesFromItsSplitsAgain() async throws {
        // A version whose lib/ is missing, or holds no originals, gets them from its own APKs, the splits' too.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try #require(try await library.importUnit(
            Sets.unit(["base.apk": Self.base(["libhelper.so"]), "arm64.apk": Self.arm64], in: sandbox.paths)).first?.entry)
        let lib = sandbox.paths.libraryDirectory(for: entry)
        try FileManager.default.removeItem(at: lib)

        #expect(try await library.prepareNativeCode(id: entry.id) == 2)
        #expect(Native.names(in: lib) == ["libhelper.so", "libmain.so"])

        for original in Native.names(in: lib) { try FileManager.default.removeItem(at: lib.appendingPathComponent(original)) }
        try Data().write(to: lib.appendingPathComponent("libmain.so.dylib"))

        #expect(try await library.prepareNativeCode(id: entry.id) == 2)
        #expect(Native.names(in: lib) == ["libhelper.so", "libmain.so"])
    }

    @Test func theFirstTapExtractsAJoinedSetsLibrariesAgain() async throws {
        // From the joined version's folder, whose APKs include clones of the installed ones.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await Self.installedSet(library, sandbox.paths, base: ["libhelper.so"])
        let joined = try #require(try await library.importUnit(Sets.unit(
            ["f.apk": Self.split("dynamic", ["libfeature.so"], isFeatureSplit: true)], in: sandbox.paths)).first?.entry)
        try FileManager.default.removeItem(at: sandbox.paths.libraryDirectory(for: joined))

        #expect(try await library.prepareNativeCode(id: joined.id) == 3)

        #expect(Native.names(in: sandbox.paths.libraryDirectory(for: joined)) == ["libfeature.so", "libhelper.so", "libmain.so"])
    }

    @Test func aRelaunchedLibraryExtractsASetsLibrariesAgain() async throws {
        // What library.json records of the set is enough: its base and split files by name.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let entry = try #require(try await Library(paths: sandbox.paths).importUnit(
            Sets.unit(["base.apk": Self.base(["libhelper.so"]), "arm64.apk": Self.arm64], in: sandbox.paths)).first?.entry)
        try FileManager.default.removeItem(at: sandbox.paths.libraryDirectory(for: entry))

        let relaunched = try Library(paths: sandbox.paths)

        #expect(try await relaunched.prepareNativeCode(id: entry.id) == 2)
        #expect(Native.names(in: sandbox.paths.libraryDirectory(for: entry)) == ["libhelper.so", "libmain.so"])
    }

    @Test func anErrorHalfwayThroughTheFirstTapLeavesNoLib() async throws {
        // The base's libhelper.so is out before the split's libmain.so fails, and a lib/ with it alone would pass as
        // complete on the next tap, which would then start the app without libmain.so.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try #require(try await library.importUnit(
            Sets.unit(["base.apk": Self.base(["libhelper.so"]), "arm64.apk": Self.arm64], in: sandbox.paths)).first?.entry)
        try FileManager.default.removeItem(at: sandbox.paths.libraryDirectory(for: entry))
        let split = sandbox.paths.versionDirectory(for: entry).appendingPathComponent(try #require(entry.splits.first).fileName)
        try SplitAPK.make(split: "config.arm64_v8a").write(to: split, options: .atomic) // in place of the read-only file

        for _ in 1...2 {
            await #expect(throws: (any Error).self) { try await library.prepareNativeCode(id: entry.id) }
            #expect(!(try Sets.versionFiles(entry, sandbox.paths)).contains { $0.hasPrefix("lib") })
        }
    }

    @Test func aSetWhoseLibraryIsRefusedOnTheFirstTapKeepsNoAPK() async throws {
        // As at import (ADR-0032): the version keeps neither its APKs nor the code.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try #require(try await library.importUnit(
            Sets.unit(["base.apk": Self.base(), "arm64.apk": Self.arm64], in: sandbox.paths)).first?.entry)

        await #expect(throws: ConversionRefused.self) {
            try await library.resign(id: entry.id, fingerprint: "bb", with: Native.Converter(refuses: "libmain.so"))
        }

        let refused = try #require(await library.entries.first)
        #expect(refused.state == .incompatible)
        #expect(refused.base == nil && refused.splits.isEmpty)
        let stored = try Sets.versionFiles(entry, sandbox.paths)
        #expect(!stored.contains { $0.hasSuffix(".apk") || $0 == "lib" })
    }
}
