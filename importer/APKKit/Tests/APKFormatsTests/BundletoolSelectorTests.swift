import APKFormats
import Foundation
import Testing

/// The choice from bundletool's table of contents, as `extract-apks` makes it for one device spec at 1.18.3
/// (`device/VariantMatcher.java`, `device/ApkMatcher.java`), with ADR-0049 decision 2's departures: on-demand feature
/// modules install (the M5 plan's decision 4), every language APK stays (decision 1), and the packs Play delivers
/// later are named for `packs/`. `tools/formats-check.sh` compares it with `extract-apks` on real output.
@Suite("bundletool's choice")
struct BundletoolSelectorTests {
    typealias Toc = BundletoolToc

    static let iPhone = BundletoolDevice(DeviceProfile.iPhone(nativeScale: 3, languages: ["de-DE", "en-US"]))

    static func apk(_ path: String, _ targeting: (inout Toc.ApkTargeting) -> Void = { _ in }) -> Toc.ApkDescription {
        var description = Toc.ApkDescription(path: path, kind: .split(id: "", isMaster: true))
        targeting(&description.targeting)
        return description
    }

    static func module(_ name: String, delivery: Int32 = Toc.DeliveryType.installTime, dependencies: [String] = [],
                       _ apks: [Toc.ApkDescription], _ targeting: (inout Toc.ModuleMetadata) -> Void = { _ in }) -> Toc.ApkSet {
        var module = Toc.ModuleMetadata()
        module.name = name
        module.deliveryType = delivery
        module.dependencies = dependencies
        module.moduleType = 1
        targeting(&module)
        return Toc.ApkSet(module: module, apks: apks)
    }

    static func variant(sdk: Int32?, alternatives: [Int32?] = [], runtime: Bool = false, _ sets: [Toc.ApkSet]) -> Toc.Variant {
        var targeting = Toc.VariantTargeting()
        targeting.sdk = .init(value: [sdk], alternatives: alternatives)
        targeting.requiresSdkRuntime = runtime
        return Toc.Variant(targeting: targeting, apkSets: sets)
    }

    static func toc(_ variants: [Toc.Variant], version: String = "1.18.3", _ more: (inout Toc) -> Void = { _ in }) -> Toc {
        var toc = Toc()
        toc.packageName = "org.example.sets"
        toc.bundletoolVersion = version
        toc.variants = variants
        more(&toc)
        return toc
    }

    static func paths(_ toc: Toc, on device: BundletoolDevice = iPhone) -> [String] {
        let choice = BundletoolSelector.choose(toc, for: device)
        return choice.incompatibility.map { ["incompatible: \($0)"] } ?? choice.apks.map(\.path)
    }

    /// One module whose APKs target `values` with the others as alternatives, as bundletool writes a dimension.
    static func dimension<T>(_ named: [(String, T)], fallback: String? = nil,
                             _ set: (inout Toc.ApkTargeting, Toc.Values<T>) -> Void) -> [Toc.ApkDescription] {
        var apks = named.map { name, value in
            apk(name) { set(&$0, .init(value: [value], alternatives: named.map(\.1).filter { "\($0)" != "\(value)" })) }
        }
        if let fallback { apks.append(apk(fallback) { set(&$0, .init(value: [], alternatives: named.map(\.1))) }) }
        return apks
    }

    // MARK: - Variants (VariantMatcher)

    @Test func android15TakesTheSplitVariantOverTheStandaloneOne() {
        let abis = Self.dimension([("splits/base-arm64_v8a.apk", Int32(3)), ("splits/base-armeabi_v7a.apk", 2)]) { $0.abi = $1 }
        let toc = Self.toc([
            Self.variant(sdk: 15, alternatives: [21], [Self.module("base", [Self.apk("standalones/standalone.apk")])]),
            Self.variant(sdk: 21, alternatives: [15], [Self.module("base", [Self.apk("splits/base-master.apk")] + abis)]),
        ])
        #expect(Self.paths(toc) == ["splits/base-master.apk", "splits/base-arm64_v8a.apk"])
    }

    @Test func aVariantForTheSdkRuntimeGoesToADeviceWithOne() {
        // VariantMatcher.java:76-100: a device with the SDK runtime tries its variants first; Minato has none.
        let toc = Self.toc([Self.variant(sdk: 34, runtime: true, [Self.module("base", [Self.apk("a.apk")])]),
                            Self.variant(sdk: 21, [Self.module("base", [Self.apk("b.apk")])])])
        #expect(Self.paths(toc) == ["b.apk"])
        var withRuntime = Self.iPhone
        withRuntime.supportsSdkRuntime = true
        #expect(Self.paths(toc, on: withRuntime) == ["a.apk"])
    }

    @Test func anInstantVariantIsNoChoice() {
        var instant = Self.apk("instant/base-master.apk")
        instant.kind = .instant(id: "", isMaster: true)
        let toc = Self.toc([Self.variant(sdk: 21, [Self.module("base", [instant])]),
                            Self.variant(sdk: 21, [Self.module("base", [Self.apk("splits/base-master.apk")])])])
        #expect(Self.paths(toc) == ["splits/base-master.apk"])
    }

    @Test func aMultiABIVariantGoesByTheBestSetTheDeviceRuns() {
        // MultiAbiMatcher with MULTI_ABI_ALIAS_COMPARATOR: a set that holds another ranks above it.
        func variant(_ value: [Int32], _ alternative: [Int32], _ path: String) -> Toc.Variant {
            var targeting = Toc.VariantTargeting()
            targeting.multiAbi = .init(value: [value], alternatives: [alternative])
            return Toc.Variant(targeting: targeting, apkSets: [Self.module("base", [Self.apk(path)])])
        }
        let toc = Self.toc([variant([3], [3, 2], "arm64.apk"), variant([3, 2], [3], "arm64-v7a.apk")])
        #expect(Self.paths(toc) == ["arm64.apk"])
        let both = BundletoolDevice(DeviceProfile(abis: ["arm64-v8a", "armeabi-v7a"], densityDpi: 480, languages: [],
                                                  textureFormats: ["astc"]))
        #expect(Self.paths(toc, on: both) == ["arm64-v7a.apk"])
        // x86_64 ranks above arm64-v8a, which the device does not run.
        #expect(Self.paths(Self.toc([variant([3], [5], "arm64.apk")])) == ["arm64.apk"])
        // By the best ABI first: arm64-v8a ranks above armeabi-v7a.
        let order = Self.toc([variant([2], [3], "v7a.apk"), variant([3], [2], "arm64-only.apk")])
        #expect(Self.paths(order, on: both) == ["arm64-only.apk"])
    }

    @Test func anSDKMinimumOfTheDevicesOwnFitsAndAnEqualAlternativeIsNoBetter() {
        // SdkVersionMatcher.java: a minimum the device runs fits, 35 included; an alternative wins only with a higher one.
        #expect(Self.paths(Self.toc([Self.variant(sdk: 35, [Self.module("base", [Self.apk("a.apk")])])])) == ["a.apk"])
        #expect(Self.paths(Self.toc([Self.variant(sdk: 21, alternatives: [21], [Self.module("base", [Self.apk("a.apk")])])]))
            == ["a.apk"])
    }

    @Test func noVariantOrTwoMeanNoChoice() {
        // SdkVersionMatcher.java:118-133 refuses a device below every variant; getMatchingVariant takes at most one.
        #expect(Self.paths(Self.toc([Self.variant(sdk: 36, [Self.module("base", [Self.apk("a.apk")])])]))
            == ["incompatible: It needs Android SDK 36 or later, and this iPhone runs SDK 35"])
        let two = Self.toc([Self.variant(sdk: 21, [Self.module("base", [Self.apk("a.apk")])]),
                            Self.variant(sdk: 24, [Self.module("base", [Self.apk("b.apk")])])])
        #expect(Self.paths(two) == ["incompatible: More than one of its variants fits this iPhone"])
        #expect(Self.paths(Self.toc([])) == ["incompatible: It has no APK for this iPhone"])
    }

    // MARK: - Modules (ApkMatcher, ModuleMatcher)

    @Test func installTimeModulesAndOnDemandFeatureModulesWithTheirDependencies() {
        // An on-demand module is requested, as `--modules` would, with its dependencies; an install-time module that
        // does not fit stays out unless one of them depends on it. An SDK module stays out.
        let toc = Self.toc([Self.variant(sdk: 21, [
            Self.module("base", [Self.apk("base.apk")]),
            Self.module("camera", delivery: Toc.DeliveryType.onDemand, dependencies: ["ar"], [Self.apk("camera.apk")]),
            Self.module("ar", [Self.apk("ar.apk")]) { $0.targeting.deviceFeatures = [.init(name: "android.hardware.camera.ar", version: 0)] },
            Self.module("vr", [Self.apk("vr.apk")]) { $0.targeting.deviceFeatures = [.init(name: "android.hardware.vr.high_performance", version: 0)] },
            Self.module("ads", delivery: Toc.DeliveryType.onDemand, [Self.apk("ads.apk")]) { $0.moduleType = 3 },
        ])])
        var device = Self.iPhone
        device.features = ["reqGlEsVersion=0x30000"]
        #expect(Self.paths(toc, on: device) == ["base.apk", "camera.apk", "ar.apk"])
    }

    @Test func aConditionalModuleFitsByTheDevicesFeatures() {
        // Without features in the spec every condition fits (TargetingDimensionMatcher.java's module predicate).
        let toc = Self.toc([Self.variant(sdk: 21, [
            Self.module("base", [Self.apk("base.apk")]),
            Self.module("ar", [Self.apk("ar.apk")]) { $0.targeting.deviceFeatures = [.init(name: "android.hardware.camera.ar", version: 0)] },
            Self.module("gl32", [Self.apk("gl32.apk")]) {
                $0.targeting.deviceFeatures = [.init(name: "android.hardware.opengles.version", version: 0x30002)]
            },
            Self.module("new", [Self.apk("new.apk")]) { $0.targeting.sdk = .init(value: [36]) },
            Self.module("group", [Self.apk("group.apk")]) { $0.targeting.deviceGroups = ["highend"] },
        ])])
        var device = Self.iPhone
        device.features = []
        #expect(Self.paths(toc, on: device) == ["base.apk", "ar.apk", "gl32.apk"])
        device.features = ["reqGlEsVersion=0x30000", "android.hardware.camera.ar"]
        #expect(Self.paths(toc, on: device) == ["base.apk", "ar.apk"])
        device.features = ["reqGlEsVersion=0x30002"]
        device.deviceGroups = ["highend"]
        #expect(Self.paths(toc, on: device) == ["base.apk", "gl32.apk", "group.apk"])
    }

    @Test func minatosSpecTakesAModuleWhereItsPackageManagerHasTheFeature() {
        // ADR-0049 decision 2's follow-up: the spec lists Minato's system features by name, so a condition holds
        // where hasSystemFeature does, a versioned feature's too; OpenGL ES is 3.1 on the iPhone, ANGLE's Vulkan
        // path (ADR-0044 d)), and 3.0 without Vulkan's features in the Simulator, which has no MoltenVK.
        func module(_ name: String, _ feature: String, version: Int32 = 0) -> Toc.ApkSet {
            Self.module(name, [Self.apk("\(name).apk")]) {
                $0.targeting.deviceFeatures = [.init(name: feature, version: version)]
            }
        }
        let toc = Self.toc([Self.variant(sdk: 21, [
            Self.module("base", [Self.apk("base.apk")]),
            module("ar", "android.hardware.camera.ar"),
            module("touch", "android.hardware.touchscreen.multitouch.jazzhand"),
            module("gl31", "android.hardware.opengles.version", version: 0x30001),
            module("gl32", "android.hardware.opengles.version", version: 0x30002),
            module("vulkan", "android.hardware.vulkan.version"),
        ])])
        let iPhone = BundletoolDevice(DeviceProfile.iPhone(nativeScale: 3, languages: ["en-US"], vulkan: true))
        #expect(Self.paths(toc, on: iPhone) == ["base.apk", "touch.apk", "gl31.apk", "vulkan.apk"])
        let simulator = BundletoolDevice(DeviceProfile.iPhone(nativeScale: 3, languages: ["en-US"], vulkan: false))
        #expect(Self.paths(toc, on: simulator) == ["base.apk", "touch.apk"])
    }

    @Test func aBundletoolBefore0_10_2NamesOnDemandModulesByItsOldFlag() {
        // ApkMatcher.java:272-279 with VersionGuardedFeature.NEW_DELIVERY_TYPE_MANIFEST_TAG ("0.10.2"); an empty
        // version is bundletool's own (ResultUtils.java:91-97).
        func toc(_ version: String) -> Toc {
            Self.toc([Self.variant(sdk: 21, [
                Self.module("base", [Self.apk("base.apk")]),
                Self.module("old", [Self.apk("old.apk")]) {
                    $0.onDemandDeprecated = true
                    $0.targeting.deviceGroups = ["nobody"]
                },
            ])], version: version)
        }
        #expect(Self.paths(toc("0.10.1")) == ["base.apk", "old.apk"])
        #expect(Self.paths(toc("0.10.2")) == ["base.apk"])
        #expect(Self.paths(toc("")) == ["base.apk"])
    }

    // MARK: - APKs (ApkMatcher's dimensions)

    @Test func eachDimensionTakesWhatExtractApksTakes() {
        let abi = Self.dimension([("arm64.apk", Int32(3)), ("v7a.apk", 2), ("x86.apk", 4)]) { $0.abi = $1 }
        let density = Self.dimension([("xhdpi.apk", Toc.Density.alias(6)), ("xxhdpi.apk", .alias(7)), ("xxxhdpi.apk", .dpi(640))]) {
            $0.density = $1
        }
        let texture = Self.dimension([("astc.apk", Int32(9)), ("etc2.apk", 10)], fallback: "other_tcf.apk") { $0.textureFormat = $1 }
        let toc = Self.toc([Self.variant(sdk: 21, [Self.module("base", [Self.apk("master.apk")] + abi + density + texture)])])
        #expect(Self.paths(toc) == ["master.apk", "arm64.apk", "xxhdpi.apk", "astc.apk"])

        // Near the lower density, by Android's rule (ScreenDensitySelector); without ASTC, ETC2. A device with
        // neither fails the ASTC APK's check before its fallback counts (TextureCompressionFormatMatcher.java:
        // 116-140, run over every APK of the module), so extract-apks takes none.
        var device = BundletoolDevice(DeviceProfile(abis: ["arm64-v8a"], densityDpi: 330, languages: [], textureFormats: ["etc2"]))
        #expect(Self.paths(toc, on: device) == ["master.apk", "arm64.apk", "xhdpi.apk", "etc2.apk"])
        device.profile.textureFormats = ["pvrtc"]
        #expect(Self.paths(toc, on: device) == ["incompatible: No texture format this iPhone decodes"])
    }

    @Test func aModuleWhoseBestDensityHasNoAPKIsIncompatible() {
        // ExtractApksCommand.java:213-218 asks for every density and ABI dimension of a module
        // (ensureDensityAndAbiApksMatched): here the best density is only an alternative.
        let toc = Self.toc([Self.variant(sdk: 21, [Self.module("base", [
            Self.apk("master.apk"), Self.apk("xhdpi.apk") { $0.density = .init(value: [.alias(6)], alternatives: [.alias(7)]) },
        ])])])
        #expect(Self.paths(toc) == ["incompatible: No screen density APK of the module “base” fits this iPhone"])
    }

    @Test func aFallbackAPKAloneIsNotCheckedAgainstTheDevice() {
        // TextureCompressionFormatMatcher.java: a fallback, without values, is exempt from the device's check.
        let toc = Self.toc([Self.variant(sdk: 21, [Self.module("base", [
            Self.apk("master.apk"), Self.apk("other_tcf.apk") { $0.textureFormat = .init(value: [], alternatives: [8]) },
        ])])])
        #expect(Self.paths(toc) == ["master.apk", "other_tcf.apk"])
    }

    @Test func anAssetPacksSlicesFallBackWithoutTheModulesCheck() {
        // ApkMatcher matches asset slices without checking them against the device, so a fallback slice fits where the
        // device has none of the others' ABIs or texture formats, and the others do not.
        let abis = Self.dimension([("x86.apk", Int32(4)), ("x86_64.apk", 5)], fallback: "other_abis.apk") { $0.abi = $1 }
        let textures = Self.dimension([("pvrtc.apk", Int32(8)), ("s3tc.apk", 7)], fallback: "other_tcf.apk") {
            $0.textureFormat = $1
        }
        let toc = Self.toc([Self.variant(sdk: 21, [Self.module("base", [Self.apk("base.apk")])])]) {
            $0.assetSliceSets = [Toc.AssetSliceSet(module: .init(name: "start", deliveryType: Toc.DeliveryType.installTime),
                                                   apks: abis + textures)]
        }
        #expect(Self.paths(toc) == ["base.apk", "other_abis.apk", "other_tcf.apk"])
    }

    @Test func everyLanguageAPKStays() {
        // The M5 plan's decision 1, where LanguageMatcher takes the device's languages and a fallback only when
        // the others miss one of them.
        let languages = Self.dimension([("de.apk", "de"), ("fr.apk", "fr"), ("ja.apk", "ja")], fallback: "other_lang.apk") {
            $0.language = $1
        }
        let toc = Self.toc([Self.variant(sdk: 21, [Self.module("base", [Self.apk("master.apk")] + languages)])])
        #expect(Self.paths(toc) == ["master.apk", "de.apk", "fr.apk", "ja.apk", "other_lang.apk"])
    }

    @Test func tierCountrySetAndGroupFollowTheTocsDefaults() {
        // ExtractApksCommand.java:374-413: a spec without them takes the toc's defaults, else tier 0, no country
        // set and the group "".
        let tiers = Self.dimension([("tier_0.apk", Int32(0)), ("tier_1.apk", 1), ("tier_2.apk", 2)]) { $0.deviceTier = $1 }
        let countries = Self.dimension([("latam.apk", "latam"), ("sea.apk", "sea")], fallback: "other_countries.apk") {
            $0.countrySet = $1
        }
        let groups = [Self.apk("highend.apk") { $0.deviceGroup = .init(value: ["highend"], alternatives: ["lowend"]) },
                      Self.apk("lowend.apk") { $0.deviceGroup = .init(value: ["lowend"], alternatives: ["highend"]) }]
        let sets = [Self.module("base", [Self.apk("master.apk")] + tiers + countries + groups)]
        #expect(Self.paths(Self.toc([Self.variant(sdk: 21, sets)]))
            == ["master.apk", "tier_0.apk", "other_countries.apk"])
        let defaults = Self.toc([Self.variant(sdk: 21, sets)]) {
            $0.defaultTargeting = [.init(dimension: Toc.SplitDimension.deviceTier, value: "2"),
                                   .init(dimension: Toc.SplitDimension.countrySet, value: "sea"),
                                   .init(dimension: Toc.SplitDimension.deviceGroup, value: "highend")]
        }
        #expect(Self.paths(defaults) == ["master.apk", "tier_2.apk", "sea.apk", "highend.apk"])
        var device = Self.iPhone
        device.deviceTier = 1
        device.countrySet = "latam"
        device.deviceGroups = ["lowend"]
        #expect(Self.paths(defaults, on: device) == ["master.apk", "tier_1.apk", "latam.apk", "lowend.apk"])
    }

    @Test(arguments: [
        ("abi", "No arm64-v8a APK"),
        ("texture", "No texture format this iPhone decodes"),
        ("tier", "No device tier APK fits this iPhone"),
        ("country", "No country set APK fits this iPhone"),
    ])
    func aDimensionWithoutTheDevicesValueIsIncompatible(_ dimension: String, _ reason: String) {
        // Each matcher's checkDeviceCompatible, which extract-apks runs over every APK of a chosen module.
        let apks: [Toc.ApkDescription]
        switch dimension {
        case "abi": apks = Self.dimension([("x86.apk", Int32(4)), ("x86_64.apk", 5)]) { $0.abi = $1 }
        case "texture": apks = Self.dimension([("pvrtc.apk", Int32(8)), ("s3tc.apk", 7)]) { $0.textureFormat = $1 }
        case "tier": apks = Self.dimension([("tier_1.apk", Int32(1)), ("tier_2.apk", 2)]) { $0.deviceTier = $1 }
        default: apks = Self.dimension([("latam.apk", "latam"), ("sea.apk", "sea")]) { $0.countrySet = $1 }
        }
        var device = Self.iPhone
        if dimension == "country" { device.countrySet = "europe" }
        let toc = Self.toc([Self.variant(sdk: 21, [Self.module("base", [Self.apk("master.apk")] + apks)])])
        #expect(Self.paths(toc, on: device) == ["incompatible: \(reason)"])
    }

    // MARK: - Asset packs

    @Test func installTimePacksInstallAndTheOthersWaitInPacks() {
        // ApkMatcher.java's upfront asset modules; fast-follow and on-demand ones go to packs/ (format 6).
        func pack(_ name: String, _ delivery: Int32) -> Toc.AssetSliceSet {
            var master = Self.apk("asset-slices/\(name)-master.apk")
            master.kind = .assetSlice(id: "", isMaster: true)
            let slices = Self.dimension([("asset-slices/\(name)-astc.apk", Int32(9)), ("asset-slices/\(name)-etc2.apk", 10)]) {
                $0.textureFormat = $1
            }
            return Toc.AssetSliceSet(module: .init(name: name, deliveryType: delivery), apks: [master] + slices)
        }
        let toc = Self.toc([Self.variant(sdk: 21, [Self.module("base", [Self.apk("base.apk")])])]) {
            $0.assetSliceSets = [pack("start", Toc.DeliveryType.installTime), pack("level2", Toc.DeliveryType.fastFollow),
                                 pack("level3", Toc.DeliveryType.onDemand)]
        }
        let choice = BundletoolSelector.choose(toc, for: Self.iPhone)
        #expect(choice.apks.map(\.path) == ["base.apk", "asset-slices/start-master.apk", "asset-slices/start-astc.apk"])
        #expect(choice.apks.map(\.module) == ["base", "start", "start"])
        #expect(choice.packs == [
            .init(name: "level2", deliveryType: Toc.DeliveryType.fastFollow,
                  paths: ["asset-slices/level2-master.apk", "asset-slices/level2-astc.apk"]),
            .init(name: "level3", deliveryType: Toc.DeliveryType.onDemand,
                  paths: ["asset-slices/level3-master.apk", "asset-slices/level3-astc.apk"]),
        ])
    }

    @Test func aUniversalTocGivesItsOneAPK() {
        // build-apks --mode=universal: one standalone variant without targeting.
        var universal = Self.apk("universal.apk")
        universal.kind = .standalone(id: "")
        let toc = Self.toc([Toc.Variant(apkSets: [Self.module("base", [universal])])])
        #expect(Self.paths(toc) == ["universal.apk"])
    }
}
