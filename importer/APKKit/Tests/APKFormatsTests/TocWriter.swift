import APKFormats
import Foundation

/// protobuf's wire format for the tests' tables of contents: the model's fields under `commands.proto`'s and
/// `targeting.proto`'s numbers at bundletool 1.18.3, every field written, defaults too.
enum TocWriter {
    static func varint(_ value: UInt64) -> Data {
        var value = value, out = Data()
        repeat {
            out.append(UInt8(value & 0x7F) | (value >= 0x80 ? 0x80 : 0))
            value >>= 7
        } while value != 0
        return out
    }

    static func field(_ number: Int, varint value: UInt64) -> Data { varint(UInt64(number) << 3) + varint(value) }
    static func field(_ number: Int, int32 value: Int32) -> Data { field(number, varint: UInt64(bitPattern: Int64(value))) }
    static func field(_ number: Int, bool value: Bool) -> Data { field(number, varint: value ? 1 : 0) }
    static func field(_ number: Int, bytes: Data) -> Data { varint(UInt64(number) << 3 | 2) + varint(UInt64(bytes.count)) + bytes }
    static func field(_ number: Int, string: String) -> Data { field(number, bytes: Data(string.utf8)) }

    static func values<T>(_ values: BundletoolToc.Values<T>, valueField: Int = 1, alternativesField: Int = 2,
                          _ element: (T) -> Data) -> Data {
        values.value.reduce(Data()) { $0 + field(valueField, bytes: element($1)) }
            + values.alternatives.reduce(Data()) { $0 + field(alternativesField, bytes: element($1)) }
    }

    static func alias(_ value: Int32) -> Data { field(1, int32: value) }
    static func sdk(_ min: Int32?) -> Data { min.map { field(1, bytes: field(1, int32: $0)) } ?? Data() }
    static func density(_ density: BundletoolToc.Density) -> Data {
        switch density {
        case .alias(let alias): field(1, int32: alias)
        case .dpi(let dpi): field(2, int32: dpi)
        }
    }
    static func multiAbi(_ abis: [Int32]) -> Data { abis.reduce(Data()) { $0 + field(1, bytes: alias($1)) } }

    static func variantTargeting(_ targeting: BundletoolToc.VariantTargeting) -> Data {
        field(1, bytes: values(targeting.sdk, sdk)) + field(2, bytes: values(targeting.abi, alias))
            + field(3, bytes: values(targeting.density, density)) + field(4, bytes: values(targeting.multiAbi, multiAbi))
            + field(5, bytes: values(targeting.textureFormat, alias))
            + field(6, bytes: field(1, bool: targeting.requiresSdkRuntime))
    }

    static func apkTargeting(_ targeting: BundletoolToc.ApkTargeting) -> Data {
        field(1, bytes: values(targeting.abi, alias)) + field(3, bytes: values(targeting.language) { Data($0.utf8) })
            + field(4, bytes: values(targeting.density, density)) + field(5, bytes: values(targeting.sdk, sdk))
            + field(6, bytes: values(targeting.textureFormat, alias)) + field(7, bytes: values(targeting.multiAbi, multiAbi))
            + field(8, bytes: targeting.sanitizer.reduce(Data()) { $0 + field(1, bytes: alias($1)) })
            + field(9, bytes: values(targeting.deviceTier, valueField: 3, alternativesField: 4) { field(1, int32: $0) })
            + field(10, bytes: values(targeting.countrySet) { Data($0.utf8) })
            + field(11, bytes: values(targeting.deviceGroup) { Data($0.utf8) })
    }

    static func moduleTargeting(_ targeting: BundletoolToc.ModuleTargeting) -> Data {
        field(1, bytes: values(targeting.sdk, sdk))
            + targeting.deviceFeatures.reduce(Data()) {
                $0 + field(2, bytes: field(1, bytes: field(1, string: $1.name) + field(2, int32: $1.version)))
            }
            + field(5, bytes: targeting.deviceGroups.reduce(Data()) { $0 + field(1, string: $1) })
    }

    static func apk(_ apk: BundletoolToc.ApkDescription) -> Data {
        func split(_ id: String, _ master: Bool) -> Data { field(1, string: id) + field(2, bool: master) }
        let kind: Data
        switch apk.kind {
        case .split(let id, let master): kind = field(3, bytes: split(id, master))
        case .standalone(let id): kind = field(4, bytes: field(1, string: "base") + field(3, string: id))
        case .instant(let id, let master): kind = field(5, bytes: split(id, master))
        case .system: kind = field(6, bytes: field(1, string: "base"))
        case .assetSlice(let id, let master): kind = field(7, bytes: split(id, master))
        case .apex: kind = field(8, bytes: Data())
        case .archived: kind = field(9, bytes: Data())
        case .none: kind = Data()
        }
        return field(1, bytes: apkTargeting(apk.targeting)) + field(2, string: apk.path) + kind
    }

    static func module(_ module: BundletoolToc.ModuleMetadata) -> Data {
        field(1, string: module.name) + field(2, bool: module.onDemandDeprecated) + field(3, bool: module.isInstant)
            + module.dependencies.reduce(Data()) { $0 + field(4, string: $1) }
            + field(5, bytes: moduleTargeting(module.targeting)) + field(6, int32: module.deliveryType)
            + field(7, int32: module.moduleType)
    }

    /// The toc as protobuf's JsonFormat writes it, enums by their numbers, which it reads too.
    static func json(_ toc: BundletoolToc) -> String {
        func values<T>(_ values: BundletoolToc.Values<T>, _ element: (T) -> Any) -> [String: Any] {
            ["value": values.value.map(element), "alternatives": values.alternatives.map(element)]
        }
        func sdk(_ min: Int32?) -> Any { min.map { ["min": $0] } ?? [String: Any]() }
        func density(_ density: BundletoolToc.Density) -> Any {
            switch density {
            case .alias(let alias): ["densityAlias": alias]
            case .dpi(let dpi): ["densityDpi": dpi]
            }
        }
        func multiAbi(_ abis: [Int32]) -> Any { ["abi": abis.map { ["alias": $0] }] }
        func apk(_ apk: BundletoolToc.ApkDescription) -> [String: Any] {
            let targeting = apk.targeting
            var out: [String: Any] = ["path": apk.path, "targeting": [
                "abiTargeting": values(targeting.abi) { ["alias": $0] }, "languageTargeting": values(targeting.language) { $0 },
                "screenDensityTargeting": values(targeting.density, density), "sdkVersionTargeting": values(targeting.sdk, sdk),
                "textureCompressionFormatTargeting": values(targeting.textureFormat) { ["alias": $0] },
                "multiAbiTargeting": values(targeting.multiAbi, multiAbi),
                "sanitizerTargeting": ["value": targeting.sanitizer.map { ["alias": $0] }],
                "deviceTierTargeting": values(targeting.deviceTier) { $0 },
                "countrySetTargeting": values(targeting.countrySet) { $0 },
                "deviceGroupTargeting": values(targeting.deviceGroup) { $0 },
            ] as [String: Any]]
            switch apk.kind {
            case .split(let id, let master): out["splitApkMetadata"] = ["splitId": id, "isMasterSplit": master]
            case .standalone(let id): out["standaloneApkMetadata"] = ["splitId": id]
            case .instant(let id, let master): out["instantApkMetadata"] = ["splitId": id, "isMasterSplit": master]
            case .system: out["systemApkMetadata"] = [String: Any]()
            case .assetSlice(let id, let master): out["assetSliceMetadata"] = ["splitId": id, "isMasterSplit": master]
            case .apex: out["apexApkMetadata"] = [String: Any]()
            case .archived: out["archivedApkMetadata"] = [String: Any]()
            case .none: break
            }
            return out
        }
        func module(_ module: BundletoolToc.ModuleMetadata) -> [String: Any] {
            ["name": module.name, "deliveryType": module.deliveryType, "onDemandDeprecated": module.onDemandDeprecated,
             "isInstant": module.isInstant, "dependencies": module.dependencies, "moduleType": module.moduleType,
             "targeting": ["sdkVersionTargeting": values(module.targeting.sdk, sdk),
                           "deviceFeatureTargeting": module.targeting.deviceFeatures.map {
                               ["requiredFeature": ["featureName": $0.name, "featureVersion": $0.version]]
                           },
                           "deviceGroupTargeting": ["value": module.targeting.deviceGroups]] as [String: Any]]
        }
        let root: [String: Any] = [
            "packageName": toc.packageName, "bundletool": ["version": toc.bundletoolVersion],
            "variant": toc.variants.map { variant -> [String: Any] in
                let targeting = variant.targeting
                return ["variantNumber": variant.number, "targeting": [
                    "sdkVersionTargeting": values(targeting.sdk, sdk), "abiTargeting": values(targeting.abi) { ["alias": $0] },
                    "multiAbiTargeting": values(targeting.multiAbi, multiAbi),
                    "screenDensityTargeting": values(targeting.density, density),
                    "textureCompressionFormatTargeting": values(targeting.textureFormat) { ["alias": $0] },
                    "sdkRuntimeTargeting": ["requiresSdkRuntime": targeting.requiresSdkRuntime],
                ] as [String: Any], "apkSet": variant.apkSets.map { ["moduleMetadata": module($0.module), "apkDescription": $0.apks.map(apk)] }]
            },
            "assetSliceSet": toc.assetSliceSets.map {
                ["assetModuleMetadata": ["name": $0.module.name, "deliveryType": $0.module.deliveryType] as [String: Any],
                 "apkDescription": $0.apks.map(apk)]
            },
            "defaultTargetingValue": toc.defaultTargeting.map { ["dimension": $0.dimension, "defaultValue": $0.value] as [String: Any] },
        ]
        let data = try! JSONSerialization.data(withJSONObject: root, options: [.sortedKeys])
        return String(decoding: data, as: UTF8.self)
    }

    static func toc(_ toc: BundletoolToc) -> Data {
        var out = field(4, string: toc.packageName)
        for variant in toc.variants {
            let sets = variant.apkSets.reduce(Data()) {
                $0 + field(2, bytes: field(1, bytes: module($1.module)) + $1.apks.reduce(Data()) { $0 + field(2, bytes: apk($1)) })
            }
            out += field(1, bytes: field(1, bytes: variantTargeting(variant.targeting)) + sets
                                   + field(3, varint: UInt64(variant.number)))
        }
        out += field(2, bytes: field(2, string: toc.bundletoolVersion))
        for set in toc.assetSliceSets {
            let metadata = field(1, string: set.module.name) + field(4, int32: set.module.deliveryType)
            out += field(3, bytes: field(1, bytes: metadata) + set.apks.reduce(Data()) { $0 + field(2, bytes: apk($1)) })
        }
        for value in toc.defaultTargeting {
            out += field(7, bytes: field(1, int32: value.dimension) + field(2, string: value.value))
        }
        return out
    }
}
