import APKFormats
import APKKit
import APKTestSupport
import Foundation
import Testing

/// A split set analysed over the APKs Minato installs (ADR-0048, decision 5; step b5a, test 7).
@Suite("Set analysis")
struct SetAnalysisTests {
    typealias Config = ARSCBuilder.Config

    static let png = Data([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A])

    /// An unsigned APK of the set: `split` nil makes the base.
    static func apk(split: String? = nil, manifest: [AXMLBuilder.Attribute] = [],
                    application: [AXMLBuilder.Attribute] = [.android("hasCode", 0x0101_000C, .bool(false))],
                    resources: ARSCBuilder? = nil, files: [ZipBuilder.Entry] = []) -> Data {
        var attributes: [AXMLBuilder.Attribute] = [.plain("package", .string(SplitAPK.package)),
                                                   .android("versionCode", 0x0101_021B, .int(1))]
        if let split { attributes.append(.plain("split", .string(split))) }
        var zip = ZipBuilder()
        zip.add("AndroidManifest.xml", AXMLBuilder().build(AXMLBuilder.Element("manifest", attributes + manifest, [
            AXMLBuilder.Element("application", application),
        ])))
        if var resources {
            resources.packageName = SplitAPK.package
            zip.add("resources.arsc", resources.build(), method: .stored)
        }
        zip.entries += files
        return zip.build().data
    }

    static func library(_ abi: String, machine: UInt16 = 183) -> ZipBuilder.Entry {
        var elf = ELFBuilder(segments: [.load(align: 0x4000), .load(align: 0x4000), .dynamic])
        elf.machine = machine
        return ZipBuilder.Entry(name: "lib/\(abi)/libmain.so", content: elf.build())
    }

    /// The set of `apks` (file name to bytes) as an @3x iPhone with these languages installs it.
    static func analyze(_ apks: [String: Data], languages: [String] = ["en"]) throws -> (report: APKReport, icon: Data?) {
        let archives = try apks.mapValues { try ZipArchive(data: $0) }
        let members = try apks.keys.sorted().map { try SetMember.read(archives[$0]!, fileName: $0, sha256: $0) }
        let set = try SetChecks.check(members).get()
        let choice = SplitSelector.choose(set, for: .iPhone(nativeScale: 3, languages: languages))
        return try SetAnalysis.analyze(set, choice: choice, preferredLanguages: languages) { archives[$0.fileName]! }
    }

    @Test func theLabelAndTheIconComeFromTheSplitsThatFitTheIPhone() throws {
        var base = ARSCBuilder()
        let label = base.add("string", "app_name", [(.default, .string("Game"))])
        let icon = base.add("mipmap", "ic_launcher", [(Config(density: 160), .string("res/mipmap-mdpi-v4/ic_launcher.png"))])
        var german = ARSCBuilder()
        german.add("string", "app_name", [(Config(language: "de"), .string("Spiel"))])
        // Density splits hold the base's IDs with their own configuration.
        func density(_ dpi: UInt16, _ name: String) -> ARSCBuilder {
            var table = ARSCBuilder()
            table.add("string", "app_name", [])
            table.add("mipmap", "ic_launcher", [(Config(density: dpi), .string("res/mipmap-\(name)-v4/ic_launcher.png"))])
            return table
        }
        let apks = [
            "base.apk": Self.apk(application: [.android("hasCode", 0x0101_000C, .bool(false)),
                                               .android("label", 0x0101_0001, .reference(label)),
                                               .android("icon", 0x0101_0002, .reference(icon))],
                                 resources: base,
                                 files: [ZipBuilder.Entry(name: "res/mipmap-mdpi-v4/ic_launcher.png", content: Self.png + Data("mdpi".utf8))]),
            "de.apk": Self.apk(split: "config.de", resources: german),
            "xxhdpi.apk": Self.apk(split: "config.xxhdpi", resources: density(480, "xxhdpi"), files: [
                ZipBuilder.Entry(name: "res/mipmap-xxhdpi-v4/ic_launcher.png", content: Self.png + Data("xxhdpi".utf8)),
            ]),
            // Skipped for an @3x iPhone, though its icon is larger.
            "xxxhdpi.apk": Self.apk(split: "config.xxxhdpi", resources: density(640, "xxxhdpi"), files: [
                ZipBuilder.Entry(name: "res/mipmap-xxxhdpi-v4/ic_launcher.png", content: Self.png + Data("xxxhdpi".utf8)),
            ]),
        ]

        let analysis = try Self.analyze(apks, languages: ["de"])

        #expect(analysis.report.label == "Spiel")
        #expect(analysis.report.iconPath == "res/mipmap-xxhdpi-v4/ic_launcher.png")
        #expect(analysis.icon == Self.png + Data("xxhdpi".utf8))
    }

    @Test func aVectorIconsLauncherImageMayLieInASplit() throws {
        // No table gives a raster: the largest launcher-named image of any installed APK.
        var base = ARSCBuilder()
        let icon = base.add("mipmap", "ic_launcher", [(.default, .string("res/drawable/ic_launcher.xml"))])
        let apks = [
            "base.apk": Self.apk(application: [.android("hasCode", 0x0101_000C, .bool(false)),
                                               .android("icon", 0x0101_0002, .reference(icon))],
                                 resources: base,
                                 files: [ZipBuilder.Entry(name: "res/drawable/ic_launcher.xml", content: Data("<vector/>".utf8))]),
            "xxhdpi.apk": Self.apk(split: "config.xxhdpi", files: [
                ZipBuilder.Entry(name: "res/drawable-xxhdpi-v4/ic_launcher.png", content: Self.png + Data("xxhdpi".utf8)),
            ]),
        ]

        let analysis = try Self.analyze(apks)

        #expect(analysis.report.iconPath == "res/drawable-xxhdpi-v4/ic_launcher.png")
        #expect(analysis.icon == Self.png + Data("xxhdpi".utf8))
    }

    @Test func anIconFileItsAPKLacksIsNoCandidate() throws {
        // A table names files in its own APK; one that is not there is passed over.
        var base = ARSCBuilder()
        let icon = base.add("mipmap", "ic_launcher", [(Config(density: 160), .string("res/mipmap-mdpi-v4/ic_launcher.png")),
                                                      (Config(density: 640), .string("res/mipmap-xxxhdpi-v4/ic_launcher.png"))])
        let apks = [
            "base.apk": Self.apk(application: [.android("hasCode", 0x0101_000C, .bool(false)),
                                               .android("icon", 0x0101_0002, .reference(icon))],
                                 resources: base,
                                 files: [ZipBuilder.Entry(name: "res/mipmap-mdpi-v4/ic_launcher.png", content: Self.png + Data("mdpi".utf8))]),
            "xxxhdpi.apk": Self.apk(split: "config.xxxhdpi", files: [
                ZipBuilder.Entry(name: "res/mipmap-xxxhdpi-v4/ic_launcher.png", content: Self.png + Data("xxxhdpi".utf8)),
            ]),
        ]

        let analysis = try Self.analyze(apks, languages: ["en"])

        #expect(analysis.report.iconPath == "res/mipmap-mdpi-v4/ic_launcher.png")
        #expect(analysis.icon == Self.png + Data("mdpi".utf8))
    }

    @Test func splitsWithoutTheirBaseAreNoSetToAnalyse() throws {
        // They may only join the installed version (decision 7), which step b5b does.
        let archive = try ZipArchive(data: Self.apk(split: "config.de"))
        let set = try SetChecks.check([try SetMember.read(archive, fileName: "de.apk", sha256: "de")]).get()
        let choice = SplitSelector.choose(set, for: .iPhone(nativeScale: 3, languages: ["de"]))

        let refusal = #expect(throws: SetRefusal.self) {
            try SetAnalysis.analyze(set, choice: choice, preferredLanguages: ["de"]) { _ in archive }
        }
        #expect(refusal?.reason == "Split APK “de.apk” needs its base APK. Import it together with its base APK.")
    }

    @Test func theLibrariesComeFromTheARM64Split() throws {
        let apks = [
            "base.apk": Self.apk(),
            "arm64.apk": Self.apk(split: "config.arm64_v8a", files: [Self.library("arm64-v8a")]),
            "armv7.apk": Self.apk(split: "config.armeabi_v7a", files: [Self.library("armeabi-v7a", machine: 40)]),
        ]

        let report = try Self.analyze(apks).report

        #expect(report.abis == ["arm64-v8a", "armeabi-v7a"])
        #expect(report.selectedABI == "arm64-v8a")
        #expect(report.nativeLibraries.map(\.path) == ["lib/arm64-v8a/libmain.so"])
        #expect(report.nativeLibraries.map(\.split) == ["config.arm64_v8a"])
        #expect(report.compatibility.status == .ready)
    }

    @Test func anARM64SplitWithOtherCodeIsIncompatible() throws {
        let apks = ["base.apk": Self.apk(), "arm64.apk": Self.apk(split: "config.arm64_v8a", files: [Self.library("arm64-v8a", machine: 62)])]

        let report = try Self.analyze(apks).report

        #expect(report.compatibility.issues.contains(.wrongArchitecture("libmain.so")))
        #expect(report.compatibility.status == .incompatible)
    }

    @Test func aSetWhoseABISplitsAre32BitOnlyIsIncompatible() throws {
        // The choice skips the split, and the set still says why it cannot run.
        let apks = ["base.apk": Self.apk(), "armv7.apk": Self.apk(split: "config.armeabi_v7a", files: [Self.library("armeabi-v7a", machine: 40)])]

        let report = try Self.analyze(apks).report

        #expect(report.compatibility.issues.contains(.only32Bit(["armeabi-v7a"])))
        #expect(report.compatibility.status == .incompatible)
    }

    @Test func aSplitWithCodeNeedsJava() throws {
        let feature = Self.apk(split: "dynamic", manifest: [.android("isFeatureSplit", 0x0101_055B, .bool(true))],
                               application: [.android("hasCode", 0x0101_000C, .bool(true))])

        #expect(!(try Self.analyze(["base.apk": Self.apk()]).report.compatibility.issues.contains(.needsART)))
        #expect(try Self.analyze(["base.apk": Self.apk(), "feature.apk": feature]).report.compatibility.issues.contains(.needsART))
    }

    @Test func aLibraryRecordedBeforeSplitsIsTheBases() throws {
        // library.json from before step b5a names no split.
        let json = #"{"name":"libmain.so","path":"lib/arm64-v8a/libmain.so","size":1,"isCompressed":false}"#
        let library = try JSONDecoder().decode(NativeLibrary.self, from: Data(json.utf8))

        #expect(library.split == nil)
        #expect(library.path == "lib/arm64-v8a/libmain.so")
    }

    @Test func theSetDecidesWhetherSplitsAreMissing() throws {
        // The base says it needs splits; the set that brings one does not.
        let base = Self.apk(manifest: [.android("isSplitRequired", 0x0101_0591, .bool(true))])

        let alone = try Self.analyze(["base.apk": base]).report
        let complete = try Self.analyze(["base.apk": base, "de.apk": Self.apk(split: "config.de")]).report

        #expect(alone.requiresSplits && alone.compatibility.status == .needsSplits)
        #expect(!complete.requiresSplits && complete.compatibility.status == .ready)
    }
}
