import APKFormats
import APKKit
import APKTestSupport
import Foundation
import Testing

@testable import APKLibrary

/// APKMirror's plain APKM (ADR-0049 step e): a ZIP with `info.json` beside the app's APKs, whose set is chosen as a
/// loose set's is (decision 3); `info.json` decides nothing. Its layout follows public descriptions, not a spec or a
/// sample [unverified]: the last check is an APKM the user owns (R5). An encrypted one is ContainerImportTests'.
@Suite("APKM import")
struct APKMImportTests {
    typealias Sandbox = LibraryTests.Sandbox
    typealias Sets = SetImportTests

    /// `info.json` with APKMirror's keys as public descriptions name them [unverified].
    static func info(package: String = SplitAPK.package, versionCode: Int = 1) -> Data {
        Data(#"{"apkm_version": 5, "app_name": "Sets", "pname": "\#(package)", "versioncode": "\#(versionCode)"}"#.utf8)
    }

    static let x86 = SplitAPK.make(split: "config.x86_64", abis: ["x86_64"])

    @Test(arguments: ["game.apkm", "game.zip"])
    func aPlainAPKMInstallsTheSetALooseSetWouldGive(_ name: String) async throws {
        // Its content decides, not its extension (decision 1): renamed, it is the same APKM.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let data = ContainerImportTests.zip([
            ("info.json", Self.info()), ("icon.png", Data([0x89, 0x50])), ("base.apk", Sets.base),
            ("split_config.arm64_v8a.apk", Sets.arm64), ("split_config.x86_64.apk", Self.x86),
            ("split_config.xxhdpi.apk", Sets.xxhdpi), ("split_config.xxxhdpi.apk", Sets.xxxhdpi)])
        let unit = try Sets.unit([name: data], in: sandbox.paths)

        let results = try await library.importUnit(unit)

        let entry = try #require(results.first?.entry)
        #expect(results.map(\.decision) == [.install])
        #expect(entry.id == SplitAPK.package && entry.state == .ready)
        #expect(entry.splits.map(\.splitName) == ["config.arm64_v8a", "config.xxhdpi"])
        #expect(entry.source.fileName == name && entry.source.size == UInt64(data.count))
        #expect(!sandbox.exists(unit))
    }

    @Test func itsInfoJSONDecidesNothing() async throws {
        // Decision 3: the APKs' manifests are what Android installs, whatever package and version info.json names.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let data = ContainerImportTests.zip([("info.json", Self.info(package: "org.example.other", versionCode: 99)),
                                             ("base.apk", Sets.base), ("split_config.arm64_v8a.apk", Sets.arm64)])

        let results = try await library.importUnit(Sets.unit(["game.apkm": data], in: sandbox.paths))

        let entry = try #require(results.first?.entry)
        #expect(results.count == 1)
        #expect(entry.id == SplitAPK.package && entry.state == .ready && entry.report?.versionCode == 1)
        #expect(await library.entries.map(\.id) == [SplitAPK.package])
    }

    @Test func itsAPKsCountInAFolderToo() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let data = ContainerImportTests.zip([("info.json", Self.info()), ("apks/base.apk", Sets.base),
                                             ("apks/split_config.arm64_v8a.apk", Sets.arm64)])

        let results = try await library.importUnit(Sets.unit(["game.apkm": data], in: sandbox.paths))

        let entry = try #require(results.first?.entry)
        #expect(entry.id == SplitAPK.package && entry.state == .ready)
        #expect(entry.splits.map(\.splitName) == ["config.arm64_v8a"])
    }

    @Test func anAPKMWithoutAnAPKSaysSo() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let data = ContainerImportTests.zip([("info.json", Self.info()), ("icon.png", Data([0x89, 0x50]))])
        let unit = try Sets.unit(["game.apkm": data], in: sandbox.paths)

        let results = try await library.importUnit(unit)

        let entry = try #require(results.first?.entry)
        #expect(results.count == 1)
        #expect(entry.displayName == "game.apkm" && entry.refusal == "It holds no APK")
        #expect(entry.state == .invalid("It holds no APK"))
        #expect(!sandbox.exists(unit))
    }
}
