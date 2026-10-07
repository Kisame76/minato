import APKFormats
import APKKit
import APKTestSupport
import Foundation
import Testing

/// bundletool's `.apks` as a container: its table of contents read within decision 7's caps and the chosen APKs
/// named before anything is extracted (ADR-0049 decision 2).
@Suite("bundletool's .apks")
struct BundletoolContainerTests {
    typealias Toc = BundletoolToc

    static let device = BundletoolSelectorTests.iPhone

    /// A split variant whose base has an arm64-v8a and an armeabi-v7a split, and an on-demand module.
    static var toc: Toc {
        let abis = BundletoolSelectorTests.dimension([("splits/base-arm64_v8a.apk", Int32(3)),
                                                      ("splits/base-armeabi_v7a.apk", 2)]) { $0.abi = $1 }
        return BundletoolSelectorTests.toc([BundletoolSelectorTests.variant(sdk: 21, [
            BundletoolSelectorTests.module("base", [BundletoolSelectorTests.apk("splits/base-master.apk")] + abis),
            BundletoolSelectorTests.module("camera", delivery: Toc.DeliveryType.onDemand,
                                           [BundletoolSelectorTests.apk("splits/camera-master.apk")]),
        ])])
    }

    static let apks: [(String, Data)] = [
        ("splits/base-master.apk", SplitAPK.make()),
        ("splits/base-arm64_v8a.apk", SplitAPK.make(split: "config.arm64_v8a")),
        ("splits/base-armeabi_v7a.apk", SplitAPK.make(split: "config.armeabi_v7a")),
        ("splits/camera-master.apk", SplitAPK.make(split: "camera", isFeatureSplit: true)),
    ]

    static func container(_ entries: [(String, Data)]) throws -> (ContainerExtractionTests.Unit, Container) {
        let unit = try ContainerExtractionTests.Unit()
        // bundletool stores every entry (ApkSetWriter.java).
        return (unit, try Container.open(unit.file(ContainerTests.zip(entries, method: .stored))))
    }

    @Test(arguments: ["toc.pb", "toc.json"])
    func theChosenAPKsAreNamedFromTheTocBeforeAnyIsTakenOut(_ name: String) throws {
        let toc = name == "toc.pb" ? TocWriter.toc(Self.toc) : Data(TocWriter.json(Self.toc).utf8)
        let (unit, container) = try Self.container([(name, toc)] + Self.apks)
        #expect(container.kind == .bundletool)
        let (items, choice) = try container.bundletoolItems(for: Self.device)

        #expect(choice.apks.map(\.path) == ["splits/base-master.apk", "splits/base-arm64_v8a.apk", "splits/camera-master.apk"])
        #expect(items.map(\.entry.name) == choice.apks.map(\.path))
        #expect(items.map(\.fileName) == ["1.apk", "2.apk", "3.apk"])
        let files = try container.extract(items, into: unit.destination)
        #expect(try files.map { try Data(contentsOf: $0) } == [Self.apks[0].1, Self.apks[1].1, Self.apks[3].1])
    }

    @Test func laterPackSlicesCountTowardTheCapWithTheAPKs() throws {
        // ADR-0049 decision 7: the slices of the packs Play fetches later are taken out with the APKs (decision 2).
        let slices = (0..<Container.maxAPKs).map { "asset-slices/p\($0).apk" }
        let (_, container) = try Self.container([("toc.json", Data(TocWriter.json(Self.toc).utf8))] + Self.apks
                                                + slices.dropLast().map { ($0, Data("slice".utf8)) })
        func choice(_ count: Int) -> BundletoolChoice {
            BundletoolChoice(apks: [.init(path: "splits/base-master.apk", module: "base")],
                             packs: [.init(name: "p", deliveryType: Toc.DeliveryType.onDemand, paths: Array(slices.prefix(count)))])
        }

        let items = try container.bundletoolPackItems(of: choice(Container.maxAPKs - 1))
        #expect(items.count == Container.maxAPKs - 1)
        #expect(items.first?.pack == "p" && items.first?.path == "asset-slices/p0.apk" && items.first?.item.fileName == "p1.apk")
        #expect(throws: ContainerError.overCap("it holds 513 APKs and pack slices for this iPhone, more than the 512 Minato "
                                               + "takes from a container")) {
            try container.bundletoolPackItems(of: choice(Container.maxAPKs))
        }
    }

    @Test func aContainerWithBothTocsIsRefused() throws {
        // ResultUtils.java:111-125.
        let (_, container) = try Self.container([("toc.pb", TocWriter.toc(Self.toc)), ("toc.json", Data("{}".utf8))] + Self.apks)
        #expect(throws: ContainerError.invalidTOC("it holds both toc.pb and toc.json")) {
            try container.bundletoolItems(for: Self.device)
        }
    }

    @Test func aTocOverItsCapIsRefused() throws {
        // Decision 7: a toc.pb of 16 MiB, a toc.json of 1 MiB, by their declared sizes; deflated, so the test is small.
        func refused(_ name: String, _ size: Int) throws {
            let unit = try ContainerExtractionTests.Unit()
            let container = try Container.open(unit.file(ContainerTests.zip([(name, Data(count: size))] + Self.apks,
                                                                            method: .deflated)))
            #expect(throws: ContainerError.overCap("its \(name) has \(size) bytes, more than the \(name == "toc.pb" ? 16 << 20 : 1 << 20) Minato reads")) {
                try container.bundletoolItems(for: Self.device)
            }
        }
        try refused("toc.pb", (16 << 20) + 1)
        try refused("toc.json", (1 << 20) + 1)
    }

    @Test func aDamagedTocIsRefusedWithItsReason() throws {
        let (_, container) = try Self.container([("toc.pb", Data([0x0A, 0x05, 0x01]))] + Self.apks)
        #expect(throws: ContainerError.invalidTOC("its toc.pb is cut short in field 1's 5 bytes")) {
            try container.bundletoolItems(for: Self.device)
        }
    }

    @Test func aTocNamingAnAPKTheContainerLacksIsRefused() throws {
        let (_, container) = try Self.container([("toc.pb", TocWriter.toc(Self.toc))] + Self.apks.filter { $0.0 != "splits/base-arm64_v8a.apk" })
        #expect(throws: ContainerError.invalidTOC("its table of contents names splits/base-arm64_v8a.apk, which it lacks")) {
            try container.bundletoolItems(for: Self.device)
        }
    }

    @Test func aTocWithNothingForThisIPhoneSaysWhy() throws {
        let abis = BundletoolSelectorTests.dimension([("splits/base-x86.apk", Int32(4)), ("splits/base-x86_64.apk", 5)]) {
            $0.abi = $1
        }
        let toc = BundletoolSelectorTests.toc([BundletoolSelectorTests.variant(sdk: 21, [
            BundletoolSelectorTests.module("base", [BundletoolSelectorTests.apk("splits/base-master.apk")] + abis)])])
        let (_, container) = try Self.container([("toc.pb", TocWriter.toc(toc)), ("splits/base-master.apk", Data()),
                                                 ("splits/base-x86.apk", Data()), ("splits/base-x86_64.apk", Data())])
        #expect(throws: ContainerError.incompatible("No arm64-v8a APK")) { try container.bundletoolItems(for: Self.device) }
    }

    @Test func onlyABundletoolContainerHasAToc() throws {
        let (_, container) = try Self.container(Self.apks)
        #expect(throws: ContainerError.invalidTOC("it has no table of contents")) {
            try container.bundletoolItems(for: Self.device)
        }
    }
}
