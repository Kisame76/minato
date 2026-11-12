import APKFormats
import APKKit
import APKTestSupport
import Foundation
import Testing

@testable import APKLibrary

/// Later packs in the other containers (ADR-0050, step 5): an XAPK, an APKM or a loose set that carries the slices
/// of a fast-follow or on-demand pack, as bundletool writes them (no versionCode, the app's as versionName), keeps
/// them in packs/ as a bundletool `.apks` does (ADR-0049 f), not as splits of the set. Install-time packs, which
/// carry the app's versionCode, stay splits.
@Suite("Pack import from other containers")
struct ContainerPackTests {
    typealias Sandbox = LibraryTests.Sandbox
    typealias Sets = SetImportTests
    typealias Packs = PackImportTests

    static let level2 = Packs.laterSlice("level2")
    static let level2ASTC = Packs.laterSlice("level2.config.astc", configForSplit: "level2")

    /// The three containers around the same APKs, each as a unit: the container's entries and its name.
    static func units(_ apks: [(String, Data)], in paths: LibraryPaths) throws -> [String: URL] {
        let base = [("base.apk", Sets.base), ("split_config.arm64_v8a.apk", Sets.arm64)]
        let xapk = XAPKImportTests.xapk([("\(SplitAPK.package).apk", Sets.base), ("config.arm64_v8a.apk", Sets.arm64)]
                                        + apks)
        let apkm = ContainerImportTests.zip([("info.json", APKMImportTests.info())] + base + apks)
        return [
            "xapk": try Sets.unit(["game.xapk": xapk], in: paths),
            "apkm": try Sets.unit(["game.apkm": apkm], in: paths),
            "loose": try Sets.unit(Dictionary(uniqueKeysWithValues: base + apks), in: paths),
            // A plain zip without info.json is a loose set too.
            "zip": try Sets.unit(["game.zip": ContainerImportTests.zip(base + apks)], in: paths),
        ]
    }

    @Test(arguments: ["xapk", "apkm", "loose", "zip"])
    func aLaterPackSliceGoesToPacksNotToTheSplits(_ kind: String) async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let unit = try #require(Self.units([("level2.apk", Self.level2), ("level2_astc.apk", Self.level2ASTC)],
                                           in: sandbox.paths)[kind])

        let entry = try #require(try await library.importUnit(unit).first?.entry)

        #expect(entry.state == .ready)
        #expect(entry.splits.compactMap(\.splitName) == ["config.arm64_v8a"])
        #expect(entry.packs?.map(\.fileName).sorted() == ["level2-astc.apk", "level2-master.apk"])
        #expect(Set(entry.packs?.compactMap(\.splitName) ?? []) == ["level2", "level2.config.astc"])
        #expect(try Data(contentsOf: PackImportTests.packsFolder(entry, sandbox.paths)
            .appendingPathComponent("level2-master.apk")) == Self.level2)
        #expect(entry.ignoredFiles == nil)
    }

    @Test(arguments: ["xapk", "apkm", "loose", "zip"])
    func anInstallTimePackStaysASplit(_ kind: String) async throws {
        // It carries the app's versionCode, as the installer needs of the splits it installs.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let unit = try #require(Self.units([("start.apk", PackImportTests.start)], in: sandbox.paths)[kind])

        let entry = try #require(try await library.importUnit(unit).first?.entry)

        #expect(entry.state == .ready)
        #expect(Set(entry.splits.compactMap(\.splitName)) == ["config.arm64_v8a", "start"])
        #expect(entry.packs == nil)
    }

    @Test func aSliceOfAnotherVersionIsNamedWhereItCame() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let unit = try #require(Self.units([("level2.apk", Packs.laterSlice("level2", versionName: "2"))],
                                           in: sandbox.paths)["xapk"])

        let entry = try #require(try await library.importUnit(unit).first?.entry)

        #expect(entry.packs == nil)
        #expect(entry.ignoredFiles?.map(\.reason) == ["its versionName 2 is not the app’s versionCode 1"])
        #expect(entry.ignoredFiles?.first?.path.hasPrefix("game.xapk/") == true)
    }

    @Test func eachContainerKeepsItsOwnPacks() async throws {
        // Two containers of two packages in a unit: a slice goes with the set of its own container.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        func apkm(_ package: String, _ pack: String) -> Data {
            ContainerImportTests.zip([
                ("info.json", APKMImportTests.info(package: package)), ("base.apk", SplitAPK.make(package: package)),
                ("\(pack).apk", SplitAPK.make(package: package, versionCode: nil, split: pack,
                                              attributes: [PackImportTests.versionName("1")]))])
        }
        let unit = try Sets.unit(["a.apkm": apkm("org.example.one", "level2"),
                                  "b.apkm": apkm("org.example.two", "level3")], in: sandbox.paths)

        let results = try await library.importUnit(unit)

        let packs = Dictionary(uniqueKeysWithValues: results.map(\.entry).map { ($0.id, $0.packs?.map(\.fileName)) })
        #expect(packs == ["org.example.one": ["level2-master.apk"], "org.example.two": ["level3-master.apk"]])
    }

    @Test func aSliceOfASecondContainerOfTheSameVersionIsKept() async throws {
        // Two XAPKs of one app version make one set, whose first member is the first XAPK's: the slice of the second
        // XAPK is kept.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let first = XAPKImportTests.xapk([("\(SplitAPK.package).apk", Sets.base), ("config.arm64_v8a.apk", Sets.arm64)])
        let xapk = XAPKImportTests.xapk([("config.xxhdpi.apk", Sets.xxhdpi), ("level2.apk", Self.level2)])
        let unit = try Sets.unit(["a.xapk": first, "b.xapk": xapk], in: sandbox.paths)

        let entries = try await library.importUnit(unit).map(\.entry)

        #expect(entries.count == 1 && entries[0].state == .ready)
        #expect(entries[0].splits.compactMap(\.splitName).contains("config.xxhdpi"))
        #expect(entries[0].packs?.map(\.fileName) == ["level2-master.apk"])
    }

    @Test func aSliceOfASecondContainerThatIsRefusedIsNamedOnTheEntry() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let foreign = SplitAPK.make(versionCode: nil, split: "level2", attributes: [PackImportTests.versionName("1")],
                                    signers: [.rotated])
        let first = XAPKImportTests.xapk([("\(SplitAPK.package).apk", Sets.base), ("config.arm64_v8a.apk", Sets.arm64)])
        let xapk = XAPKImportTests.xapk([("config.xxhdpi.apk", Sets.xxhdpi), ("level2.apk", foreign)])
        let unit = try Sets.unit(["a.xapk": first, "b.xapk": xapk], in: sandbox.paths)

        let entries = try await library.importUnit(unit).map(\.entry)

        #expect(entries.count == 1 && entries[0].packs == nil)
        #expect(entries[0].ignoredFiles?.map(\.reason) == ["it is signed by another key than the app"])
    }

    @Test func eachVersionOfOneAppInItsContainerKeepsItsOwnSlice() async throws {
        // Two XAPKs of two versions, each with a base of the same name and a slice naming its own version: each slice
        // goes with its own version's set.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        func xapk(_ code: Int32, _ pack: String) -> Data {
            XAPKImportTests.xapk([("\(SplitAPK.package).apk", SplitAPK.make(versionCode: code)),
                                  ("late.apk", Packs.laterSlice(pack, versionName: "\(code)"))])
        }
        let unit = try Sets.unit(["a.xapk": xapk(1, "level2"), "b.xapk": xapk(2, "level3")], in: sandbox.paths)

        let entries = try await library.importUnit(unit).map(\.entry)

        let kept = entries.map { ($0.report?.versionCode ?? 0, $0.packs?.map(\.fileName) ?? []) }
        #expect(kept.contains { $0.0 == 1 && $0.1 == ["level2-master.apk"] })
        #expect(kept.contains { $0.0 == 2 && $0.1 == ["level3-master.apk"] })
        #expect(entries.allSatisfy { $0.ignoredFiles == nil })
    }

    @Test func aSliceGoesToTheVersionItNames() async throws {
        // Two versions of the package in one container: the slice names version 2 as its versionName, so it goes with
        // that set, not with the first.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let xapk = XAPKImportTests.xapk([("one.apk", Sets.base), ("two.apk", SplitAPK.make(versionCode: 2)),
                                         ("level2.apk", Packs.laterSlice("level2", versionName: "2"))])
        let unit = try Sets.unit(["game.xapk": xapk], in: sandbox.paths)

        let entries = try await library.importUnit(unit).compactMap(\.entry)

        let withPacks = entries.filter { $0.packs != nil }
        #expect(withPacks.map { $0.report?.versionCode } == [2])
        #expect(entries.allSatisfy { $0.ignoredFiles == nil })
    }

    @Test func aSetWithoutVersionCodeKeepsItsSplits() async throws {
        // Base, a config split and a feature split, none with a versionCode and all with the versionName "1.0": the
        // marker's first two parts hold for the splits, but no base has a versionCode to name, so none is a pack.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let name = PackImportTests.versionName("1.0")
        let xapk = XAPKImportTests.xapk([
            ("base.apk", SplitAPK.make(versionCode: nil, attributes: [name])),
            ("config.arm64_v8a.apk", SplitAPK.make(versionCode: nil, split: "config.arm64_v8a", abis: ["arm64-v8a"],
                                                    attributes: [name])),
            ("dyn.apk", SplitAPK.make(versionCode: nil, split: "dyn", isFeatureSplit: true, attributes: [name]))])
        let unit = try Sets.unit(["game.xapk": xapk], in: sandbox.paths)

        let entry = try #require(try await library.importUnit(unit).first?.entry)

        #expect(entry.state == .ready)
        #expect(Set(entry.splits.compactMap(\.splitName)) == ["config.arm64_v8a", "dyn"])
        #expect(entry.packs == nil)
    }

    @Test func aForgedMarkerOnAConfigSplitIsNamedNotKeptAsAPack() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let forged = SplitAPK.make(versionCode: nil, split: "config.xxhdpi",
                                   attributes: [PackImportTests.versionName("1")])
        let xapk = XAPKImportTests.xapk([("\(SplitAPK.package).apk", Sets.base), ("config.arm64_v8a.apk", Sets.arm64),
                                         ("config.xxhdpi.apk", forged)])
        let unit = try Sets.unit(["game.xapk": xapk], in: sandbox.paths)

        let entry = try #require(try await library.importUnit(unit).first?.entry)

        #expect(entry.state == .ready)
        #expect(entry.splits.compactMap(\.splitName) == ["config.arm64_v8a"])
        #expect(entry.packs == nil)
        #expect(entry.ignoredFiles?.map(\.reason) == ["it is no slice of the pack config.xxhdpi"])
    }

    @Test func aSliceWithTheNameOfASplitOfTheSetIsNamed() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let xapk = XAPKImportTests.xapk([("\(SplitAPK.package).apk", Sets.base), ("start.apk", PackImportTests.start),
                                         ("late.apk", Packs.laterSlice("start"))])
        let unit = try Sets.unit(["game.xapk": xapk], in: sandbox.paths)

        let entry = try #require(try await library.importUnit(unit).first?.entry)

        #expect(entry.splits.compactMap(\.splitName) == ["start"])
        #expect(entry.packs == nil)
        #expect(entry.ignoredFiles?.map(\.reason) == ["the set has a split start"])
    }

    @Test(arguments: [
        (Packs.laterSlice("Level2"), "the set has a split level2"),
        (Packs.laterSlice("level2.config.astc", configForSplit: "level2"), "the set has a split level2"),
    ])
    func aSliceOfAPackWhoseNameASplitHasIsNamed(_ slice: Data, _ reason: String) async throws {
        // An install-time pack is a split; a later slice of the same pack name, in any case, or its config slice, would
        // lie beside it in packs/ and clash on APFS or in Play's view of the pack.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let xapk = XAPKImportTests.xapk([("\(SplitAPK.package).apk", Sets.base),
                                         ("start.apk", SplitAPK.make(split: "level2")), ("late.apk", slice)])
        let unit = try Sets.unit(["game.xapk": xapk], in: sandbox.paths)

        let entry = try #require(try await library.importUnit(unit).first?.entry)

        #expect(entry.splits.compactMap(\.splitName) == ["level2"])
        #expect(entry.packs == nil)
        #expect(entry.ignoredFiles?.map(\.reason) == [reason])
    }

    @Test func twoSlicesThatDifferInCaseOnlyKeepTheFirst() async throws {
        // One file on APFS; SetChecks folds case for the splits too.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let xapk = XAPKImportTests.xapk([("\(SplitAPK.package).apk", Sets.base), ("a.apk", Packs.laterSlice("level2")),
                                         ("b.apk", Packs.laterSlice("Level2"))])
        let unit = try Sets.unit(["game.xapk": xapk], in: sandbox.paths)

        let entry = try #require(try await library.importUnit(unit).first?.entry)

        #expect(entry.state == .ready)
        #expect(entry.packs?.count == 1)
        #expect(entry.ignoredFiles?.count == 1)
    }

    @Test func aLooseSliceAndAContainersSplitMakeOneSetWithThePack() async throws {
        // A loose base and slice and an XAPK with the arm64 split of the same version: the group's container is the
        // XAPK's, the slice's none; it is kept all the same.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let xapk = XAPKImportTests.xapk([("config.arm64_v8a.apk", Sets.arm64)])
        let unit = try Sets.unit(["base.apk": Sets.base, "level2.apk": Self.level2, "x.xapk": xapk], in: sandbox.paths)

        let entries = try await library.importUnit(unit).map(\.entry)

        #expect(entries.count == 1 && entries[0].state == .ready)
        #expect(entries[0].packs?.map(\.fileName) == ["level2-master.apk"])
    }

    @Test func aLoneLaterSliceInAFolderIsNoSetOfItsOwnPack() async throws {
        // No base of its package beside it, so it is no pack of any set: it stays a member, as it was before.
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let unit = try Sets.unit(["level2.apk": Self.level2], in: sandbox.paths)

        let results = try await library.importUnit(unit)

        #expect(results.count == 1 && results.compactMap(\.entry).allSatisfy { $0.packs == nil })
    }

    @Test(arguments: ["xapk", "apkm", "loose", "zip"])
    func aSliceOfAnotherKeyIsLeftOutAndNamed(_ kind: String) async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let library = try Library(paths: sandbox.paths)
        let foreign = SplitAPK.make(versionCode: nil, split: "level2", attributes: [PackImportTests.versionName("1")],
                                    signers: [.rotated])
        let unit = try #require(Self.units([("level2.apk", foreign)], in: sandbox.paths)[kind])

        let entry = try #require(try await library.importUnit(unit).first?.entry)

        #expect(entry.state == .ready)
        #expect(entry.packs == nil)
        #expect(entry.ignoredFiles?.map(\.reason) == ["it is signed by another key than the app"])
    }
}
