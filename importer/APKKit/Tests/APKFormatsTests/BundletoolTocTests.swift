import APKFormats
import Foundation
import Testing

/// bundletool's table of contents as `toc.pb` and as `toc.json` (ADR-0049 decision 2), against the tests' own
/// writer; `tools/formats-check.sh` reads real ones.
@Suite("bundletool's table of contents")
struct BundletoolTocTests {
    typealias Toc = BundletoolToc

    /// Every field the choice reads, each with a value that is not its default.
    static var sample: Toc {
        var toc = Toc()
        toc.packageName = "org.example.sets"
        toc.bundletoolVersion = "1.18.3"
        var variant = Toc.VariantTargeting()
        variant.sdk = .init(value: [21], alternatives: [nil, 31])
        variant.abi = .init(value: [3], alternatives: [2])
        variant.multiAbi = .init(value: [[3, 2]], alternatives: [[3]])
        variant.density = .init(value: [.alias(7)], alternatives: [.dpi(560)])
        variant.textureFormat = .init(value: [9], alternatives: [10])
        variant.requiresSdkRuntime = true
        var base = Toc.ModuleMetadata()
        base.name = "base"
        base.deliveryType = Toc.DeliveryType.installTime
        base.moduleType = 1
        var camera = Toc.ModuleMetadata()
        camera.name = "camera"
        camera.deliveryType = Toc.DeliveryType.onDemand
        camera.onDemandDeprecated = true
        camera.isInstant = true
        camera.dependencies = ["base", "core"]
        camera.targeting.sdk = .init(value: [26])
        camera.targeting.deviceFeatures = [.init(name: "android.hardware.camera.ar", version: 0),
                                           .init(name: "android.hardware.opengles.version", version: 0x30002)]
        camera.targeting.deviceGroups = ["highend"]
        var targeting = Toc.ApkTargeting()
        targeting.abi = .init(value: [3], alternatives: [2, 5])
        targeting.language = .init(value: ["de"], alternatives: ["fr"])
        targeting.density = .init(value: [.dpi(480)], alternatives: [.alias(6)])
        targeting.sdk = .init(value: [24])
        targeting.textureFormat = .init(value: [9])
        targeting.multiAbi = .init(value: [[3]], alternatives: [[2]])
        targeting.sanitizer = [1]
        targeting.deviceTier = .init(value: [1], alternatives: [0, 2])
        targeting.countrySet = .init(value: ["latam"], alternatives: ["sea"])
        targeting.deviceGroup = .init(value: ["highend"], alternatives: ["lowend"])
        let apks: [Toc.ApkDescription] = [
            .init(path: "splits/base-master.apk", kind: .split(id: "", isMaster: true)),
            .init(targeting: targeting, path: "splits/base-arm64_v8a.apk", kind: .split(id: "config.arm64_v8a", isMaster: false)),
            .init(path: "standalones/standalone-arm64_v8a.apk", kind: .standalone(id: "")),
            .init(path: "instant/instant-base-master.apk", kind: .instant(id: "", isMaster: true)),
            .init(path: "system/system.apk", kind: .system),
            .init(path: "asset-slices/x.apk", kind: .assetSlice(id: "x", isMaster: false)),
            .init(path: "apex/x.apex", kind: .apex),
            .init(path: "archive/archive.apk", kind: .archived),
            .init(path: "odd.apk", kind: .none),
        ]
        toc.variants = [Toc.Variant(targeting: variant, apkSets: [Toc.ApkSet(module: base, apks: apks),
                                                                 Toc.ApkSet(module: camera)], number: 2)]
        var slice = Toc.ApkTargeting()
        slice.textureFormat = .init(value: [9], alternatives: [10])
        toc.assetSliceSets = [Toc.AssetSliceSet(module: .init(name: "textures", deliveryType: Toc.DeliveryType.fastFollow),
                                                apks: [.init(path: "asset-slices/textures-master.apk",
                                                             kind: .assetSlice(id: "", isMaster: true)),
                                                       .init(targeting: slice, path: "asset-slices/textures-astc.apk",
                                                             kind: .assetSlice(id: "config.astc", isMaster: false))])]
        toc.defaultTargeting = [.init(dimension: Toc.SplitDimension.deviceTier, value: "1"),
                                .init(dimension: Toc.SplitDimension.countrySet, value: "latam")]
        return toc
    }

    // MARK: - toc.pb

    @Test func aTocReadsBackAsWritten() throws {
        #expect(try Toc(proto: TocWriter.toc(Self.sample)) == Self.sample)
        #expect(try Toc(proto: Data()) == Toc())
    }

    @Test func unknownFieldsAndKnownNumbersOfAnotherWireTypeAreSkipped() throws {
        // As protobuf's Java parser does: an unknown field's bytes are never read, so they need not be a message.
        let apk = Toc.ApkDescription(path: "splits/base-master.apk", kind: .split(id: "", isMaster: true))
        let description = TocWriter.apk(apk) + TocWriter.field(50, bytes: Data([0xFF, 0xFF])) + TocWriter.field(51, varint: 9)
        let data = TocWriter.field(99, varint: 5) + TocWriter.field(4, varint: 7) + TocWriter.field(4, string: "org.example")
            + TocWriter.field(1, bytes: TocWriter.field(2, bytes: TocWriter.field(2, bytes: description)))
            + TocWriter.field(100, bytes: Data([0x0B]))

        let toc = try Toc(proto: data)
        #expect(toc.packageName == "org.example")
        #expect(toc.variants == [Toc.Variant(apkSets: [Toc.ApkSet(apks: [apk])])])
    }

    @Test func aDamagedTocIsRefused() throws {
        let data = TocWriter.toc(Self.sample)
        #expect { try Toc(proto: data.dropLast()) } throws: { $0 is ProtoError }
        // A string that is no UTF-8, as protobuf's proto3 readers refuse one.
        #expect(throws: ProtoError.invalid("a string that is no UTF-8")) {
            try Toc(proto: TocWriter.field(4, bytes: Data([0xC3, 0x28])))
        }
    }

    // MARK: - toc.json

    /// The sample as protobuf's JsonFormat writes it, field names in lowerCamelCase and enums by name.
    static let sampleJSON = """
    {
      "packageName": "org.example.sets",
      "bundletool": {"version": "1.18.3"},
      "variant": [{
        "targeting": {
          "sdkVersionTargeting": {"value": [{"min": 21}], "alternatives": [{}, {"min": 31}]},
          "abiTargeting": {"value": [{"alias": "ARM64_V8A"}], "alternatives": [{"alias": "ARMEABI_V7A"}]},
          "multiAbiTargeting": {"value": [{"abi": [{"alias": "ARM64_V8A"}, {"alias": "ARMEABI_V7A"}]}],
                                "alternatives": [{"abi": [{"alias": "ARM64_V8A"}]}]},
          "screenDensityTargeting": {"value": [{"densityAlias": "XXHDPI"}], "alternatives": [{"densityDpi": 560}]},
          "textureCompressionFormatTargeting": {"value": [{"alias": "ASTC"}], "alternatives": [{"alias": "ETC2"}]},
          "sdkRuntimeTargeting": {"requiresSdkRuntime": true}
        },
        "apkSet": [{
          "moduleMetadata": {"name": "base", "deliveryType": "INSTALL_TIME", "moduleType": "FEATURE_MODULE"},
          "apkDescription": [
            {"path": "splits/base-master.apk", "splitApkMetadata": {"isMasterSplit": true}},
            {"targeting": {
               "abiTargeting": {"value": [{"alias": "ARM64_V8A"}], "alternatives": [{"alias": "ARMEABI_V7A"}, {"alias": "X86_64"}]},
               "languageTargeting": {"value": ["de"], "alternatives": ["fr"]},
               "screenDensityTargeting": {"value": [{"densityDpi": 480}], "alternatives": [{"densityAlias": "XHDPI"}]},
               "sdkVersionTargeting": {"value": [{"min": 24}]},
               "textureCompressionFormatTargeting": {"value": [{"alias": "ASTC"}]},
               "multiAbiTargeting": {"value": [{"abi": [{"alias": "ARM64_V8A"}]}], "alternatives": [{"abi": [{"alias": "ARMEABI_V7A"}]}]},
               "sanitizerTargeting": {"value": [{"alias": "HWADDRESS"}]},
               "deviceTierTargeting": {"value": [1], "alternatives": [0, 2]},
               "countrySetTargeting": {"value": ["latam"], "alternatives": ["sea"]},
               "deviceGroupTargeting": {"value": ["highend"], "alternatives": ["lowend"]}
             },
             "path": "splits/base-arm64_v8a.apk", "splitApkMetadata": {"splitId": "config.arm64_v8a"}},
            {"path": "standalones/standalone-arm64_v8a.apk", "standaloneApkMetadata": {"fusedModuleName": ["base"]}},
            {"path": "instant/instant-base-master.apk", "instantApkMetadata": {"isMasterSplit": true}},
            {"path": "system/system.apk", "systemApkMetadata": {"fusedModuleName": ["base"]}},
            {"path": "asset-slices/x.apk", "assetSliceMetadata": {"splitId": "x"}},
            {"path": "apex/x.apex", "apexApkMetadata": {}},
            {"path": "archive/archive.apk", "archivedApkMetadata": {}},
            {"path": "odd.apk"}
          ]
        }, {
          "moduleMetadata": {
            "name": "camera", "deliveryType": "ON_DEMAND", "onDemandDeprecated": true, "isInstant": true,
            "dependencies": ["base", "core"],
            "targeting": {
              "sdkVersionTargeting": {"value": [{"min": 26}]},
              "deviceFeatureTargeting": [{"requiredFeature": {"featureName": "android.hardware.camera.ar"}},
                                         {"requiredFeature": {"featureName": "android.hardware.opengles.version",
                                                              "featureVersion": 196610}}],
              "deviceGroupTargeting": {"value": ["highend"]}
            }
          }
        }],
        "variantNumber": 2
      }],
      "assetSliceSet": [{
        "assetModuleMetadata": {"name": "textures", "deliveryType": "FAST_FOLLOW"},
        "apkDescription": [
          {"path": "asset-slices/textures-master.apk", "assetSliceMetadata": {"isMasterSplit": true}},
          {"targeting": {"textureCompressionFormatTargeting": {"value": [{"alias": "ASTC"}], "alternatives": [{"alias": "ETC2"}]}},
           "path": "asset-slices/textures-astc.apk", "assetSliceMetadata": {"splitId": "config.astc"}}
        ]
      }],
      "defaultTargetingValue": [{"dimension": "DEVICE_TIER", "defaultValue": "1"},
                                {"dimension": "COUNTRY_SET", "defaultValue": "latam"}]
    }
    """

    @Test func aTocJsonReadsAsItsProtoDoes() throws {
        #expect(try Toc(json: Data(Self.sampleJSON.utf8)) == Self.sample)
    }

    @Test func aTocJsonTakesProtoNamesNumbersAndUnknownFieldsAsJsonFormatDoes() throws {
        // JsonFormat takes a field by its proto name too, an enum or a number as a number or a string, and skips
        // what it does not know (`ResultUtils.java:79-84`, ignoringUnknownFields); null is the default.
        let json = """
        {"package_name": "org.example", "unknownField": {"x": [1, 2]}, "bundletool": null,
         "variant": [{"variant_number": "3", "apk_set": [{"module_metadata": {"name": "base", "delivery_type": 1,
           "is_instant": "true"}, "apk_description": [{"path": "a.apk", "split_apk_metadata": {"split_id": "config.de"},
           "targeting": {"abi_targeting": {"value": [{"alias": 3}]}, "device_tier_targeting": {"value": ["2"]}}}]}]}]}
        """
        let toc = try Toc(json: Data(json.utf8))
        var module = Toc.ModuleMetadata()
        module.name = "base"
        module.deliveryType = 1
        module.isInstant = true
        var targeting = Toc.ApkTargeting()
        targeting.abi = .init(value: [3])
        targeting.deviceTier = .init(value: [2])
        #expect(toc.packageName == "org.example")
        #expect(toc.bundletoolVersion == "")
        #expect(toc.variants == [Toc.Variant(apkSets: [Toc.ApkSet(module: module, apks: [
            .init(targeting: targeting, path: "a.apk", kind: .split(id: "config.de", isMaster: false))])], number: 3)])
    }

    @Test(arguments: [
        ("[]", "toc.json is no JSON object"),
        ("{", "toc.json is no JSON object"),
        (#"{"packageName": 5}"#, "toc.json's packageName is no string"),
        (#"{"variant": {}}"#, "toc.json's variant is no list"),
        (#"{"variant": [{"variantNumber": -1}]}"#, "toc.json's variantNumber is no uint32"),
        (#"{"variant": [{"variantNumber": 4294967296}]}"#, "toc.json's variantNumber is no uint32"),
        (#"{"variant": [{"variantNumber": 1.5}]}"#, "toc.json's variantNumber is no uint32"),
        (#"{"variant": [{"apkSet": [{"moduleMetadata": {"deliveryType": "LATER"}}]}]}"#,
         "toc.json's deliveryType LATER is no value Minato knows"),
        (#"{"variant": [{"targeting": {"abiTargeting": {"value": [{"alias": "ARM128"}]}}}]}"#,
         "toc.json's alias ARM128 is no value Minato knows"),
    ] as [(String, String)])
    func aTocJsonOfAnotherShapeIsRefused(_ json: String, _ reason: String) {
        #expect(throws: ProtoError.invalid(reason)) { try Toc(json: Data(json.utf8)) }
    }
}
