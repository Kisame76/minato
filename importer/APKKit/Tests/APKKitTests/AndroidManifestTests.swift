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

    /// Without `<uses-sdk>` PackageImpl keeps its defaults, minSdk 1 and targetSdk 0 (ParsingUtils.java:52-54).
    @Test func defaultsSdkLevelsLikeAndroid() throws {
        let noSdk = try Self.parse(Self.manifest([], []))
        #expect(noSdk.minSdk == 1)
        #expect(noSdk.targetSdk == 0)
        #expect(noSdk.sdkRefusal == nil)

        let minOnly = try Self.parse(Self.manifest([], [E("uses-sdk", [.android("minSdkVersion", 0x0101_020C, .int(21))])]))
        #expect(minOnly.targetSdk == 21)
    }

    // MARK: - uses-sdk as Android 15 installs it

    static let minID: UInt32 = 0x0101_020C
    static let targetID: UInt32 = 0x0101_0270

    static func usesSdk(_ attributes: [A]) throws -> AndroidManifest {
        try parse(manifest([], [E("uses-sdk", attributes)]))
    }

    static func codename(_ code: String) -> String {
        "Requires development platform \(code) but this is a release platform."
    }

    /// A codename names a preview, which a release build such as Android 15 refuses to install
    /// (FrameworkParsingPackageUtils.java:298-330, 363-397); its level stands at CUR_DEVELOPMENT.
    @Test func treatsPreviewCodenameAsDevelopmentSdk() throws {
        let manifest = try Self.parse(Self.manifest([], [
            E("uses-sdk", [.android("minSdkVersion", 0x0101_020C, .string("Baklava"))]),
        ]))

        #expect(manifest.minSdk == 10_000)
        #expect(manifest.targetSdk == 10_000)
        #expect(manifest.sdkRefusal == Self.codename("Baklava"))

        let target = try Self.usesSdk([
            .android("minSdkVersion", Self.minID, .int(21)),
            .android("targetSdkVersion", Self.targetID, .string("Baklava")),
        ])
        #expect(target.minSdk == 21)
        #expect(target.sdkRefusal == Self.codename("Baklava"))
    }

    @Test func aMinimumAboveAndroid15() throws {
        #expect(try Self.usesSdk([.android("minSdkVersion", Self.minID, .int(35))]).sdkRefusal == nil)
        #expect(try Self.usesSdk([.android("minSdkVersion", Self.minID, .int(36))]).sdkRefusal
            == "Requires newer sdk version #36 (current version is #35)")
    }

    /// The installer's own parse reads a level as text (ApkLiteParseUtils.java:569-625): the raw string, else the typed
    /// value as TypedValue.coerceToString writes it (XmlBlock.java:281-301), and what Integer.parseInt refuses is a
    /// codename. A reference is "@" and its ID.
    @Test func aReferenceIsACodenameToTheInstaller() throws {
        let manifest = try Self.usesSdk([
            .android("minSdkVersion", Self.minID, .int(21)),
            .android("targetSdkVersion", Self.targetID, .reference(0x7F0A_0001)),
        ])
        #expect(manifest.sdkRefusal == Self.codename("@2131361793"))
        #expect(manifest.sdkReference == nil)
    }

    @Test func otherTypesAreCodenamesToTheInstallerToo() throws {
        let codes: [(AXMLBuilder.Value, String)] = [
            (.hex(0x1E), "0x1e"),
            (.bool(true), "true"),
            (.typed(type: 0x04, data: Float(30).bitPattern), "30.0"),
            (.typed(type: 0x02, data: 0x0101_0000), "?16842752"),
            (.typed(type: 0x1C, data: 0xFF00_00FF), "#ff0000ff"),
        ]
        for (value, code) in codes {
            let manifest = try Self.usesSdk([.android("targetSdkVersion", Self.targetID, value)])
            #expect(manifest.sdkRefusal == Self.codename(code), "\(value)")
        }
        // The other integer types read as their decimal, in both parses.
        let manifest = try Self.usesSdk([.android("targetSdkVersion", Self.targetID, .typed(type: 0x13, data: 30))])
        #expect(manifest.sdkRefusal == nil)
        #expect(manifest.targetSdk == 30)
    }

    /// A number as a string passes the installer's Integer.parseInt, but the package parser takes every string as a
    /// codename (ParsingPackageUtils.java:1666-1676).
    @Test func aNumberAsTextIsACodenameToThePackageParser() throws {
        let manifest = try Self.usesSdk([.android("targetSdkVersion", Self.targetID, .string("30"))])
        #expect(manifest.sdkRefusal == Self.codename("30"))
    }

    /// No aapt keeps a raw string beside a typed value, but a file may: the installer reads the raw string, the
    /// package parser the typed value, and its level counts. A reference it would resolve, which Minato does not.
    @Test func aNumericRawValueOverATypedOne() throws {
        let hex = try Self.usesSdk([.android("targetSdkVersion", Self.targetID, .hex(0x1E), raw: "21")])
        #expect(hex.sdkRefusal == nil)
        #expect(hex.targetSdk == 30)

        let reference = try Self.usesSdk([.android("targetSdkVersion", Self.targetID, .reference(0x7F0A_0001), raw: "21")])
        #expect(reference.sdkRefusal == nil)
        #expect(reference.sdkReference == "targetSdkVersion")
    }

    /// The installer finds a level by its namespace and name, the package parser by its resource ID.
    @Test func eachParseFindsTheLevelItsOwnWay() throws {
        let obfuscated = try Self.usesSdk([
            .android("minSdkVersion", Self.minID, .int(21)),
            .android("a", Self.targetID, .int(30)),
        ])
        #expect(obfuscated.sdkRefusal == nil)
        #expect(obfuscated.targetSdk == 30)

        let unnumbered = try Self.usesSdk([
            .android("minSdkVersion", Self.minID, .int(21)),
            .android("targetSdkVersion", nil, .string("Baklava")),
        ])
        #expect(unnumbered.sdkRefusal == Self.codename("Baklava"))
        #expect(unnumbered.targetSdk == 21)
    }

    /// RetrieveAttributes walks the attributes once, as if sorted by resource ID (AttributeResolution.cpp:352-420), so
    /// the package parser misses a minSdkVersion after the targetSdkVersion.
    @Test func thePackageParserReadsTheLevelsInIDOrder() throws {
        let manifest = try Self.usesSdk([
            .android("targetSdkVersion", Self.targetID, .int(30)),
            .android("minSdkVersion", Self.minID, .int(21)),
        ])
        #expect(manifest.sdkRefusal == nil)
        #expect(manifest.minSdk == 1)
        #expect(manifest.targetSdk == 30)
    }

    /// TYPE_NULL without a raw string makes the installer's getAttributeValue throw, and Android refuses the file as
    /// unparsable (ApkLiteParseUtils.java:390). With one, the package parser takes the level as absent.
    @Test func aNullLevel() throws {
        #expect(throws: APKError.self) {
            try Self.usesSdk([.android("targetSdkVersion", Self.targetID, .typed(type: 0x00, data: 0))])
        }
        let raw = try Self.usesSdk([
            .android("minSdkVersion", Self.minID, .int(21)),
            .android("targetSdkVersion", Self.targetID, .typed(type: 0x00, data: 0), raw: "30"),
        ])
        #expect(raw.sdkRefusal == nil)
        #expect(raw.targetSdk == 21)
    }

    /// A string the pool cannot give throws where Android reads it (StringBlock.java:106-126, android_util_StringBlock
    /// .cpp:78-88), and Android refuses the file as unparsable: the installer reads any raw string index from 0 up
    /// (XmlBlock.java:287), the package parser a typed string's (TypedArray.java:1389, 1395-1404).
    @Test func aStringThePoolCannotGiveIsUnparsable() throws {
        var outside = A.android("minSdkVersion", Self.minID, .int(21))
        outside.rawIndex = 4096
        #expect(throws: APKError.self) { try Self.usesSdk([outside]) }
        #expect(throws: APKError.self) { try Self.usesSdk([.android("minSdkVersion", Self.minID, .typed(type: 0x03, data: 30))]) }
        // An index with its top bit set is no raw string, as for 0xFFFFFFFF.
        outside.rawIndex = 0x8000_0000
        #expect(try Self.usesSdk([outside]).minSdk == 21)
        // After the installer's refusal, the package parser never reads the string.
        let refused = try Self.usesSdk([.android("minSdkVersion", Self.minID, .typed(type: 0x03, data: 30), raw: "Baklava")])
        #expect(refused.sdkRefusal == Self.codename("Baklava"))
    }

    /// RetrieveAttributes turns @null into TYPE_NULL, so the package parser takes the level as absent.
    @Test func atNullIsAbsentToThePackageParser() throws {
        let manifest = try Self.usesSdk([
            .android("minSdkVersion", Self.minID, .int(21)),
            .android("targetSdkVersion", Self.targetID, .reference(0), raw: "30"),
        ])
        #expect(manifest.sdkRefusal == nil)
        #expect(manifest.sdkReference == nil)
        #expect(manifest.targetSdk == 21)
    }

    /// Integer.parseInt: a sign, then decimal digits of any script, within Int32.
    @Test func theInstallerReadsNumbersAsIntegerParseInt() {
        #expect(UsesSdk.parseInt("+30") == 30)
        #expect(UsesSdk.parseInt("-1") == -1)
        #expect(UsesSdk.parseInt("٣٠") == 30)
        #expect(UsesSdk.parseInt("-2147483648") == -2_147_483_648)
        for text in ["", "+", "-", " 30", "30 ", "0x1e", "2147483648", "3.0", "1e2"] {
            #expect(UsesSdk.parseInt(text) == nil, "\(text)")
        }
        let signed = try? Self.usesSdk([.android("targetSdkVersion", Self.targetID, .int(30), raw: "+30")])
        #expect(signed?.sdkRefusal == nil)
        #expect(signed?.targetSdk == 30)
    }

    /// Both parses take each `<uses-sdk>` in turn: the last one's levels count, and the first refusal stops.
    @Test func everyUsesSdkInTurn() throws {
        let manifest = try Self.parse(Self.manifest([], [
            E("uses-sdk", [.android("minSdkVersion", Self.minID, .int(21)), .android("targetSdkVersion", Self.targetID, .int(28))]),
            E("uses-sdk", [.android("minSdkVersion", Self.minID, .int(23)), .android("targetSdkVersion", Self.targetID, .int(30))]),
        ]))
        #expect(manifest.minSdk == 23)
        #expect(manifest.targetSdk == 30)

        let refused = try Self.parse(Self.manifest([], [
            E("uses-sdk", [.android("minSdkVersion", Self.minID, .string("Baklava"))]),
            E("uses-sdk", [.android("minSdkVersion", Self.minID, .int(36))]),
        ]))
        #expect(refused.sdkRefusal == Self.codename("Baklava"))
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

    /// ADR-0054 c2a: android.permission.INTERNET as Android 15's package parser requests it, from <uses-permission>,
    /// <uses-permission-sdk-23> or <uses-permission-sdk-m>, only where its int-typed SDK bounds include 35.
    @Test func requestsInternetWithinAndroid15sBounds() throws {
        func internet(_ element: String, _ extra: [A] = []) throws -> Bool {
            try Self.parse(Self.manifest([], [
                E(element, [.android("name", 0x0101_0003, .string("android.permission.INTERNET"))] + extra),
            ])).requestsInternet
        }
        #expect(try internet("uses-permission"))
        #expect(try internet("uses-permission-sdk-23"))
        #expect(try internet("uses-permission-sdk-m"))
        #expect(try !internet("uses-permission", [.android("maxSdkVersion", 0x0101_0271, .int(34))]))
        #expect(try internet("uses-permission", [.android("maxSdkVersion", 0x0101_0271, .int(35))]))
        #expect(try !internet("uses-permission", [.android("minSdkVersion", 0x0101_020C, .int(36))]))
        // Android 15's bounds default to Integer.MIN_VALUE and MAX_VALUE; an explicit 0 is a bound like any other
        // (ParsingPackageUtils.parseMinOrMaxSdkVersion), unlike the parser before R, where 0 meant none.
        #expect(try !internet("uses-permission", [.android("maxSdkVersion", 0x0101_0271, .int(0))]))
        #expect(try internet("uses-permission", [.android("minSdkVersion", 0x0101_020C, .int(0))]))
        // A bound that is not an int, such as a string, is not read, as the parser reads only int-typed ones.
        #expect(try internet("uses-permission", [.android("maxSdkVersion", 0x0101_0271, .string("22"))]))
        #expect(try !Self.parse(Self.manifest([], [
            E("uses-permission", [.android("name", 0x0101_0003, .string("android.permission.CAMERA"))]),
        ])).requestsInternet)
        #expect(try !Self.parse(Self.manifest([], [])).requestsInternet)
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

    // MARK: - The installer's reading (ApkLiteParseUtils.java:406-440, 685-773; ADR-0048 b2)

    /// Why the manifest is refused, or nil.
    static func refusal(_ root: E) -> String? {
        do {
            _ = try parse(root)
            return nil
        } catch APKError.invalidManifest(let reason) {
            return reason
        } catch {
            return "\(error)"
        }
    }

    @Test func thePackageSplitAndConfigForSplitAreReadRawFirstOnly() throws {
        // Android reads them so everywhere: its installer, its package parser (ParsingPackageUtils.java:702-703,
        // 751-752) and the guest's ManifestPackage (framework/java/android/app/ManifestPackage.java:192). A typed value
        // beside the raw string is no second reading.
        let package = A(namespace: nil, name: "package", resourceID: nil, value: .string("org.example.typed"), raw: "org.example.raw")
        let configForSplit = A(namespace: nil, name: "configForSplit", resourceID: nil, value: .string("feature"), raw: "other")
        let manifest = try Self.parse(E("manifest", [package, .plain("split", .bool(true)), configForSplit], []))
        #expect(manifest.packageName == "org.example.raw")
        // coerceToString makes a typed boolean the split "true".
        #expect(manifest.splitName == "true")
        #expect(manifest.configForSplit == "other")
    }

    @Test(arguments: [("nodot", "must have at least one '.' separator"), ("", "Invalid filename"), ("org.1x", "bad character '1'")])
    func aPackageNameAndroidRefusesIsRefused(name: String, problem: String) {
        // FrameworkParsingPackageUtils.validateName (:72-103), from ApkLiteParseUtils.java:701-709.
        #expect(Self.refusal(E("manifest", [.plain("package", .string(name))], [])) == "Invalid manifest package: \(problem)")
    }

    @Test func anEmptySplitIsTheBase() throws {
        // ApkLiteParseUtils.java:711-715.
        #expect(try Self.parse(Self.manifest([.plain("split", .string(""))], [])).splitName == nil)
    }

    @Test func aSplitAndroidRefusesIsRefused() {
        #expect(Self.refusal(Self.manifest([.plain("split", .string("x-y"))], [])) == "Invalid manifest split: bad character '-'")
        // TYPE_NULL throws in getAttributeValue, and Android's parse fails.
        #expect(Self.refusal(Self.manifest([.plain("split", .typed(type: 0x00, data: 0))], []))
            == "Not a valid APK: Android cannot read the manifest's split")
    }

    @Test func versionsAsTheInstallerAndThePackageParserReadThem() throws {
        // The installer reads integer types by name (getAttributeIntValue, XmlBlock.java:484-530). The package parser
        // reads by resource ID with TypedArray.getInteger (PackageImpl.java:2721-2723), which throws on a string
        // (TypedArray.java:651), and Android's parse fails.
        for (attribute, name) in [
            (A.android("versionCode", 0x0101_021B, .string("0")), "versionCode"),
            (A.android("versionCodeMajor", 0x0101_0576, .string("2")), "versionCodeMajor"),
            (A(namespace: AXMLBuilder.androidNamespace, name: "", resourceID: 0x0101_021B, value: .string("5")), "versionCode"),
        ] {
            #expect(Self.refusal(Self.manifest([attribute], [])) == "Not a valid APK: Android cannot read the manifest's \(name)")
        }
        // An attribute only its name finds is 0 to the package parser: the two readings differ.
        let minor = A(namespace: AXMLBuilder.androidNamespace, name: "versionCode", resourceID: nil, value: .int(5))
        #expect(Self.refusal(Self.manifest([minor], [])) == "Not a valid APK: its manifest's versionCode reads as 5 and as 0")
        let major = A(namespace: AXMLBuilder.androidNamespace, name: "versionCodeMajor", resourceID: nil, value: .int(1))
        #expect(Self.refusal(Self.manifest([.android("versionCode", 0x0101_021B, .int(5)), major], []))
            == "Not a valid APK: its manifest's versionCodeMajor reads as 1 and as 0")
        // Where obfuscation blanked the name, only the resource ID finds it, and that reading stands.
        let blanked = A(namespace: AXMLBuilder.androidNamespace, name: "", resourceID: 0x0101_021B, value: .int(7))
        #expect(try Self.parse(Self.manifest([blanked], [])).versionCode == 7)
        // TYPE_NULL and @null are 0 to both; every type from 0x10 to 0x1F is an integer, a color too.
        for (value, code) in [(AXMLBuilder.Value.typed(type: 0x00, data: 0), 0), (.reference(0), 0), (.typed(type: 0x1F, data: 3), 3)] {
            #expect(try Self.parse(Self.manifest([.android("versionCode", 0x0101_021B, value)], [])).versionCode == code)
        }
        // Android resolves a reference through the app's resources, which Minato does not read here [departure].
        for reference in [AXMLBuilder.Value.reference(0x7F0A_0001), .typed(type: 0x07, data: 0x7F0A_0001)] {
            #expect(Self.refusal(Self.manifest([.android("versionCode", 0x0101_021B, reference)], []))
                == "Not a valid APK: its manifest's versionCode names a resource, which Minato does not resolve")
        }
    }

    @Test func theSplitAttributesAreFoundByNameOnly() throws {
        // No package parser reads isFeatureSplit, isSplitRequired or the split types by resource ID
        // (PackageImpl.java:2720-2736), so where obfuscation renamed them, Android's installer finds none.
        let manifest = try Self.parse(Self.manifest([
            .android("fs", 0x0101_055B, .bool(true)), .android("sr", 0x0101_0591, .bool(true)),
            .android("rst", 0x0101_064E, .string("base__abi")), .android("st", 0x0101_064F, .string("feature__abi")),
        ], []))
        #expect(!manifest.isFeatureSplit)
        #expect(!manifest.splitRequired)
        #expect(manifest.requiredSplitTypes.isEmpty)
        #expect(manifest.splitTypes.isEmpty)
        #expect(!manifest.requiresSplits)
    }

    @Test func bundletoolsSignalTakesIntegerTypesOnly() throws {
        // bundletool's marker, which Android does not read: integer types only, as Android reads the <manifest> flag
        // (XmlBlock.java:484-492), so a string, even "TRUE", is no flag.
        func signal(_ value: AXMLBuilder.Value) throws -> Bool {
            try Self.parse(Self.manifest([], [E("application", [.android("isSplitRequired", 0x0101_0591, value)])]))
                .stricterSplitSignal
        }
        #expect(!(try signal(.string("TRUE"))))
        #expect(try signal(.int(1)))
        #expect(try signal(.bool(true)))
        #expect(!(try signal(.bool(false))))
    }

    @Test func aFileNameIsCheckedPerCharacterAsJavaDoes() {
        // FileUtils.isValidExtFilename checks each UTF-16 unit, so a combining mark does not hide '/' or NUL.
        #expect(!InstallerReading.isValidExtFilename("a/\u{301}b"))
        #expect(!InstallerReading.isValidExtFilename("a\u{0}\u{301}b"))
        #expect(InstallerReading.isValidExtFilename("a\u{301}b"))
    }

    @Test func theManifestsIsSplitRequiredNeedsSplits() throws {
        // Android 15 reads <manifest android:isSplitRequired>; bundletool's <application android:isSplitRequired>
        // and meta-data stay Minato's stricter signals.
        let manifest = try Self.parse(Self.manifest([.android("isSplitRequired", 0x0101_0591, .bool(true))], []))
        #expect(manifest.splitRequired)
        #expect(!manifest.stricterSplitSignal)
        #expect(manifest.requiresSplits)
    }

    @Test func requiredSplitTypesAsAndroidSplitsThem() throws {
        // separateAndValidateSplitTypes (:757-773): "," holds no type, and each type is checked as a name.
        #expect(!(try Self.parse(Self.manifest([.android("requiredSplitTypes", 0x0101_064E, .string(","))], [])).requiresSplits))
        #expect(Self.refusal(Self.manifest([.android("requiredSplitTypes", 0x0101_064E, .string("a/b"))], []))
            == "Invalid manifest split types: bad character '/'")
    }

    @Test func aFloatLevelReadsAsJavaWritesIt() throws {
        // coerceToString writes a float with Float.toString, NaN and the infinities as Java spells them.
        for (bits, text) in [(Float.nan.bitPattern, "NaN"), (Float.infinity.bitPattern, "Infinity"), ((-Float.infinity).bitPattern, "-Infinity")] {
            let manifest = try Self.usesSdk([.android("targetSdkVersion", Self.targetID, .typed(type: 0x04, data: bits))])
            #expect(manifest.sdkRefusal == Self.codename(text))
        }
    }
}
