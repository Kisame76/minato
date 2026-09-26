import APKKit
import APKSigning
import APKTestSupport
import Foundation
import Testing

@testable import APKLibrary

/// ADR-0032, decision 4 and test 3: the import extracts a NativeActivity app's native code into the lib/
/// folder of its version, converts and signs each library, and records the certificate.
@Suite("Native code at import")
struct NativeCodeTests {
    typealias Sandbox = LibraryTests.Sandbox

    /// Writes "<fingerprint>:<ELF contents>" where the container goes; refuses or fails on a chosen file.
    struct Converter: ContainerConverting {
        var fingerprint = "aa"
        var refuses: String?
        var fails: String?

        func convert(_ elf: URL, to container: URL) throws {
            if elf.lastPathComponent == refuses { throw ConversionRefused(reason: "the file has text relocations") }
            if elf.lastPathComponent == fails { throw CocoaError(.fileWriteUnknown) }
            try (Data("\(fingerprint):".utf8) + Data(contentsOf: elf)).write(to: container)
        }
    }

    static func signing(_ converter: Converter = Converter()) -> ImportSigning {
        ImportSigning(converter: converter, fingerprint: converter.fingerprint)
    }

    /// An AArch64 library whose bytes name it.
    static func library(_ name: String) -> Data {
        ELFBuilder(segments: [.load(align: 0x4000), .load(align: 0x4000)]).build() + Data(name.utf8)
    }

    /// The compiled manifest of a NativeActivity app that loads libactivity-test.so.
    static func manifest(extractNativeLibs: Bool, versionCode: Int = 1) throws -> Data {
        let text = """
            <manifest xmlns:android="http://schemas.android.com/apk/res/android"
                package="io.github.kisame76.minato.test.activity" android:versionCode="\(versionCode)" android:versionName="1.\(versionCode)">
              <uses-sdk android:minSdkVersion="29" android:targetSdkVersion="35"/>
              <application android:label="Activity Test" android:hasCode="false" android:extractNativeLibs="\(extractNativeLibs)">
                <activity android:name="android.app.NativeActivity" android:exported="true">
                  <meta-data android:name="android.app.lib_name" android:value="activity-test"/>
                  <intent-filter>
                    <action android:name="android.intent.action.MAIN"/>
                    <category android:name="android.intent.category.LAUNCHER"/>
                  </intent-filter>
                </activity>
              </application>
            </manifest>
            """
        return try ManifestCompiler.compile(Data(text.utf8), file: "AndroidManifest.xml")
    }

    /// make-apk's debug key, made once in a temporary folder that the next run uses again.
    static let key: DebugKey = {
        try! DebugKey.load(from: URL.temporaryDirectory.appending(path: "native-code-test-key", directoryHint: .isDirectory))
    }()

    /// A NativeActivity app as make-apk packs it (ADR-0031): the manifest compiled, the APK laid out by
    /// APKPack and signed with the debug key, so that its versions update each other (ADR-0034).
    static func madeAPK(extractNativeLibs: Bool, versionCode: Int = 1,
                        libraries: [String] = ["libactivity-test.so", "libhelper.so"]) throws -> Data {
        try APKSigner.sign(
            APKLayout.build(manifest: manifest(extractNativeLibs: extractNativeLibs, versionCode: versionCode),
                            libraries: libraries.map { PackFile(name: $0, contents: library($0)) }, assets: []),
            with: key)
    }

    /// A NativeActivity app with these entries in lib/arm64-v8a/, zipped by hand: names and duplicates
    /// that make-apk refuses get in.
    static func handMadeAPK(libraries: [String]) throws -> Data {
        var zip = ZipBuilder()
        zip.add("AndroidManifest.xml", try manifest(extractNativeLibs: true))
        for name in libraries {
            zip.add("lib/arm64-v8a/\(name)", library(name))
        }
        return zip.build().data
    }

    static func names(in folder: URL) -> [String] {
        ((try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? []).sorted()
    }

    // MARK: - Converted and signed

    @Test(arguments: [false, true])
    func convertsAndSignsEveryLibraryOfAMadeAPK(extractNativeLibs: Bool) async throws {
        // Stored libraries with extractNativeLibs="false", deflated ones with "true".
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)

        let result = try await library.importFile(
            sandbox.file(Self.madeAPK(extractNativeLibs: extractNativeLibs), named: "activity-test.apk"), signing: Self.signing())

        #expect(result.entry.state == .ready)
        #expect(result.entry.signedWith == "aa")
        let lib = sandbox.paths.libraryDirectory(for: result.entry)
        #expect(Self.names(in: lib) == ["libactivity-test.so", "libactivity-test.so.dylib", "libhelper.so", "libhelper.so.dylib"])
        for name in ["libactivity-test.so", "libhelper.so"] {
            #expect(try Data(contentsOf: lib.appendingPathComponent(name)) == Self.library(name))
            #expect(try Data(contentsOf: lib.appendingPathComponent(name + ".dylib")) == Data("aa:".utf8) + Self.library(name))
        }
        // Signing again (ADR-0023) finds what the import converted.
        #expect(await library.resignCandidates() == [ResignCandidate(id: result.entry.id, signedWith: "aa", containerCount: 2)])
        #expect(try await Library(paths: sandbox.paths).entries.first?.signedWith == "aa")
    }

    @Test func convertsTheLibraryOfASampleAPK() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let apk = SampleAPK.make(nativeActivity: true)

        let result = try await library.importFile(sandbox.file(apk, named: "game.apk"), signing: Self.signing())

        let lib = sandbox.paths.libraryDirectory(for: result.entry)
        #expect(result.entry.signedWith == "aa")
        #expect(Self.names(in: lib) == ["libgame.so", "libgame.so.dylib"])
        let original = try APKKit.ZipArchive(data: apk).data(named: "lib/arm64-v8a/libgame.so")
        #expect(try Data(contentsOf: lib.appendingPathComponent("libgame.so")) == original)
    }

    @Test func anUpdateConvertsIntoTheNewVersionsFolder() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let first = try await library.importFile(
            sandbox.file(Self.madeAPK(extractNativeLibs: false), named: "v1.apk"), signing: Self.signing()).entry

        let result = try await library.importFile(
            sandbox.file(Self.madeAPK(extractNativeLibs: false, versionCode: 2), named: "v2.apk"), signing: Self.signing())

        #expect(result.decision == .update)
        #expect(result.entry.signedWith == "aa")
        #expect(Self.names(in: sandbox.paths.libraryDirectory(for: result.entry)).count == 4)
        #expect(!sandbox.exists(sandbox.paths.versionDirectory(for: first)))
    }

    // MARK: - Nothing converted

    @Test func keepsOnlyTheOriginalsWithoutACertificate() async throws {
        // The first tap signs them (ADR-0032, decision 3).
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)

        let result = try await library.importFile(sandbox.file(Self.madeAPK(extractNativeLibs: false), named: "activity-test.apk"))

        #expect(result.entry.state == .ready)
        #expect(result.entry.signedWith == nil)
        #expect(Self.names(in: sandbox.paths.libraryDirectory(for: result.entry)) == ["libactivity-test.so", "libhelper.so"])
        #expect(await library.resignCandidates() == [ResignCandidate(id: result.entry.id, signedWith: nil, containerCount: 2)])
    }

    @Test func aSigningErrorKeepsTheOriginalsUnsigned() async throws {
        // Not a refusal: the code may run, only it could not be signed now; the first tap tries again.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)

        let result = try await library.importFile(
            sandbox.file(Self.madeAPK(extractNativeLibs: false), named: "activity-test.apk"),
            signing: Self.signing(Converter(fails: "libhelper.so")))

        // The originals only, as without a certificate: the container made before the error goes too.
        #expect(result.entry.state == .ready)
        #expect(result.entry.signedWith == nil)
        #expect(Self.names(in: sandbox.paths.libraryDirectory(for: result.entry)) == ["libactivity-test.so", "libhelper.so"])
    }

    @Test func aSigningErrorStillChecksTheOtherLibraries() async throws {
        // A later library that cannot run must not slip through as ready.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)

        let result = try await library.importFile(
            sandbox.file(Self.madeAPK(extractNativeLibs: false), named: "activity-test.apk"),
            signing: Self.signing(Converter(refuses: "libhelper.so", fails: "libactivity-test.so")))

        #expect(result.entry.state == .incompatible)
        #expect(result.entry.report?.compatibility.issues.contains(
            .conversionFailed(library: "libhelper.so", reason: "the file has text relocations")) == true)
    }

    @Test func checksTheCodeWithoutACertificate() async throws {
        // Converted without a certificate only to learn whether it can run: the verdict does not depend on
        // having one at import, and the containers go again (ADR-0032).
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)

        let checked = try await library.importFile(
            sandbox.file(Self.madeAPK(extractNativeLibs: false), named: "a.apk"),
            signing: ImportSigning(converter: Converter(), fingerprint: nil))

        #expect(checked.entry.state == .ready)
        #expect(checked.entry.signedWith == nil)
        #expect(Self.names(in: sandbox.paths.libraryDirectory(for: checked.entry)) == ["libactivity-test.so", "libhelper.so"])

        let refused = try await library.importFile(
            sandbox.file(Self.madeAPK(extractNativeLibs: false, versionCode: 2), named: "b.apk"),
            signing: ImportSigning(converter: Converter(refuses: "libhelper.so"), fingerprint: nil))

        // Refused without a certificate too; as an update of a ready version it is rejected (ADR-0034).
        #expect(refused.decision == .reject(.incompatible))
        #expect(refused.report?.compatibility.issues.contains(
            .conversionFailed(library: "libhelper.so", reason: "the file has text relocations")) == true)
    }

    @Test func anAppThatNeedsSplitsGetsNothingConverted() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)

        let result = try await library.importFile(
            sandbox.file(SampleAPK.make(splitRequired: true, nativeActivity: true), named: "game.apk"), signing: Self.signing())

        #expect(result.entry.state == .needsSplits)
        #expect(!sandbox.exists(sandbox.paths.libraryDirectory(for: result.entry)))
    }

    @Test func extractsOnlyTheLibrariesAndroidExtracts() async throws {
        // Android's installer skips other names (NativeLibraryHelper: lib*.so of A-Z, a-z, 0-9 and + , - . = _).
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let apk = try Self.handMadeAPK(libraries: ["libactivity-test.so", "libodd name.so", "helper.so", "libcafé.so"])

        let result = try await library.importFile(sandbox.file(apk, named: "activity-test.apk"), signing: Self.signing())

        #expect(result.entry.state == .ready)
        #expect(Self.names(in: sandbox.paths.libraryDirectory(for: result.entry)) == ["libactivity-test.so", "libactivity-test.so.dylib"])
    }

    @Test func aLibraryInTheAPKTwiceMakesTheAppIncompatible() async throws {
        // Android refuses an APK with two entries of the same name.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let apk = try Self.handMadeAPK(libraries: ["libactivity-test.so", "libactivity-test.so"])

        let result = try await library.importFile(sandbox.file(apk, named: "activity-test.apk"), signing: Self.signing())

        #expect(result.entry.state == .incompatible)
        #expect(result.entry.report?.compatibility.issues.contains(
            .conversionFailed(library: "libactivity-test.so", reason: "it is in the APK twice")) == true)
    }

    @Test func aJavaAppGetsNothingConverted() async throws {
        // Its conversion comes with M4.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)

        let result = try await library.importFile(sandbox.file(SampleAPK.make(), named: "game.apk"), signing: Self.signing())

        #expect(result.entry.report?.runtime == .art)
        #expect(result.entry.signedWith == nil)
        #expect(!sandbox.exists(sandbox.paths.libraryDirectory(for: result.entry)))
    }

    // MARK: - Refused

    @Test func aLibraryTheConverterRefusesMakesTheAppIncompatible() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)

        let result = try await library.importFile(
            sandbox.file(Self.madeAPK(extractNativeLibs: false), named: "activity-test.apk"),
            signing: Self.signing(Converter(refuses: "libhelper.so")))

        #expect(result.decision == .install)
        #expect(result.entry.state == .incompatible)
        #expect(result.entry.report?.compatibility.status == .incompatible)
        #expect(result.entry.report?.compatibility.issues.contains(
            .conversionFailed(library: "libhelper.so", reason: "the file has text relocations")) == true)
        // An incompatible version keeps neither its APK nor its code.
        #expect(result.entry.base == nil)
        #expect(result.entry.signedWith == nil)
        #expect(Self.names(in: sandbox.paths.versionDirectory(for: result.entry)).allSatisfy { !$0.hasPrefix("base.apk") && $0 != "lib" })
    }

    @Test func aRefusedUpdateLeavesTheReadyVersion() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let installed = try await library.importFile(
            sandbox.file(Self.madeAPK(extractNativeLibs: false), named: "v1.apk"), signing: Self.signing()).entry
        let picked = try await library.adopt(sandbox.file(Self.madeAPK(extractNativeLibs: false, versionCode: 2), named: "v2.apk"))
        let recorder = ProgressRecorder()

        let result = try await library.importFile(picked, signing: Self.signing(Converter(refuses: "libhelper.so"))) {
            recorder.add($0)
        }

        #expect(recorder.values.last == 1)
        #expect(result.decision == .reject(.incompatible))
        #expect(result.entry == installed)
        #expect(await library.entries == [installed])
        #expect(Self.names(in: sandbox.paths.installDirectory(for: installed.id)) == [installed.folder].compactMap { $0 })
        #expect(Self.names(in: sandbox.paths.libraryDirectory(for: installed)).count == 4)
        #expect(result.report?.compatibility.issues.contains(
            .conversionFailed(library: "libhelper.so", reason: "the file has text relocations")) == true)
        #expect(!sandbox.exists(picked))
    }

    @Test func aDamagedLibraryMakesTheAppIncompatible() async throws {
        // Its last byte no longer matches the CRC: importing the file again would fail the same way.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        var apk = try Self.madeAPK(extractNativeLibs: false)
        let archive = try ZipArchive(data: apk)
        let entry = try #require(archive.entry(named: "lib/arm64-v8a/libhelper.so"))
        let last = try Int(archive.dataOffset(of: entry) + entry.compressedSize) - 1
        apk[last] ^= 0xFF

        let result = try await library.importFile(sandbox.file(apk, named: "activity-test.apk"), signing: Self.signing())

        #expect(result.entry.state == .incompatible)
        #expect(result.entry.report?.compatibility.issues.contains(
            .conversionFailed(library: "libhelper.so", reason: "its data in the APK is damaged")) == true)
    }

    @Test func aRefusedUpdateOverAnAppThatCannotRunReplacesIt() async throws {
        // Only a ready version is kept (ADR-0034, decision 1).
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let installed = try await library.importFile(
            sandbox.file(SampleAPK.make(package: "io.github.kisame76.minato.test.activity", splitRequired: true, nativeActivity: true,
                                        signers: [Self.key.certificate]),
                         named: "v1.apk"), signing: Self.signing()).entry
        #expect(installed.state == .needsSplits)

        let result = try await library.importFile(
            sandbox.file(Self.madeAPK(extractNativeLibs: false, versionCode: 2), named: "v2.apk"),
            signing: Self.signing(Converter(refuses: "libhelper.so")))

        #expect(result.decision == .update)
        #expect(result.entry.state == .incompatible)
        #expect(result.entry.report?.versionCode == 2)
        #expect(Self.names(in: sandbox.paths.installDirectory(for: installed.id)) == [result.entry.folder].compactMap { $0 })
    }

    @Test func signingAgainGivesTheSameVerdict() async throws {
        // Imported unsigned, refused when signed later (ADR-0023): incompatible, as at import.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try await library.importFile(sandbox.file(Self.madeAPK(extractNativeLibs: false), named: "activity-test.apk")).entry

        await #expect(throws: ConversionRefused(reason: "the file has text relocations")) {
            try await library.resign(id: entry.id, fingerprint: "aa", with: Converter(refuses: "libhelper.so"))
        }

        let refused = try #require(await library.entries.first)
        #expect(refused.state == .incompatible)
        #expect(refused.base == nil)
        #expect(refused.signedWith == nil)
        #expect(refused.report?.compatibility.issues.contains(
            .conversionFailed(library: "libhelper.so", reason: "the file has text relocations")) == true)
        #expect(Self.names(in: sandbox.paths.versionDirectory(for: refused)).allSatisfy { $0 != "lib" && $0 != "base.apk" })
        #expect(await library.resignCandidates() == [ResignCandidate(id: entry.id, signedWith: nil, containerCount: 0)])
        #expect(try await Library(paths: sandbox.paths).entries.first?.state == .incompatible)
    }

    // MARK: - What counts as a refusal

    @Test func aDamagedOrTooLargeLibraryCannotRunButAReadErrorIsNoVerdict() {
        #expect(RefusedLibrary(readError: APKError.crcMismatch("x"), library: "libx.so")?.reason == "its data in the APK is damaged")
        #expect(RefusedLibrary(readError: APKError.truncated("x"), library: "libx.so")?.reason == "its data in the APK is damaged")
        #expect(RefusedLibrary(readError: APKError.encryptedEntry("x"), library: "libx.so")?.reason == "its data in the APK is damaged")
        #expect(RefusedLibrary(readError: APKError.unsupportedCompression(entry: "x", method: 12), library: "libx.so")?.reason
            == "its data in the APK is damaged")
        #expect(RefusedLibrary(readError: APKError.entryTooLarge("x", 300 << 20), library: "libx.so")?.reason
            == "it is larger than 256 MiB, which Minato cannot extract yet")
        // Reading the file failed, which may not happen again: no verdict about the library.
        #expect(RefusedLibrary(readError: CocoaError(.fileReadUnknown), library: "libx.so") == nil)
    }

    @Test func readsAPKConvertsRefusal() {
        #expect(ConversionRefused(apkConvertMessage: "incompatible: machine x86-64: only AArch64 (arm64-v8a) code runs")
            == ConversionRefused(reason: "machine x86-64: only AArch64 (arm64-v8a) code runs"))
        #expect(ConversionRefused(apkConvertMessage: "cannot open /x/libgame.so: No such file or directory") == nil)
        #expect(ConversionRefused(reason: "the file has text relocations").localizedDescription == "the file has text relocations")
    }

    // MARK: - Before the first tap (ADR-0032, decision 3)

    @Test func extractsTheCodeOfAnEntryImportedWithoutIt() async throws {
        // Entries imported before step a) kept only their base.apk; the first tap needs the originals.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try await library.importFile(sandbox.file(Self.madeAPK(extractNativeLibs: false), named: "activity-test.apk")).entry
        let lib = sandbox.paths.libraryDirectory(for: entry)
        try FileManager.default.removeItem(at: lib)

        #expect(try await library.prepareNativeCode(id: entry.id) == 2)

        #expect(Self.names(in: lib) == ["libactivity-test.so", "libhelper.so"])
        #expect(try Data(contentsOf: lib.appendingPathComponent("libhelper.so")) == Self.library("libhelper.so"))
        #expect(await library.resignCandidates() == [ResignCandidate(id: entry.id, signedWith: nil, containerCount: 2)])
    }

    @Test func aDamagedLibraryFoundAtTheFirstTapMakesTheAppIncompatible() async throws {
        // Its data in base.apk no longer matches the CRC: the tap explains why the app cannot run.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try await library.importFile(sandbox.file(Self.madeAPK(extractNativeLibs: false), named: "activity-test.apk")).entry
        let lib = sandbox.paths.libraryDirectory(for: entry)
        try FileManager.default.removeItem(at: lib)
        let base = sandbox.paths.versionDirectory(for: entry).appendingPathComponent("base.apk")
        var apk = try Data(contentsOf: base)
        let archive = try ZipArchive(data: apk)
        let helper = try #require(archive.entry(named: "lib/arm64-v8a/libhelper.so"))
        apk[try Int(archive.dataOffset(of: helper) + helper.compressedSize) - 1] ^= 0xFF
        try apk.write(to: base)

        await #expect(throws: ConversionRefused(reason: "its data in the APK is damaged")) {
            try await library.prepareNativeCode(id: entry.id)
        }

        let refused = try #require(await library.entries.first)
        #expect(refused.state == .incompatible)
        #expect(refused.report?.compatibility.issues.contains(
            .conversionFailed(library: "libhelper.so", reason: "its data in the APK is damaged")) == true)
        #expect(!sandbox.exists(base))
        #expect(!sandbox.exists(lib))
    }

    @Test func leavesCodeThatIsThereAsItIs() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try await library.importFile(
            sandbox.file(Self.madeAPK(extractNativeLibs: false), named: "activity-test.apk"), signing: Self.signing()).entry
        let lib = sandbox.paths.libraryDirectory(for: entry)
        // Not extracted again: the originals stay whatever they are.
        let helper = lib.appendingPathComponent("libhelper.so")
        try Data("kept".utf8).write(to: helper)

        #expect(try await library.prepareNativeCode(id: entry.id) == 2)

        #expect(Self.names(in: lib).count == 4)
        #expect(try Data(contentsOf: helper) == Data("kept".utf8))
        #expect(await library.entries.first?.signedWith == "aa")
    }

    @Test(arguments: [false, true])
    func extractsNothingForAnAppThatDoesNotStart(needsSplits: Bool) async throws {
        // A Java app gets its code with M4; one that needs its splits cannot start before M5.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let apk = needsSplits ? SampleAPK.make(splitRequired: true, nativeActivity: true) : SampleAPK.make()
        let entry = try await library.importFile(sandbox.file(apk, named: "game.apk")).entry

        #expect(try await library.prepareNativeCode(id: entry.id) == 0)

        #expect(!sandbox.exists(sandbox.paths.libraryDirectory(for: entry)))
    }

    @Test func readsTheEntriesBeforeTheLibraryOpens() async throws {
        // At launch, before anything else, to choose between game mode and the library (ADR-0032, decision 1).
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let entry = try await Library(paths: sandbox.paths).importFile(
            sandbox.file(Self.madeAPK(extractNativeLibs: false), named: "activity-test.apk")).entry

        #expect(try Library.entries(in: sandbox.paths).map(\.id) == [entry.id])
        #expect(try Library.entries(in: sandbox.paths).first?.folder == entry.folder)
        #expect(try Library.entries(in: LibraryTests.Sandbox().paths).isEmpty)
    }

    // MARK: - Removal

    @Test func deletingRemovesItAll() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entry = try await library.importFile(
            sandbox.file(Self.madeAPK(extractNativeLibs: false), named: "activity-test.apk"), signing: Self.signing()).entry

        try await library.remove(id: entry.id)

        #expect(!sandbox.exists(sandbox.paths.installDirectory(for: entry.id)))
        #expect(!sandbox.exists(sandbox.paths.dataDirectory(for: entry.id)))
    }
}
