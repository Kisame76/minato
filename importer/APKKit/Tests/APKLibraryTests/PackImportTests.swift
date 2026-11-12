import APKFormats
import APKKit
import APKTestSupport
import Foundation
import Testing

@testable import APKLibrary

/// The asset packs Play fetches after the install, fast-follow and on-demand (ADR-0049 decision 2, format 6): the
/// slices bundletool's choice takes are kept in the version folder's packs/ under bundletool's names, each checked
/// against the set's package, versionCode and signer; install-time packs stay splits.
@Suite("Pack import")
struct PackImportTests {
    typealias Sandbox = LibraryTests.Sandbox
    typealias Sets = SetImportTests

    /// bundletool's toc.json: the base with its arm64-v8a split, an install-time pack `start`, a fast-follow pack
    /// `level2` with an ASTC slice and, with `level3`, an on-demand pack `level3`.
    static func toc(level3: Bool = true) -> String {
        let onDemand = level3 ? """
            ,
            {"assetModuleMetadata": {"name": "level3", "deliveryType": "ON_DEMAND"}, "apkDescription": [
              {"path": "asset-slices/level3-master.apk", "assetSliceMetadata": {"isMasterSplit": true}}]}
            """ : ""
        return """
            {"packageName": "org.example.sets", "bundletool": {"version": "1.18.3"}, "variant": [{
              "targeting": {"sdkVersionTargeting": {"value": [{"min": 21}]}},
              "apkSet": [{"moduleMetadata": {"name": "base", "deliveryType": "INSTALL_TIME"}, "apkDescription": [
                {"path": "splits/base-master.apk", "splitApkMetadata": {"isMasterSplit": true}},
                {"path": "splits/base-arm64_v8a.apk", "splitApkMetadata": {"splitId": "config.arm64_v8a"},
                 "targeting": {"abiTargeting": {"value": [{"alias": "ARM64_V8A"}]}}}]}]}],
              "assetSliceSet": [
                {"assetModuleMetadata": {"name": "start", "deliveryType": "INSTALL_TIME"}, "apkDescription": [
                  {"path": "asset-slices/start-master.apk", "assetSliceMetadata": {"isMasterSplit": true}}]},
                {"assetModuleMetadata": {"name": "level2", "deliveryType": "FAST_FOLLOW"}, "apkDescription": [
                  {"path": "asset-slices/level2-master.apk", "assetSliceMetadata": {"isMasterSplit": true}},
                  {"path": "asset-slices/level2-astc.apk", "assetSliceMetadata": {"splitId": "level2.config.astc"},
                   "targeting": {"textureCompressionFormatTargeting": {"value": [{"alias": "ASTC"}]}}}]}\(onDemand)]}
            """
    }

    /// bundletool's versionName on a slice of a fast-follow or on-demand pack, which carries no versionCode: the app's
    /// versionCode written out, "since their version code is not used by Android" (`AssetSlicesGenerator.java:56-76`,
    /// 1.18.3).
    static func versionName(_ name: String) -> AXMLBuilder.Attribute {
        .android("versionName", 0x0101_021C, .string(name))
    }

    /// A later pack's slice as bundletool writes it: no versionCode, the app's as its versionName.
    static func laterSlice(_ split: String, configForSplit: String? = nil, versionName name: String? = "1") -> Data {
        SplitAPK.make(versionCode: nil, split: split, configForSplit: configForSplit,
                      attributes: name.map { [versionName($0)] } ?? [])
    }

    static let start = SplitAPK.make(split: "start")
    static let level2 = SplitAPK.make(split: "level2")
    static let level2ASTC = SplitAPK.make(split: "level2.config.astc", configForSplit: "level2")
    static let level3 = SplitAPK.make(split: "level3")

    /// The container's entries: the toc, the base's APKs and the packs' slices, `level3` under `level3`'s path.
    static func entries(level3: Data = level3, named name: String = "level3") -> [(String, Data)] {
        [("toc.json", Data(toc().replacingOccurrences(of: "level3", with: name).utf8)), ("splits/base-master.apk", Sets.base),
         ("splits/base-arm64_v8a.apk", Sets.arm64), ("asset-slices/start-master.apk", start),
         ("asset-slices/level2-master.apk", level2), ("asset-slices/level2-astc.apk", level2ASTC),
         ("asset-slices/\(name)-master.apk", level3)]
    }

    static func apks(level3: Data = level3, named name: String = "level3") -> Data {
        ContainerImportTests.zip(entries(level3: level3, named: name))
    }

    static func packsFolder(_ entry: LibraryEntry, _ paths: LibraryPaths) -> URL {
        paths.versionDirectory(for: entry).appendingPathComponent("packs", isDirectory: true)
    }

    static func packFiles(_ entry: LibraryEntry, _ paths: LibraryPaths) throws -> [String] {
        try FileManager.default.contentsOfDirectory(atPath: packsFolder(entry, paths).path).sorted()
    }

    @Test func aBundletoolSetKeepsItsLaterPacksInPacks() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let unit = try Sets.unit(["game.apks": Self.apks()], in: sandbox.paths)

        let results = try await library.importUnit(unit)

        let entry = try #require(results.first?.entry)
        #expect(results.count == 1 && entry.state == .ready)
        // The install-time pack is a split of the set, as Android installs it.
        #expect(Set(entry.splits.compactMap(\.splitName)) == ["config.arm64_v8a", "start"])
        #expect(entry.packs?.map(\.fileName) == ["level2-master.apk", "level2-astc.apk", "level3-master.apk"])
        #expect(entry.packs?.map(\.splitName) == ["level2", "level2.config.astc", "level3"])
        #expect(entry.packs?.first?.sha256 == SetImportTests.sha256(Self.level2))
        #expect(try Self.packFiles(entry, sandbox.paths) == ["level2-astc.apk", "level2-master.apk", "level3-master.apk"])
        #expect(try Data(contentsOf: Self.packsFolder(entry, sandbox.paths).appendingPathComponent("level3-master.apk"))
            == Self.level3)
        #expect(entry.ignoredFiles == nil)
        #expect(!sandbox.exists(unit))
    }

    @Test func itsPacksAreReadOnlyAndLeftOutOfBackups() async throws {
        // As the version's APKs are read-only for the guest, and as an app's OBBs are left out of backups (ADR-0049,
        // decision 9).
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)

        let entry = try #require(try await library.importUnit(Sets.unit(["game.apks": Self.apks()], in: sandbox.paths)).first?.entry)

        let folder = Self.packsFolder(entry, sandbox.paths)
        #expect(XAPKImportTests.isMarkedExcluded(folder))
        #expect(!FileManager.default.isWritableFile(atPath: folder.appendingPathComponent("level2-master.apk").path))
    }

    @Test(arguments: [
        (SplitAPK.make(split: "level3", signers: [.rotated]), "it is signed by another key than the app"),
        // The app's certificate, named in a signature Minato cannot verify, beside a verified base:
        // anyone can copy a certificate's name.
        (SplitAPK.make(split: "level3", v2: [.init(.ec, algorithms: [TestSigner.ecdsaSHA256Verity])]),
         "its signature cannot be verified"),
        (SplitAPK.make(versionCode: 2, split: "level3"), "its versionCode 2 is not the app’s 1"),
        // A versionCode of its own counts, whatever its versionName says.
        (SplitAPK.make(versionCode: 2, split: "level3", attributes: [versionName("1")]), "its versionCode 2 is not the app’s 1"),
        (SplitAPK.make(package: "org.example.other", split: "level3"), "it belongs to org.example.other"),
        // bundletool's form, of another version: its --asset-modules-version-override writes this too.
        (laterSlice("level3", versionName: "2"), "its versionName 2 is not the app’s versionCode 1"),
        (laterSlice("level3", versionName: nil), "it has no versionCode and no versionName"),
    ])
    func aPackSliceThatIsNotTheAppsIsLeftOutAndNamed(_ slice: Data, _ reason: String) async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)

        let results = try await library.importUnit(Sets.unit(["game.apks": Self.apks(level3: slice)], in: sandbox.paths))

        let entry = try #require(results.first?.entry)
        #expect(results.count == 1 && entry.state == .ready)
        #expect(entry.packs?.map(\.fileName) == ["level2-master.apk", "level2-astc.apk"])
        #expect(entry.ignoredFiles == [IgnoredFile(path: "asset-slices/level3-master.apk", reason: reason)])
        #expect(try Self.packFiles(entry, sandbox.paths) == ["level2-astc.apk", "level2-master.apk"])
    }

    @Test(arguments: [
        (SplitAPK.make(split: "level4"), "it is no slice of the pack level3"),
        (SplitAPK.make(split: "level3.config.astc", configForSplit: "level3"), ""),
        (SplitAPK.make(split: "level2.config.etc2", configForSplit: "level2"), "it is no slice of the pack level3"),
        (Data("no APK".utf8), "it is no APK Minato can read"),
    ])
    func aSliceIsKeptOnlyUnderItsOwnPacksName(_ slice: Data, _ reason: String) async throws {
        // The toc says level3; the slice's own split must agree, as Play's libraries read the split from the name.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)

        let results = try await library.importUnit(Sets.unit(["game.apks": Self.apks(level3: slice)], in: sandbox.paths))

        let entry = try #require(results.first?.entry)
        #expect(entry.state == .ready)
        if reason.isEmpty {
            // A config slice of level3 under the master's path is still level3's, named for its suffix.
            #expect(entry.packs?.map(\.fileName) == ["level2-master.apk", "level2-astc.apk", "level3-astc.apk"])
            #expect(entry.ignoredFiles == nil)
        } else {
            #expect(entry.packs?.map(\.fileName) == ["level2-master.apk", "level2-astc.apk"])
            #expect(entry.ignoredFiles == [IgnoredFile(path: "asset-slices/level3-master.apk", reason: reason)])
        }
    }

    @Test func aPackWhoseNameIsNoModulesKeepsNoSlice() async throws {
        // A pack's name becomes a file name in packs/: one a toc gives with more than a module's letters, digits and
        // "_" names nothing there, though Android takes "le.vel" as a split.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let unit = try Sets.unit(["game.apks": Self.apks(level3: SplitAPK.make(split: "le.vel"), named: "le.vel")],
                                 in: sandbox.paths)

        let entry = try #require(try await library.importUnit(unit).first?.entry)

        #expect(entry.packs?.map(\.fileName) == ["level2-master.apk", "level2-astc.apk"])
        #expect(entry.ignoredFiles == [IgnoredFile(path: "asset-slices/le.vel-master.apk", reason: "it is no slice of the pack le.vel")])
    }

    @Test func aDamagedPackSliceRefusesItsContainerAndLeavesNothingInTheUnit() async throws {
        // Its slices are taken out after its APKs; the container is refused as a whole, and both folders go with it.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        var zip = ZipBuilder()
        for (name, content) in Self.entries() { zip.add(name, content, method: .stored) }
        let output = zip.build()
        var data = output.data
        data[try #require(output.dataOffsets["asset-slices/level3-master.apk"])] ^= 0xFF
        let unit = try Sets.unit(["game.apks": data], in: sandbox.paths)

        let entry = try #require(try await library.importUnit(unit).first?.entry)

        #expect(entry.id == "file:game.apks")
        #expect(entry.refusal == "“asset-slices/level3-master.apk” in it is damaged: its checksum does not match")
        #expect(!sandbox.exists(unit))
    }

    @Test func keepInstalledLetsTheTakenOutPacksGoToo() async throws {
        // An older set asks; "Keep Installed" lets its container go with the APKs and the slices it took out.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importUnit(Sets.unit(["base.apk": SplitAPK.make(versionCode: 2)], in: sandbox.paths))
        let unit = try Sets.unit(["game.apks": Self.apks()], in: sandbox.paths)
        #expect(try await library.importUnit(unit).map(\.decision) == [.ask([.olderVersion])])

        await library.keepInstalled(discarding: unit)

        #expect(!sandbox.exists(unit))
        #expect(await library.entries.first?.report?.versionCode == 2)
    }

    @Test func twoSlicesOfOneNameKeepTheFirst() async throws {
        // A toc that lists level2's master twice, the second under another path: both would be level2-master.apk, so
        // the set keeps the first and names the second.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let toc = Self.toc().replacingOccurrences(of: "\"name\": \"level3\"", with: "\"name\": \"level2\"")
            .replacingOccurrences(of: "asset-slices/level3-master.apk", with: "asset-slices/level2-again.apk")
        let data = ContainerImportTests.zip(Self.entries().dropFirst().dropLast() + [("asset-slices/level2-again.apk",
                                                                                      SplitAPK.make(split: "level2"))]
                                            + [("toc.json", Data(toc.utf8))])

        let results = try await library.importUnit(Sets.unit(["game.apks": data], in: sandbox.paths))

        let entry = try #require(results.first?.entry)
        #expect(entry.packs?.map(\.fileName) == ["level2-master.apk", "level2-astc.apk"])
        #expect(entry.packs?.first?.sha256 == SetImportTests.sha256(Self.level2))
        #expect(entry.ignoredFiles == [IgnoredFile(path: "asset-slices/level2-again.apk",
                                                   reason: "the set has another level2-master.apk")])
    }

    @Test func anUpdateKeepsOnlyItsOwnPacks() async throws {
        // Version 2 brings level2 only: version 1's level3 goes with its version folder.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importUnit(Sets.unit(["game.apks": Self.apks()], in: sandbox.paths))
        let first = try #require(await library.entries.first)
        let v2 = ContainerImportTests.zip([
            ("toc.json", Data(Self.toc(level3: false).utf8)), ("splits/base-master.apk", SplitAPK.make(versionCode: 2)),
            ("splits/base-arm64_v8a.apk", SplitAPK.make(versionCode: 2, split: "config.arm64_v8a", abis: ["arm64-v8a"])),
            ("asset-slices/start-master.apk", SplitAPK.make(versionCode: 2, split: "start")),
            ("asset-slices/level2-master.apk", SplitAPK.make(versionCode: 2, split: "level2")),
            ("asset-slices/level2-astc.apk",
             SplitAPK.make(versionCode: 2, split: "level2.config.astc", configForSplit: "level2"))])

        let results = try await library.importUnit(Sets.unit(["game.apks": v2], in: sandbox.paths))

        let entry = try #require(results.first?.entry)
        #expect(results.map(\.decision) == [.update])
        #expect(entry.packs?.map(\.fileName) == ["level2-master.apk", "level2-astc.apk"])
        #expect(try Self.packFiles(entry, sandbox.paths) == ["level2-astc.apk", "level2-master.apk"])
        #expect(!sandbox.exists(sandbox.paths.versionDirectory(for: first)))
    }

    @Test func theSameVersionAddsThePacksItsContainerBrings() async throws {
        // Installed from its loose APKs first, the set gets the packs its .apks brings, and its version stays.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importUnit(Sets.unit(["base.apk": Sets.base, "arm64.apk": Sets.arm64, "start.apk": Self.start],
                                                   in: sandbox.paths))
        let first = try #require(await library.entries.first)
        #expect(first.packs == nil)

        let results = try await library.importUnit(Sets.unit(["game.apks": Self.apks()], in: sandbox.paths))

        let entry = try #require(results.first?.entry)
        #expect(results.map(\.decision) == [.unchanged])
        #expect(entry.folder == first.folder)
        #expect(entry.packs?.map(\.fileName) == ["level2-master.apk", "level2-astc.apk", "level3-master.apk"])
        #expect(try Self.packFiles(entry, sandbox.paths) == ["level2-astc.apk", "level2-master.apk", "level3-master.apk"])
    }

    @Test func theSameVersionReplacesItsPacksWithThoseItsContainerBrings() async throws {
        // The same APKs again in an .apks with level2 only: level3 goes, and packs/ is never half written.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importUnit(Sets.unit(["game.apks": Self.apks()], in: sandbox.paths))
        let withoutLevel3 = ContainerImportTests.zip(Self.entries().dropFirst().dropLast() + [("toc.json", Data(Self.toc(level3: false).utf8))])

        let results = try await library.importUnit(Sets.unit(["game.apks": withoutLevel3], in: sandbox.paths))

        let entry = try #require(results.first?.entry)
        #expect(results.map(\.decision) == [.unchanged])
        #expect(entry.packs?.map(\.fileName) == ["level2-master.apk", "level2-astc.apk"])
        #expect(try Self.packFiles(entry, sandbox.paths) == ["level2-astc.apk", "level2-master.apk"])
        #expect(!sandbox.exists(sandbox.paths.versionDirectory(for: entry).appendingPathComponent("packs.partial")))
        #expect(XAPKImportTests.isMarkedExcluded(Self.packsFolder(entry, sandbox.paths)))
    }

    @Test func slicesMinatoCannotVerifyGoBesideABaseItCannotVerifyEither() async throws {
        // The whole set is signed with a verity digest only: it counts as one APK Minato cannot verify, its slices too.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let verity: [TestSigner.Signer] = [.init(.ec, algorithms: [TestSigner.ecdsaSHA256Verity])]
        let data = ContainerImportTests.zip([
            ("toc.json", Data(Self.toc().utf8)), ("splits/base-master.apk", SplitAPK.make(v2: verity)),
            ("splits/base-arm64_v8a.apk", SplitAPK.make(split: "config.arm64_v8a", abis: ["arm64-v8a"], v2: verity)),
            ("asset-slices/start-master.apk", SplitAPK.make(split: "start", v2: verity)),
            ("asset-slices/level2-master.apk", SplitAPK.make(split: "level2", v2: verity)),
            ("asset-slices/level2-astc.apk", SplitAPK.make(split: "level2.config.astc", configForSplit: "level2", v2: verity)),
            ("asset-slices/level3-master.apk", SplitAPK.make(split: "level3", v2: verity))])
        let unit = try Sets.unit(["game.apks": data], in: sandbox.paths)

        var results = try await library.importUnit(unit)
        if case .ask(let warnings) = results.first?.decision {
            results = try await library.importUnit(unit, replacing: [AskingVersion(package: SplitAPK.package, versionCode: 1,
                                                                                    warnings: warnings)])
        }

        let entry = try #require(results.first?.entry)
        #expect(entry.packs?.map(\.fileName) == ["level2-master.apk", "level2-astc.apk", "level3-master.apk"])
        #expect(entry.ignoredFiles == nil)
    }

    /// The slices of fast-follow and on-demand packs as bundletool writes them, with the app's versionCode as their
    /// versionName and no versionCode of their own, are the app's (ADR-0049 f's fix).
    @Test func laterSlicesAsBundletoolWritesThemAreKept() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entries = Self.entries().map { entry -> (String, Data) in
            switch entry.0 {
            case "asset-slices/level2-master.apk": (entry.0, Self.laterSlice("level2"))
            case "asset-slices/level2-astc.apk": (entry.0, Self.laterSlice("level2.config.astc", configForSplit: "level2"))
            case "asset-slices/level3-master.apk": (entry.0, Self.laterSlice("level3"))
            default: entry
            }
        }

        let results = try await library.importUnit(Sets.unit(["game.apks": ContainerImportTests.zip(entries)], in: sandbox.paths))

        let entry = try #require(results.first?.entry)
        #expect(results.count == 1 && entry.state == .ready)
        #expect(entry.packs?.map(\.fileName) == ["level2-master.apk", "level2-astc.apk", "level3-master.apk"])
        #expect(entry.ignoredFiles == nil)
    }

    /// The versionName bundletool writes is the base's versionCode attribute alone, without its versionCodeMajor
    /// (`AndroidManifest.getVersionCode`), while the app's version counts both.
    @Test func aLaterSliceNamesTheVersionCodeWithoutItsMajor() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let entries: [(String, Data)] = [
            ("toc.json", Data(Self.toc().utf8)), ("splits/base-master.apk", SplitAPK.make(versionCodeMajor: 1)),
            ("splits/base-arm64_v8a.apk", SplitAPK.make(versionCodeMajor: 1, split: "config.arm64_v8a", abis: ["arm64-v8a"])),
            ("asset-slices/start-master.apk", SplitAPK.make(versionCodeMajor: 1, split: "start")),
            ("asset-slices/level2-master.apk", Self.laterSlice("level2")),
            ("asset-slices/level2-astc.apk", Self.laterSlice("level2.config.astc", configForSplit: "level2")),
            ("asset-slices/level3-master.apk", Self.laterSlice("level3")),
        ]

        let results = try await library.importUnit(Sets.unit(["game.apks": ContainerImportTests.zip(entries)], in: sandbox.paths))

        let entry = try #require(results.first?.entry)
        #expect(results.count == 1 && entry.state == .ready)
        #expect(entry.report?.versionCode == Int64(1) << 32 | 1)
        #expect(entry.packs?.map(\.fileName) == ["level2-master.apk", "level2-astc.apk", "level3-master.apk"])
        #expect(entry.ignoredFiles == nil)
    }

    @Test func aSetWithoutLaterPacksHasNone() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)

        let entry = try #require(try await library.importUnit(Sets.unit(["game.apks": ContainerImportTests.container("bundletool")],
                                                                        in: sandbox.paths)).first?.entry)

        #expect(entry.state == .ready && entry.packs == nil)
        #expect(!sandbox.exists(Self.packsFolder(entry, sandbox.paths)))
    }
}
