import APKFormats
import APKKit
import APKTestSupport
import Foundation
import Testing

@testable import APKLibrary

/// Containers in a unit (ADR-0049 a3): a file that is no APK by its content gives its APKs, extracted next to it in the
/// background import, and they join the unit's own; one Minato refuses becomes an entry of its own with the reason.
@Suite("Container import")
struct ContainerImportTests {
    typealias Sandbox = LibraryTests.Sandbox
    typealias Sets = SetImportTests

    static func zip(_ entries: [(String, Data)]) -> Data {
        var zip = ZipBuilder()
        for (name, content) in entries { zip.add(name, content, method: .stored) }
        return zip.build().data
    }

    static let v7a = SplitAPK.make(split: "config.armeabi_v7a", abis: ["armeabi-v7a"])

    /// bundletool's toc.json for one split variant: the base with an arm64-v8a and an armeabi-v7a split.
    static let toc = """
        {"packageName": "org.example.sets", "bundletool": {"version": "1.18.3"}, "variant": [{
          "targeting": {"sdkVersionTargeting": {"value": [{"min": 21}]}},
          "apkSet": [{"moduleMetadata": {"name": "base", "deliveryType": "INSTALL_TIME"}, "apkDescription": [
            {"path": "splits/base-master.apk", "splitApkMetadata": {"isMasterSplit": true}},
            {"path": "splits/base-arm64_v8a.apk", "splitApkMetadata": {"splitId": "config.arm64_v8a"},
             "targeting": {"abiTargeting": {"value": [{"alias": "ARM64_V8A"}], "alternatives": [{"alias": "ARMEABI_V7A"}]}}},
            {"path": "splits/base-armeabi_v7a.apk", "splitApkMetadata": {"splitId": "config.armeabi_v7a"},
             "targeting": {"abiTargeting": {"value": [{"alias": "ARMEABI_V7A"}], "alternatives": [{"alias": "ARM64_V8A"}]}}}]}]}]}
        """

    static func container(_ layout: String, base: Data = Sets.base, arm64: Data = Sets.arm64) -> Data {
        switch layout {
        case "SAI":
            zip([("base.apk", base), ("split_config.arm64_v8a.apk", arm64), ("icon.png", Data([0x89])),
                 ("meta.sai_v2.json", Data("{}".utf8))])
        case "App Manager":
            zip([("info.json", Data(#"{"info_version": 1}"#.utf8)), ("base.apk", base), ("split_config.arm64_v8a.apk", arm64)])
        case "a zipped folder":
            zip([("game/base.apk", base), ("game/arm64.apk", arm64), ("__MACOSX/game/._base.apk", Data([0]))])
        default:
            zip([("toc.json", Data(toc.utf8)), ("splits/base-master.apk", base), ("splits/base-arm64_v8a.apk", arm64),
                 ("splits/base-armeabi_v7a.apk", v7a)])
        }
    }

    @Test(arguments: ["SAI", "App Manager", "a zipped folder", "bundletool"])
    func aContainerGivesItsSetAsOneEntry(_ layout: String) async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let data = Self.container(layout)
        let unit = try Sets.unit(["game.apks": data], in: sandbox.paths)
        #expect(try await library.scanUnits().map(\.lastPathComponent) == [unit.lastPathComponent])

        let results = try await library.importUnit(unit)

        let entry = try #require(results.first?.entry)
        #expect(results.map(\.decision) == [.install])
        #expect(entry.id == SplitAPK.package && entry.state == .ready)
        #expect(entry.splits.map(\.fileName) == ["split_config.arm64_v8a.apk"])
        // The entry names what the user picked: the container, not an APK taken out of it.
        #expect(entry.source.fileName == "game.apks" && entry.source.size == UInt64(data.count))
        #expect(try Sets.versionFiles(entry, sandbox.paths) == ["base.apk", "lib", "signers.json", "split_config.arm64_v8a.apk"])
        #expect(!sandbox.exists(unit))
    }

    @Test(arguments: [
        ("game.xapk", "manifest", "XAPK files aren’t supported yet"),
        ("game.apkm", "info", "APKM files aren’t supported yet"),
        ("game.apkm", "encrypted", "Encrypted APKM files aren’t supported"),
        ("game.apks", "no APK", "It holds no APK"),
        ("game.apks", "no ZIP", "It is neither an APK nor a container of APKs"),
    ])
    func aContainerMinatoRefusesGetsAnEntryWithTheReason(_ name: String, _ kind: String, _ refusal: String) async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let data: Data
        switch kind {
        case "manifest": data = Self.zip([("manifest.json", Data("{}".utf8)), ("org.example.sets.apk", Sets.base)])
        case "info": data = Self.zip([("info.json", Data(#"{"apkm_version": 5}"#.utf8)), ("base.apk", Sets.base)])
        case "no APK": data = Self.zip([("readme.txt", Data("x".utf8))])
        default: data = Data([0x00, 0x08, 0x00, 0x00]) + Data(repeating: 0x5A, count: 64)
        }
        let unit = try Sets.unit([name: data], in: sandbox.paths)

        let results = try await library.importUnit(unit)

        let entry = try #require(results.first?.entry)
        #expect(results.count == 1)
        #expect(entry.displayName == name && entry.refusal == refusal && entry.state == .invalid(refusal))
        #expect(entry.source.fileName == name)
        #expect(!sandbox.exists(unit))
    }

    @Test func anAPKAndADamagedContainerInOneUnitEachGetTheirEntry() async throws {
        // App's review: the APK installs, and the container's refusal stands beside it.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let unit = try Sets.unit(["game.apk": SampleAPK.make(), "broken.zip": Self.zip([("readme.txt", Data("x".utf8))])],
                                 in: sandbox.paths)

        let results = try await library.importUnit(unit)

        #expect(results.count == 2)
        #expect(results.contains { $0.entry.id == "org.example.game" && $0.entry.state == .ready })
        #expect(results.contains { $0.entry.displayName == "broken.zip" && $0.entry.refusal == "It holds no APK" })
        #expect(!sandbox.exists(unit))
    }

    @Test func twoContainersInOneUnitKeepTheirAPKsApart() async throws {
        // Each container's APKs are named after it, "a.apks/1.apk", so the same "1.apk" of two never meet.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let other = SampleAPK.make(package: "org.example.other")
        let unit = try Sets.unit(["a.apks": Self.container("SAI"), "b.zip": Self.zip([("other.apk", other)])], in: sandbox.paths)

        let results = try await library.importUnit(unit)

        #expect(Set(results.map(\.entry.id)) == [SplitAPK.package, "org.example.other"])
        #expect(results.allSatisfy { $0.entry.state == .ready })
        #expect(Set(results.map(\.entry.source.fileName)) == ["a.apks", "b.zip"])
        #expect(!sandbox.exists(unit))
    }

    @Test func anInterruptedExtractionStartsAgain() async throws {
        // A ".partial" folder an earlier launch left goes, and the extraction runs again (ADR-0049 decision 6).
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let unit = try Sets.unit(["game.apks": Self.container("SAI")], in: sandbox.paths)
        let partial = unit.appendingPathComponent(".game.apks.contents.partial", isDirectory: true)
        try FileManager.default.createDirectory(at: partial, withIntermediateDirectories: true)
        try Data("half".utf8).write(to: partial.appendingPathComponent("1.apk"))

        let results = try await library.importUnit(unit)

        #expect(results.map(\.entry.state) == [.ready])
        #expect(!sandbox.exists(unit))
    }

    @Test func aCompleteExtractionIsTakenAsItIs() async throws {
        // The folder exists whole or not at all, so one an earlier import left complete, while a question waited, is
        // used again: here it holds version 2, while the container holds version 1.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let unit = try Sets.unit(["game.apks": Self.container("SAI")], in: sandbox.paths)
        let contents = unit.appendingPathComponent(".game.apks.contents", isDirectory: true)
        try FileManager.default.createDirectory(at: contents, withIntermediateDirectories: true)
        try SplitAPK.make(versionCode: 2).write(to: contents.appendingPathComponent("1.apk"))
        try SplitAPK.make(versionCode: 2, split: "config.arm64_v8a", abis: ["arm64-v8a"])
            .write(to: contents.appendingPathComponent("2.apk"))

        let results = try await library.importUnit(unit)

        #expect(results.first?.entry.report?.versionCode == 2)
        #expect(!sandbox.exists(unit))
    }

    @Test func aUnitWaitsForItsContainersButNotForHiddenFilesOrOBBs() async throws {
        // App's review: hidden files are no containers (.DS_Store, "._x", ".x.icloud"); an OBB waits for step d.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let waiting = try Sets.unit(["game.apks": Self.container("SAI")], in: sandbox.paths)
        _ = try Sets.unit([".DS_Store": Data([0]), "._game.apks": Data([0]), ".game.apks.icloud": Data([0]),
                           "main.1.org.example.sets.obb": Data([0])], in: sandbox.paths)

        #expect(try await library.scanUnits().map(\.lastPathComponent) == [waiting.lastPathComponent])
        // scan() still gives APKs only.
        #expect(try await library.scan().isEmpty)
    }
}
