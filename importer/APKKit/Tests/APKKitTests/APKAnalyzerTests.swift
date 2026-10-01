import APKTestSupport
import Foundation
import Testing

@testable import APKKit

@Suite("APKAnalyzer")
struct APKAnalyzerTests {
    typealias E = AXMLBuilder.Element
    typealias Config = ARSCBuilder.Config

    static let launcherFilter = E("intent-filter", [], [
        E("action", [.android("name", 0x0101_0003, .string("android.intent.action.MAIN"))]),
        E("category", [.android("name", 0x0101_0003, .string("android.intent.category.LAUNCHER"))]),
    ])

    /// Minimal complete APK: manifest, optional resources and arbitrary extra files.
    struct TestAPK {
        var manifestAttributes: [AXMLBuilder.Attribute] = []
        var manifestChildren: [E] = []
        var resources: ARSCBuilder?
        var files: [ZipBuilder.Entry] = []

        func archive() throws -> ZipArchive {
            var zip = ZipBuilder()
            let root = E("manifest", [.plain("package", .string("org.example.game"))] + manifestAttributes, manifestChildren)
            zip.add("AndroidManifest.xml", AXMLBuilder().build(root))
            if let resources { zip.add("resources.arsc", resources.build(), method: .stored) }
            zip.entries += files
            return try ZipArchive(data: zip.build().data)
        }
    }

    static func library(_ path: String, align: UInt64 = 0x4000, machine: UInt16 = 183, method: ZipBuilder.Method = .deflated,
                        zipAlignment: Int? = nil) -> ZipBuilder.Entry {
        var elf = ELFBuilder(segments: [.load(align: align), .load(align: align), .dynamic])
        elf.machine = machine
        return ZipBuilder.Entry(name: path, content: elf.build() + Data(repeating: 0xAA, count: 5_000), method: method, alignment: zipAlignment)
    }

    static func application(_ attributes: [AXMLBuilder.Attribute] = [], _ children: [E] = []) -> E {
        E("application", attributes, children)
    }

    @Test func resolvesLabelAndVersionNameThroughResources() throws {
        var resources = ARSCBuilder()
        let label = resources.add("string", "app_name", [(.default, .string("My Game")), (Config(language: "de"), .string("Mein Spiel"))])
        let version = resources.add("string", "version", [(.default, .string("2.0.1"))])
        let apk = TestAPK(
            manifestAttributes: [.android("versionCode", 0x0101_021B, .int(201)), .android("versionName", 0x0101_021C, .reference(version))],
            manifestChildren: [Self.application([.android("label", 0x0101_0001, .reference(label))])],
            resources: resources)
        let report = try APKAnalyzer.analyze(apk.archive(), preferredLanguages: ["de"])

        #expect(report.packageName == "org.example.game")
        #expect(report.label == "Mein Spiel")
        #expect(report.versionName == "2.0.1")
        #expect(report.versionCode == 201)
    }

    @Test func fallsBackToPackageNameWithoutLabel() throws {
        let report = try APKAnalyzer.analyze(TestAPK().archive(), preferredLanguages: [])

        #expect(report.label == "org.example.game")
    }

    @Test func selectsARM64FromFatAPK() throws {
        let apk = TestAPK(files: [
            Self.library("lib/armeabi-v7a/libgame.so"),
            Self.library("lib/arm64-v8a/libgame.so"),
            Self.library("lib/arm64-v8a/libc++_shared.so"),
            Self.library("lib/x86_64/libgame.so", machine: 62),
        ])
        let report = try APKAnalyzer.analyze(apk.archive(), preferredLanguages: [])

        #expect(report.abis == ["arm64-v8a", "armeabi-v7a", "x86_64"])
        #expect(report.selectedABI == "arm64-v8a")
        #expect(report.nativeLibraries.map(\.name) == ["libgame.so", "libc++_shared.so"])
        #expect(report.compatibility.status == .ready)
    }

    @Test func marks32BitOnlyAPKIncompatible() throws {
        let apk = TestAPK(files: [Self.library("lib/armeabi-v7a/libgame.so"), Self.library("lib/armeabi/libold.so")])
        let report = try APKAnalyzer.analyze(apk.archive(), preferredLanguages: [])

        #expect(report.compatibility.status == .incompatible)
        #expect(report.compatibility.issues.contains(.only32Bit(["armeabi", "armeabi-v7a"])))
    }

    @Test func marksAPKWithoutARMCodeIncompatible() throws {
        let apk = TestAPK(files: [Self.library("lib/x86_64/libgame.so", machine: 62)])
        let report = try APKAnalyzer.analyze(apk.archive(), preferredLanguages: [])

        #expect(report.compatibility.status == .incompatible)
        #expect(report.compatibility.issues.contains(.noARM64(["x86_64"])))
    }

    @Test func flagsForeignLibraryInARM64Folder() throws {
        let apk = TestAPK(files: [Self.library("lib/arm64-v8a/libgame.so", machine: 62)])
        let report = try APKAnalyzer.analyze(apk.archive(), preferredLanguages: [])

        #expect(report.compatibility.status == .incompatible)
        #expect(report.compatibility.issues.contains(.wrongArchitecture("libgame.so")))
    }

    @Test func reportsBaseThatNeedsSplits() throws {
        let apk = TestAPK(manifestChildren: [Self.application([.android("isSplitRequired", 0x0101_0591, .bool(true))])])
        let report = try APKAnalyzer.analyze(apk.archive(), preferredLanguages: [])

        #expect(report.compatibility.status == .needsSplits)
    }

    @Test func marksLoneSplitAPKIncompatible() throws {
        let apk = TestAPK(manifestAttributes: [.plain("split", .string("config.arm64_v8a"))])
        let report = try APKAnalyzer.analyze(apk.archive(), preferredLanguages: [])

        #expect(report.compatibility.status == .incompatible)
        #expect(report.compatibility.issues.contains(.isSplitAPK("config.arm64_v8a")))
    }

    @Test func pureJavaAppNeedsART() throws {
        let apk = TestAPK(manifestChildren: [Self.application([], [
            E("activity", [.android("name", 0x0101_0003, .string(".Main"))], [Self.launcherFilter]),
        ])])
        let report = try APKAnalyzer.analyze(apk.archive(), preferredLanguages: [])

        #expect(report.compatibility.status == .ready)
        #expect(report.runtime == .art)
        #expect(report.compatibility.issues.contains(.needsART))
        #expect(report.launcherActivity == "org.example.game.Main")
    }

    @Test func nativeActivityGameRunsWithoutART() throws {
        let apk = TestAPK(
            manifestChildren: [Self.application([.android("hasCode", 0x0101_000C, .bool(false))], [
                E("activity", [.android("name", 0x0101_0003, .string("android.app.NativeActivity"))], [
                    E("meta-data", [
                        .android("name", 0x0101_0003, .string("android.app.lib_name")),
                        .android("value", 0x0101_0024, .string("game")),
                    ]),
                    Self.launcherFilter,
                ]),
            ])],
            files: [Self.library("lib/arm64-v8a/libgame.so")])
        let report = try APKAnalyzer.analyze(apk.archive(), preferredLanguages: [])

        #expect(report.runtime == .nativeActivity(library: "game"))
        #expect(report.nativeFunction == "ANativeActivity_onCreate")
        #expect(!report.compatibility.issues.contains(.needsART))
    }

    @Test func aReportFromBeforeTheFunctionNameHasNone() throws {
        let apk = TestAPK(manifestChildren: [Self.application([], [
            E("activity", [.android("name", 0x0101_0003, .string("android.app.NativeActivity"))], [Self.launcherFilter]),
        ])], files: [Self.library("lib/arm64-v8a/libmain.so")])
        let report = try APKAnalyzer.analyze(apk.archive(), preferredLanguages: [])
        var stored = try #require(JSONSerialization.jsonObject(with: JSONEncoder().encode(report)) as? [String: Any])
        stored["nativeFunction"] = nil

        let decoded = try JSONDecoder().decode(APKReport.self, from: JSONSerialization.data(withJSONObject: stored))
        #expect(decoded.nativeFunction == nil)
        #expect(decoded.runtime == report.runtime)
    }

    @Test func probesNativeLibraries() throws {
        let apk = TestAPK(
            manifestChildren: [Self.application([.android("extractNativeLibs", 0x0101_04EA, .bool(false))])],
            files: [
                Self.library("lib/arm64-v8a/libold.so", align: 0x1000),
                Self.library("lib/arm64-v8a/libnew.so", align: 0x4000, method: .stored, zipAlignment: 16_384),
            ])
        let report = try APKAnalyzer.analyze(apk.archive(), preferredLanguages: [])
        let old = try #require(report.nativeLibraries.first { $0.name == "libold.so" })
        let new = try #require(report.nativeLibraries.first { $0.name == "libnew.so" })

        #expect(old.elf?.pageAlignment == 4_096)
        #expect(old.isCompressed)
        #expect(new.elf?.pageAlignment == 16_384)
        #expect(!new.isCompressed)
        #expect(new.isZipAligned16K == true)
        #expect(report.extractNativeLibs == false)
        #expect(report.compatibility.issues.contains(.pageAligned4K(["libold.so"])))
    }

    @Test func picksHighestDensityRasterIcon() throws {
        var resources = ARSCBuilder()
        let icon = resources.add("mipmap", "ic_launcher", [
            (Config(density: 160, sdkVersion: 4), .string("res/mipmap-mdpi-v4/ic_launcher.png")),
            (Config(density: 480, sdkVersion: 4), .string("res/mipmap-xxhdpi-v4/ic_launcher.webp")),
            (Config(density: 0xFFFE, sdkVersion: 26), .string("res/mipmap-anydpi-v26/ic_launcher.xml")),
        ])
        let apk = TestAPK(
            manifestChildren: [Self.application([.android("icon", 0x0101_0002, .reference(icon))])],
            resources: resources,
            files: [
                .init(name: "res/mipmap-mdpi-v4/ic_launcher.png", content: Data("small".utf8)),
                .init(name: "res/mipmap-xxhdpi-v4/ic_launcher.webp", content: Data("large webp".utf8)),
                .init(name: "res/mipmap-anydpi-v26/ic_launcher.xml", content: Data("xml".utf8)),
            ])
        let archive = try apk.archive()
        let report = try APKAnalyzer.analyze(archive, preferredLanguages: [])

        #expect(report.iconPath == "res/mipmap-xxhdpi-v4/ic_launcher.webp")
        #expect(try APKAnalyzer.iconData(for: report, in: archive) == Data("large webp".utf8))
    }

    @Test func usesAdaptiveIconForegroundWhenOnlyXMLIsReferenced() throws {
        var resources = ARSCBuilder()
        let foreground = resources.add("mipmap", "ic_launcher_foreground", [
            (Config(density: 640, sdkVersion: 4), .string("res/mipmap-xxxhdpi-v4/ic_launcher_foreground.png")),
        ])
        let background = resources.add("color", "ic_launcher_background", [(.default, .color(0xFF00_6400))])
        let icon = resources.add("mipmap", "ic_launcher", [
            (Config(density: 0xFFFE, sdkVersion: 26), .string("res/mipmap-anydpi-v26/ic_launcher.xml")),
        ])
        let adaptive = AXMLBuilder().build(E("adaptive-icon", [], [
            E("background", [.android("drawable", 0x0101_0199, .reference(background))]),
            E("foreground", [.android("drawable", 0x0101_0199, .reference(foreground))]),
        ]))
        let apk = TestAPK(
            manifestChildren: [Self.application([.android("icon", 0x0101_0002, .reference(icon))])],
            resources: resources,
            files: [
                .init(name: "res/mipmap-anydpi-v26/ic_launcher.xml", content: adaptive),
                .init(name: "res/mipmap-xxxhdpi-v4/ic_launcher_foreground.png", content: Data("foreground".utf8)),
            ])
        let report = try APKAnalyzer.analyze(apk.archive(), preferredLanguages: [])

        #expect(report.iconPath == "res/mipmap-xxxhdpi-v4/ic_launcher_foreground.png")
    }

    @Test func fallsBackToLargestLauncherImage() throws {
        var resources = ARSCBuilder()
        let icon = resources.add("drawable", "ic_launcher", [
            (Config(density: 0xFFFE, sdkVersion: 24), .string("res/drawable-anydpi-v24/ic_launcher.xml")),
        ])
        let vector = AXMLBuilder().build(E("vector", [.android("width", nil, .string("24dp"))]))
        let apk = TestAPK(
            manifestChildren: [Self.application([.android("icon", 0x0101_0002, .reference(icon))])],
            resources: resources,
            files: [
                .init(name: "res/drawable-anydpi-v24/ic_launcher.xml", content: vector),
                .init(name: "res/mipmap-hdpi/ic_launcher.png", content: Data(count: 300)),
                .init(name: "res/mipmap-xxhdpi/ic_launcher.png", content: Data(count: 900)),
                .init(name: "res/drawable/splash.png", content: Data(count: 5_000)),
            ])
        let report = try APKAnalyzer.analyze(apk.archive(), preferredLanguages: [])

        #expect(report.iconPath == "res/mipmap-xxhdpi/ic_launcher.png")
    }

    @Test func detectsGameEngineFromLibraries() throws {
        let unity = TestAPK(files: [Self.library("lib/arm64-v8a/libunity.so"), Self.library("lib/arm64-v8a/libil2cpp.so"), Self.library("lib/arm64-v8a/libmain.so")])
        let unreal = TestAPK(files: [Self.library("lib/arm64-v8a/libUnreal.so")])
        let none = TestAPK(files: [Self.library("lib/arm64-v8a/libgame.so")])

        #expect(try APKAnalyzer.analyze(unity.archive(), preferredLanguages: []).engine == "Unity (IL2CPP)")
        #expect(try APKAnalyzer.analyze(unreal.archive(), preferredLanguages: []).engine == "Unreal Engine")
        #expect(try APKAnalyzer.analyze(none.archive(), preferredLanguages: []).engine == nil)
    }

    @Test func detectsGooglePlayDependencies() throws {
        let apk = TestAPK(manifestChildren: [
            E("uses-permission", [.android("name", 0x0101_0003, .string("com.android.vending.BILLING"))]),
            E("uses-permission", [.android("name", 0x0101_0003, .string("com.android.vending.CHECK_LICENSE"))]),
            Self.application([], [
                E("meta-data", [
                    .android("name", 0x0101_0003, .string("com.google.android.gms.version")),
                    .android("value", 0x0101_0024, .int(12_451_000)),
                ]),
                E("service", [.android("name", 0x0101_0003, .string("com.google.firebase.messaging.FirebaseMessagingService"))]),
            ]),
        ])
        let report = try APKAnalyzer.analyze(apk.archive(), preferredLanguages: [])

        #expect(Set(report.playDependencies) == [.playServices, .firebase, .billing, .licenseCheck])
        #expect(report.compatibility.issues.contains(.usesLicenseCheck))
    }

    @Test func notesGraphicsBeyondANGLEMetal() throws {
        let apk = TestAPK(manifestChildren: [
            E("uses-feature", [.android("glEsVersion", 0x0101_0281, .hex(0x0003_0002))]),
        ])
        let report = try APKAnalyzer.analyze(apk.archive(), preferredLanguages: [])

        #expect(report.requiredGLESVersion == 0x0003_0002)
        #expect(report.compatibility.issues.contains(.requiresGLES(0x0003_0002)))
    }

    @Test func requiresManifest() throws {
        var zip = ZipBuilder()
        zip.add("classes.dex", "dex")

        #expect(throws: APKError.entryNotFound("AndroidManifest.xml")) {
            _ = try APKAnalyzer.analyze(ZipArchive(data: zip.build().data), preferredLanguages: [])
        }
    }

    @Test func reportsTheLaunchersScreenOrientation() throws {
        // What game mode holds the screen in (ADR-0032).
        let landscape = try APKAnalyzer.analyze(ZipArchive(data: SampleAPK.make(nativeActivity: true, screenOrientation: 6)),
                                                preferredLanguages: [])
        let unset = try APKAnalyzer.analyze(ZipArchive(data: SampleAPK.make(nativeActivity: true)), preferredLanguages: [])

        #expect(landscape.runtime == .nativeActivity(library: "game"))
        #expect(landscape.screenOrientation == 6)
        #expect(unset.screenOrientation == nil)
    }

    @Test func reportsTheLaunchersConfigChanges() throws {
        // Whether a session may turn (ADR-0038).
        let turning = try APKAnalyzer.analyze(ZipArchive(data: SampleAPK.make(nativeActivity: true, configChanges: 0x480)),
                                              preferredLanguages: [])
        let unset = try APKAnalyzer.analyze(ZipArchive(data: SampleAPK.make(nativeActivity: true)), preferredLanguages: [])

        #expect(turning.configChanges == 0x480)
        #expect(unset.configChanges == nil)
    }
}
