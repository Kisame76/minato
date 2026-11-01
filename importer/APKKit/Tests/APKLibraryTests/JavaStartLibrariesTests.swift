import APKKit
import APKPack
import APKTestSupport
import Foundation
import Testing

@testable import APKLibrary

/// ADR-0055 decision 2, from the APK itself: the libraries that the app's Java code names in a string constant are on
/// its start path, as System.loadLibrary("game") or an SDL activity's list of libraries loads them at the start. One of
/// them that cannot load blocks the app with its reasons; any other degrades, and the entry names it for the status
/// line.
@Suite("Java start libraries")
struct JavaStartLibrariesTests {
    typealias Sandbox = LibraryTests.Sandbox
    typealias Native = NativeCodeTests
    typealias Checker = NativeCodeFindingsTests.Checker

    static let two = NativeCodeFindingsTests.two
    static let twoText = NativeCodeFindingsTests.twoText

    /// A Java app with the dex and the arm64 libraries given.
    static func apk(dex: Data, libraries: [String], dexName: String = "classes.dex") throws -> Data {
        let text = """
            <manifest xmlns:android="http://schemas.android.com/apk/res/android" package="org.example.javagame"
                android:versionCode="1" android:versionName="1.0">
              <uses-sdk android:minSdkVersion="29" android:targetSdkVersion="35"/>
              <application android:label="Java Game">
                <activity android:name="org.example.javagame.GameActivity" android:exported="true">
                  <intent-filter>
                    <action android:name="android.intent.action.MAIN"/>
                    <category android:name="android.intent.category.LAUNCHER"/>
                  </intent-filter>
                </activity>
              </application>
            </manifest>
            """
        var zip = ZipBuilder()
        zip.add("AndroidManifest.xml", try ManifestCompiler.compile(Data(text.utf8), file: "AndroidManifest.xml"))
        if dexName != "classes.dex" {
            zip.add("classes.dex", DexBuilder.make(strings: [], methods: [DexBuilder.returnVoid]))
        }
        zip.add(dexName, dex)
        for name in libraries {
            zip.add("lib/arm64-v8a/\(name)", Native.library(name))
        }
        return zip.build().data
    }

    static func importing(_ apk: Data, refusing: [String]) async throws -> (LibraryEntry, Sandbox) {
        let sandbox = try Sandbox()
        let library = try Library(paths: sandbox.paths)
        await library.use(checker: Checker(reasons: Dictionary(uniqueKeysWithValues: refusing.map { ($0, Self.two) })))
        let result = try await library.importFile(sandbox.file(apk, named: "game.apk"), signing: Native.signing())
        return (result.entry, sandbox)
    }

    @Test func aLibraryTheCodeLoadsByNameBlocks() async throws {
        let dex = DexBuilder.make(strings: ["game", "helper-not-loaded"],
                                  methods: [DexBuilder.constString(0, 0) + DexBuilder.returnVoid])
        let (entry, sandbox) = try await Self.importing(
            try Self.apk(dex: dex, libraries: ["libgame.so", "libhelper.so"]), refusing: ["libgame.so", "libhelper.so"])
        defer { sandbox.remove() }

        #expect(entry.state == .incompatible)
        let issues = entry.report?.compatibility.issues ?? []
        #expect(issues.contains(.conversionFailed(library: "libgame.so", reason: Self.twoText)))
        #expect(issues.contains(.libraryDegraded(library: "libhelper.so", reason: Self.twoText)))
    }

    @Test(arguments: ["libgame.so", "/data/app/org.example.javagame/lib/arm64/libgame.so"])
    func aFileNameOrPathTheCodeLoadsBlocks(constant: String) async throws {
        // System.load takes a path, System.loadLibrary a name; both name the library.
        let dex = DexBuilder.make(strings: [constant],
                                  methods: [DexBuilder.constStringJumbo(1, 0) + DexBuilder.returnVoid])
        let (entry, sandbox) = try await Self.importing(try Self.apk(dex: dex, libraries: ["libgame.so"]),
                                                        refusing: ["libgame.so"])
        defer { sandbox.remove() }

        #expect(entry.state == .incompatible)
    }

    @Test func aConstantAfterAnInvokeAndInALaterDexCounts() async throws {
        // invoke-static is three units long, its last one here looking like a const-string; the code is in
        // classes2.dex, as a multidex app's often is.
        let invoke: [UInt16] = [0x1071, 0x0000, 0x011A]
        let dex = DexBuilder.make(strings: ["game"],
                                  methods: [invoke + DexBuilder.constString(0, 0) + DexBuilder.returnVoid])
        let (entry, sandbox) = try await Self.importing(
            try Self.apk(dex: dex, libraries: ["libgame.so"], dexName: "classes2.dex"), refusing: ["libgame.so"])
        defer { sandbox.remove() }

        #expect(entry.state == .incompatible)
    }

    @Test func aNameTheCodeNeverLoadsStaysOffTheStartPath() async throws {
        // The string is in the table, as a class's or a member's name is, but no instruction loads it; nor does a
        // payload's data that looks like const-string of it.
        let payload = DexBuilder.byteArrayPayload([0x1A, 0x00, 0x00, 0x00])
        let dex = DexBuilder.make(strings: ["game"], methods: [DexBuilder.returnVoid + [0x0000] + payload])
        let (entry, sandbox) = try await Self.importing(try Self.apk(dex: dex, libraries: ["libgame.so"]),
                                                        refusing: ["libgame.so"])
        defer { sandbox.remove() }

        #expect(entry.state == .ready)
        #expect(entry.degradedLibraries == ["libgame.so"])
    }

    /// Ren'Py's core library is on the start path by its engine, whatever the dex names: PythonSDLActivity loads it
    /// at the start through getLibraries() (renpy-build's RAPT since 7.4, "renpython"), and nothing runs without it.
    @Test func renPysCoreLibraryBlocksWithoutAConstant() async throws {
        let dex = DexBuilder.make(strings: [], methods: [DexBuilder.returnVoid])
        let (entry, sandbox) = try await Self.importing(try Self.apk(dex: dex, libraries: ["librenpython.so"]),
                                                        refusing: ["librenpython.so"])
        defer { sandbox.remove() }

        #expect(entry.state == .incompatible)
        #expect(entry.report?.compatibility.issues.contains(
            .conversionFailed(library: "librenpython.so", reason: Self.twoText)) == true)
    }

    @Test func aReadyAppNamesWhatItRunsWithout() async throws {
        let dex = DexBuilder.make(strings: ["game"], methods: [DexBuilder.constString(0, 0) + DexBuilder.returnVoid])
        let (entry, sandbox) = try await Self.importing(
            try Self.apk(dex: dex, libraries: ["libgame.so", "libhelper.so", "libextra.so"]),
            refusing: ["libhelper.so", "libextra.so"])
        defer { sandbox.remove() }

        #expect(entry.state == .ready)
        #expect(entry.degradedLibraries == ["libextra.so", "libhelper.so"])
    }
}
