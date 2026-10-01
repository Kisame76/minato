import APKTestSupport
import Foundation
import Testing

@testable import APKKit

@Suite("AndroidManifest")
struct AndroidManifestTests {
    typealias E = AXMLBuilder.Element
    typealias A = AXMLBuilder.Attribute

    static let launcherFilter = E("intent-filter", [], [
        E("action", [.android("name", 0x0101_0003, .string("android.intent.action.MAIN"))]),
        E("category", [.android("name", 0x0101_0003, .string("android.intent.category.LAUNCHER"))]),
    ])

    static func manifest(_ attributes: [A] = [], _ children: [E]) -> E {
        E("manifest", [.plain("package", .string("org.example.game"))] + attributes, children)
    }

    static func parse(_ root: E) throws -> AndroidManifest {
        try AndroidManifest(xml: BinaryXML.parse(AXMLBuilder().build(root)))
    }

    @Test func readsIdentityAndVersions() throws {
        let manifest = try Self.parse(Self.manifest([
            .android("versionCode", 0x0101_021B, .int(42)),
            .android("versionName", 0x0101_021C, .string("1.2.3")),
        ], [
            E("uses-sdk", [
                .android("minSdkVersion", 0x0101_020C, .int(24)),
                .android("targetSdkVersion", 0x0101_0270, .int(34)),
            ]),
        ]))

        #expect(manifest.packageName == "org.example.game")
        #expect(manifest.versionCode == 42)
        #expect(manifest.versionName == .string("1.2.3"))
        #expect(manifest.minSdk == 24)
        #expect(manifest.targetSdk == 34)
    }

    @Test func combinesVersionCodeMajor() throws {
        let manifest = try Self.parse(Self.manifest([
            .android("versionCode", 0x0101_021B, .int(5)),
            .android("versionCodeMajor", 0x0101_0576, .int(1)),
        ], []))

        #expect(manifest.versionCode == 4_294_967_301)
    }

    @Test func defaultsSdkLevelsLikeAndroid() throws {
        let noSdk = try Self.parse(Self.manifest([], []))
        #expect(noSdk.minSdk == 1)
        #expect(noSdk.targetSdk == 1)

        let minOnly = try Self.parse(Self.manifest([], [E("uses-sdk", [.android("minSdkVersion", 0x0101_020C, .int(21))])]))
        #expect(minOnly.targetSdk == 21)
    }

    @Test func treatsPreviewCodenameAsDevelopmentSdk() throws {
        let manifest = try Self.parse(Self.manifest([], [
            E("uses-sdk", [.android("minSdkVersion", 0x0101_020C, .string("Baklava"))]),
        ]))

        #expect(manifest.minSdk == 10_000)
    }

    @Test func findsLauncherActivityAndQualifiesName() throws {
        let manifest = try Self.parse(Self.manifest([], [
            E("application", [], [
                E("activity", [.android("name", 0x0101_0003, .string(".SettingsActivity"))]),
                E("activity", [.android("name", 0x0101_0003, .string(".GameActivity"))], [Self.launcherFilter]),
            ]),
        ]))

        #expect(manifest.launcherActivity == "org.example.game.GameActivity")
    }

    @Test func resolvesLauncherAliasToItsTarget() throws {
        let manifest = try Self.parse(Self.manifest([], [
            E("application", [], [
                E("activity", [.android("name", 0x0101_0003, .string("org.example.game.RealActivity"))]),
                E("activity-alias", [
                    .android("name", 0x0101_0003, .string(".Launcher")),
                    .android("targetActivity", 0x0101_0202, .string("RealActivity")),
                ], [Self.launcherFilter]),
            ]),
        ]))

        #expect(manifest.launcherActivity == "org.example.game.RealActivity")
    }

    @Test func ignoresDisabledLauncher() throws {
        let manifest = try Self.parse(Self.manifest([], [
            E("application", [], [
                E("activity", [
                    .android("name", 0x0101_0003, .string(".Old")),
                    .android("enabled", 0x0101_000E, .bool(false)),
                ], [Self.launcherFilter]),
                E("activity", [.android("name", 0x0101_0003, .string(".New"))], [Self.launcherFilter]),
            ]),
        ]))

        #expect(manifest.launcherActivity == "org.example.game.New")
    }

    @Test func detectsNativeActivityWithoutJava() throws {
        let manifest = try Self.parse(Self.manifest([], [
            E("application", [.android("hasCode", 0x0101_000C, .bool(false))], [
                E("activity", [.android("name", 0x0101_0003, .string("android.app.NativeActivity"))], [
                    E("meta-data", [
                        .android("name", 0x0101_0003, .string("android.app.lib_name")),
                        .android("value", 0x0101_0024, .string("game")),
                    ]),
                    E("meta-data", [
                        .android("name", 0x0101_0003, .string("android.app.func_name")),
                        .android("value", 0x0101_0024, .string("game_onCreate")),
                    ]),
                    Self.launcherFilter,
                ]),
            ]),
        ]))

        #expect(manifest.hasCode == false)
        #expect(manifest.usesNativeActivity)
        #expect(manifest.nativeLibraryName == "game")
        #expect(manifest.nativeFunctionName == "game_onCreate")
    }

    @Test func readsTheLaunchersScreenOrientation() throws {
        // The launcher's own, another activity's does not count.
        let direct = try Self.parse(Self.manifest([], [
            E("application", [], [
                E("activity", [.android("name", 0x0101_0003, .string(".Other")), .android("screenOrientation", 0x0101_001E, .int(1))]),
                E("activity", [.android("name", 0x0101_0003, .string(".Main")), .android("screenOrientation", 0x0101_001E, .int(6))],
                  [Self.launcherFilter]),
            ]),
        ]))
        // An alias has none of its own: the activity it names decides. Found by its resource ID alone.
        let aliased = try Self.parse(Self.manifest([], [
            E("application", [], [
                E("activity", [
                    .android("name", 0x0101_0003, .string("org.example.game.RealActivity")),
                    A(namespace: AXMLBuilder.androidNamespace, name: "", resourceID: 0x0101_001E, value: .int(0)),
                ]),
                E("activity-alias", [
                    .android("name", 0x0101_0003, .string(".Launcher")),
                    .android("targetActivity", 0x0101_0202, .string("RealActivity")),
                ], [Self.launcherFilter]),
            ]),
        ]))
        let unset = try Self.parse(Self.manifest([], [
            E("application", [], [E("activity", [.android("name", 0x0101_0003, .string(".Main"))], [Self.launcherFilter])]),
        ]))

        #expect(direct.screenOrientation == 6)
        #expect(aliased.screenOrientation == 0)
        #expect(unset.screenOrientation == nil)
    }

    /// ADR-0038, decision 1.3: whether a session may turn depends on the launcher's `configChanges`, which
    /// aapt2 writes as a hexadecimal integer.
    @Test func readsTheLaunchersConfigChanges() throws {
        let direct = try Self.parse(Self.manifest([], [
            E("application", [], [
                E("activity", [.android("name", 0x0101_0003, .string(".Other")), .android("configChanges", 0x0101_001F, .hex(0x80))]),
                E("activity", [.android("name", 0x0101_0003, .string(".Main")), .android("configChanges", 0x0101_001F, .hex(0x480))],
                  [Self.launcherFilter]),
            ]),
        ]))
        let unset = try Self.parse(Self.manifest([], [
            E("application", [], [E("activity", [.android("name", 0x0101_0003, .string(".Main"))], [Self.launcherFilter])]),
        ]))

        #expect(direct.configChanges == 0x480)
        #expect(unset.configChanges == nil)
    }

    @Test func defaultsNativeLibraryNameToMain() throws {
        let manifest = try Self.parse(Self.manifest([], [
            E("application", [], [
                E("activity", [.android("name", 0x0101_0003, .string("android.app.NativeActivity"))], [Self.launcherFilter]),
            ]),
        ]))

        #expect(manifest.hasCode)
        #expect(manifest.nativeLibraryName == "main")
        #expect(manifest.nativeFunctionName == "ANativeActivity_onCreate")
    }

    @Test func aJavaActivityHasNoNativeFunction() throws {
        let manifest = try Self.parse(Self.manifest([], [
            E("application", [], [
                E("activity", [.android("name", 0x0101_0003, .string(".Main"))], [
                    E("meta-data", [
                        .android("name", 0x0101_0003, .string("android.app.func_name")),
                        .android("value", 0x0101_0024, .string("game_onCreate")),
                    ]),
                    Self.launcherFilter,
                ]),
            ]),
        ]))

        #expect(manifest.nativeFunctionName == nil)
    }

    @Test func readsGraphicsRequirements() throws {
        let manifest = try Self.parse(Self.manifest([], [
            E("uses-feature", [.android("glEsVersion", 0x0101_0281, .hex(0x0003_0001)), .android("required", 0x0101_028E, .bool(false))]),
            E("uses-feature", [.android("glEsVersion", 0x0101_0281, .hex(0x0003_0000))]),
            E("uses-feature", [
                .android("name", 0x0101_0003, .string("android.hardware.vulkan.version")),
                .android("version", nil, .hex(0x0040_1000)),
            ]),
            E("uses-feature", [
                .android("name", 0x0101_0003, .string("android.hardware.vulkan.level")),
                .android("version", nil, .int(1)),
                .android("required", 0x0101_028E, .bool(true)),
            ]),
            E("uses-feature", [.android("name", 0x0101_0003, .string("android.hardware.opengles.aep"))]),
        ]))

        #expect(manifest.requiredGLESVersion == 0x0003_0000)
        #expect(manifest.requiredVulkanVersion == 0x0040_1000)
        #expect(manifest.requiredVulkanLevel == 1)
        #expect(manifest.requiresAndroidExtensionPack)
    }

    @Test func ignoresOptionalGraphicsFeatures() throws {
        let manifest = try Self.parse(Self.manifest([], [
            E("uses-feature", [
                .android("name", 0x0101_0003, .string("android.hardware.vulkan.version")),
                .android("version", nil, .hex(0x0040_1000)),
                .android("required", 0x0101_028E, .bool(false)),
            ]),
        ]))

        #expect(manifest.requiredGLESVersion == nil)
        #expect(manifest.requiredVulkanVersion == nil)
    }

    @Test func detectsRequiredSplitsFromApplicationAttribute() throws {
        let manifest = try Self.parse(Self.manifest([], [
            E("application", [.android("isSplitRequired", 0x0101_0591, .bool(true))]),
        ]))

        #expect(manifest.requiresSplits)
    }

    @Test func detectsRequiredSplitsFromPlayMetaData() throws {
        let manifest = try Self.parse(Self.manifest([], [
            E("application", [], [
                E("meta-data", [
                    .android("name", 0x0101_0003, .string("com.android.vending.splits.required")),
                    .android("value", 0x0101_0024, .bool(true)),
                ]),
            ]),
        ]))

        #expect(manifest.requiresSplits)
    }

    @Test func detectsRequiredSplitTypes() throws {
        let manifest = try Self.parse(Self.manifest([.android("requiredSplitTypes", nil, .string("base__abi,base__density"))], []))

        #expect(manifest.requiresSplits)
    }

    @Test func recognisesSplitAPK() throws {
        let manifest = try Self.parse(Self.manifest([.plain("split", .string("config.arm64_v8a"))], []))

        #expect(manifest.splitName == "config.arm64_v8a")
        #expect(!manifest.requiresSplits)
    }

    @Test func readsExtractNativeLibs() throws {
        let off = try Self.parse(Self.manifest([], [
            E("application", [.android("extractNativeLibs", 0x0101_04EA, .bool(false))]),
        ]))
        let unset = try Self.parse(Self.manifest([], [E("application")]))

        #expect(off.extractNativeLibs == false)
        #expect(unset.extractNativeLibs == nil)
    }

    @Test func readsObfuscatedAttributesThroughResourceIDs() throws {
        let manifest = try Self.parse(E("manifest", [
            .plain("package", .string("org.example.obfuscated")),
            A(namespace: AXMLBuilder.androidNamespace, name: "", resourceID: 0x0101_021B, value: .int(7)),
        ], [
            E("uses-sdk", [A(namespace: AXMLBuilder.androidNamespace, name: "", resourceID: 0x0101_020C, value: .int(26))]),
            E("application", [A(namespace: AXMLBuilder.androidNamespace, name: "", resourceID: 0x0101_0002, value: .reference(0x7F08_0000))]),
        ]))

        #expect(manifest.versionCode == 7)
        #expect(manifest.minSdk == 26)
        #expect(manifest.icon == .reference(0x7F08_0000))
    }

    @Test func collectsPermissionsMetaDataLibrariesAndComponents() throws {
        let manifest = try Self.parse(Self.manifest([], [
            E("uses-permission", [.android("name", 0x0101_0003, .string("android.permission.INTERNET"))]),
            E("uses-permission-sdk-23", [.android("name", 0x0101_0003, .string("com.android.vending.CHECK_LICENSE"))]),
            E("application", [
                .android("label", 0x0101_0001, .reference(0x7F0A_0001)),
                .android("icon", 0x0101_0002, .reference(0x7F08_0000)),
            ], [
                E("meta-data", [
                    .android("name", 0x0101_0003, .string("com.google.android.gms.version")),
                    .android("value", 0x0101_0024, .reference(0x7F0B_0000)),
                ]),
                E("uses-library", [.android("name", 0x0101_0003, .string("org.apache.http.legacy"))]),
                E("service", [.android("name", 0x0101_0003, .string("com.google.firebase.messaging.FirebaseMessagingService"))]),
                E("receiver", [.android("name", 0x0101_0003, .string(".BootReceiver"))]),
            ]),
        ]))

        #expect(manifest.permissions == ["android.permission.INTERNET", "com.android.vending.CHECK_LICENSE"])
        #expect(manifest.metaData["com.google.android.gms.version"] == .reference(0x7F0B_0000))
        #expect(manifest.usesLibraries == ["org.apache.http.legacy"])
        #expect(manifest.components == ["com.google.firebase.messaging.FirebaseMessagingService", "org.example.game.BootReceiver"])
        #expect(manifest.label == .reference(0x7F0A_0001))
        #expect(manifest.icon == .reference(0x7F08_0000))
    }

    @Test func requiresPackageName() throws {
        #expect(throws: APKError.self) {
            _ = try Self.parse(E("manifest", [], [E("application")]))
        }
        #expect(throws: APKError.self) {
            _ = try Self.parse(E("resources", [.plain("package", .string("x"))], []))
        }
    }
}
