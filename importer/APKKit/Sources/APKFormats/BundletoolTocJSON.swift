import Foundation

// MARK: - toc.json

extension BundletoolToc {
    /// Decodes `toc.json` as bundletool reads one (`ResultUtils.java:79-84`), with protobuf's JsonFormat ignoring
    /// unknown fields: a field by its JSON name or its proto name, a number or an enum's number as a number or a
    /// string, an enum by its name, and null as the default.
    public init(json data: Data) throws {
        guard let root = (try? JSONSerialization.jsonObject(with: data, options: [.fragmentsAllowed])) as? [String: Any] else {
            throw ProtoError.invalid("toc.json is no JSON object")
        }
        self.init()
        let toc = JSONMessage(root)
        packageName = try toc.string("packageName", "package_name") ?? ""
        bundletoolVersion = try toc.message("bundletool")?.string("version") ?? ""
        variants = try toc.messages("variant").map(Self.variant)
        assetSliceSets = try toc.messages("assetSliceSet", "asset_slice_set").map { set in
            let metadata = try set.message("assetModuleMetadata", "asset_module_metadata")
            return AssetSliceSet(
                module: AssetModuleMetadata(name: try metadata?.string("name") ?? "",
                                            deliveryType: try metadata?.enumeration(Names.delivery, "deliveryType", "delivery_type") ?? 0),
                apks: try set.messages("apkDescription", "apk_description").map(Self.apkDescription))
        }
        defaultTargeting = try toc.messages("defaultTargetingValue", "default_targeting_value").map {
            DefaultValue(dimension: try $0.enumeration(Names.splitDimension, "dimension") ?? 0,
                         value: try $0.string("defaultValue", "default_value") ?? "")
        }
    }

    static func variant(_ json: JSONMessage) throws -> Variant {
        var variant = Variant(number: try json.uint32("variantNumber", "variant_number") ?? 0)
        if let targeting = try json.message("targeting") {
            variant.targeting.sdk = try values(targeting.message("sdkVersionTargeting", "sdk_version_targeting"), sdkVersion)
            variant.targeting.abi = try values(targeting.message("abiTargeting", "abi_targeting"), abi)
            variant.targeting.multiAbi = try values(targeting.message("multiAbiTargeting", "multi_abi_targeting"), multiAbi)
            variant.targeting.density = try values(targeting.message("screenDensityTargeting", "screen_density_targeting"), density)
            variant.targeting.textureFormat = try values(
                targeting.message("textureCompressionFormatTargeting", "texture_compression_format_targeting"), textureFormat)
            variant.targeting.requiresSdkRuntime = try targeting.message("sdkRuntimeTargeting", "sdk_runtime_targeting")?
                .bool("requiresSdkRuntime", "requires_sdk_runtime") ?? false
        }
        variant.apkSets = try json.messages("apkSet", "apk_set").map { set in
            ApkSet(module: try set.message("moduleMetadata", "module_metadata").map(moduleMetadata) ?? ModuleMetadata(),
                   apks: try set.messages("apkDescription", "apk_description").map(apkDescription))
        }
        return variant
    }

    static func moduleMetadata(_ json: JSONMessage) throws -> ModuleMetadata {
        var module = ModuleMetadata()
        module.name = try json.string("name") ?? ""
        module.onDemandDeprecated = try json.bool("onDemandDeprecated", "on_demand_deprecated") ?? false
        module.isInstant = try json.bool("isInstant", "is_instant") ?? false
        module.dependencies = try json.strings("dependencies")
        module.deliveryType = try json.enumeration(Names.delivery, "deliveryType", "delivery_type") ?? 0
        module.moduleType = try json.enumeration(Names.moduleType, "moduleType", "module_type") ?? 0
        if let targeting = try json.message("targeting") {
            module.targeting.sdk = try values(targeting.message("sdkVersionTargeting", "sdk_version_targeting"), sdkVersion)
            module.targeting.deviceFeatures = try targeting.messages("deviceFeatureTargeting", "device_feature_targeting").map {
                let feature = try $0.message("requiredFeature", "required_feature")
                return DeviceFeature(name: try feature?.string("featureName", "feature_name") ?? "",
                                     version: try feature?.int32("featureVersion", "feature_version") ?? 0)
            }
            module.targeting.deviceGroups = try targeting.message("deviceGroupTargeting", "device_group_targeting")?
                .strings("value") ?? []
        }
        return module
    }

    static func apkDescription(_ json: JSONMessage) throws -> ApkDescription {
        var apk = ApkDescription(path: try json.string("path") ?? "")
        if let targeting = try json.message("targeting") { apk.targeting = try apkTargeting(targeting) }
        func split(_ json: JSONMessage) throws -> (String, Bool) {
            (try json.string("splitId", "split_id") ?? "", try json.bool("isMasterSplit", "is_master_split") ?? false)
        }
        if let metadata = try json.message("splitApkMetadata", "split_apk_metadata") {
            let (id, master) = try split(metadata)
            apk.kind = .split(id: id, isMaster: master)
        } else if let metadata = try json.message("standaloneApkMetadata", "standalone_apk_metadata") {
            apk.kind = .standalone(id: try metadata.string("splitId", "split_id") ?? "")
        } else if let metadata = try json.message("instantApkMetadata", "instant_apk_metadata") {
            let (id, master) = try split(metadata)
            apk.kind = .instant(id: id, isMaster: master)
        } else if try json.message("systemApkMetadata", "system_apk_metadata") != nil {
            apk.kind = .system
        } else if let metadata = try json.message("assetSliceMetadata", "asset_slice_metadata") {
            let (id, master) = try split(metadata)
            apk.kind = .assetSlice(id: id, isMaster: master)
        } else if try json.message("apexApkMetadata", "apex_apk_metadata") != nil {
            apk.kind = .apex
        } else if try json.message("archivedApkMetadata", "archived_apk_metadata") != nil {
            apk.kind = .archived
        }
        return apk
    }

    static func apkTargeting(_ json: JSONMessage) throws -> ApkTargeting {
        var targeting = ApkTargeting()
        targeting.abi = try values(json.message("abiTargeting", "abi_targeting"), abi)
        targeting.language = try values(json.message("languageTargeting", "language_targeting")) { try $0.string($1) }
        targeting.density = try values(json.message("screenDensityTargeting", "screen_density_targeting"), density)
        targeting.sdk = try values(json.message("sdkVersionTargeting", "sdk_version_targeting"), sdkVersion)
        targeting.textureFormat = try values(
            json.message("textureCompressionFormatTargeting", "texture_compression_format_targeting"), textureFormat)
        targeting.multiAbi = try values(json.message("multiAbiTargeting", "multi_abi_targeting"), multiAbi)
        targeting.sanitizer = try json.message("sanitizerTargeting", "sanitizer_targeting")?.messages("value").map {
            try $0.enumeration(Names.sanitizer, "alias") ?? 0
        } ?? []
        targeting.deviceTier = try values(json.message("deviceTierTargeting", "device_tier_targeting")) {
            try $0.int32($1, field: "deviceTierTargeting")
        }
        targeting.countrySet = try values(json.message("countrySetTargeting", "country_set_targeting")) { try $0.string($1) }
        targeting.deviceGroup = try values(json.message("deviceGroupTargeting", "device_group_targeting")) { try $0.string($1) }
        return targeting
    }

    /// A targeting message's `value` and `alternatives`, each element read by `element` from its JSON value.
    static func values<T>(_ json: JSONMessage?, _ element: (JSONMessage, Any) throws -> T) throws -> Values<T> {
        guard let json else { return Values() }
        return Values(value: try json.list("value").map { try element(json, $0) },
                      alternatives: try json.list("alternatives").map { try element(json, $0) })
    }

    static func abi(_ parent: JSONMessage, _ value: Any) throws -> Int32 {
        try parent.message(of: value, "abi").enumeration(Names.abi, "alias") ?? 0
    }

    static func textureFormat(_ parent: JSONMessage, _ value: Any) throws -> Int32 {
        try parent.message(of: value, "texture compression format").enumeration(Names.textureFormat, "alias") ?? 0
    }

    static func multiAbi(_ parent: JSONMessage, _ value: Any) throws -> [Int32] {
        try parent.message(of: value, "multi ABI").messages("abi").map { try $0.enumeration(Names.abi, "alias") ?? 0 }
    }

    static func density(_ parent: JSONMessage, _ value: Any) throws -> Density {
        let json = try parent.message(of: value, "screen density")
        if let dpi = try json.int32("densityDpi", "density_dpi") { return .dpi(dpi) }
        return .alias(try json.enumeration(Names.density, "densityAlias", "density_alias") ?? 0)
    }

    static func sdkVersion(_ parent: JSONMessage, _ value: Any) throws -> Int32? {
        try parent.message(of: value, "SDK version").int32("min")
    }

    /// protobuf's enum names (`targeting.proto`, `commands.proto`, `config.proto` at bundletool 1.18.3).
    enum Names {
        static let abi: [String: Int32] = [
            "UNSPECIFIED_CPU_ARCHITECTURE": 0, "ARMEABI": 1, "ARMEABI_V7A": 2, "ARM64_V8A": 3, "X86": 4, "X86_64": 5,
            "MIPS": 6, "MIPS64": 7, "RISCV64": 8,
        ]
        static let density: [String: Int32] = [
            "DENSITY_UNSPECIFIED": 0, "NODPI": 1, "LDPI": 2, "MDPI": 3, "TVDPI": 4, "HDPI": 5, "XHDPI": 6, "XXHDPI": 7,
            "XXXHDPI": 8,
        ]
        static let textureFormat: [String: Int32] = [
            "UNSPECIFIED_TEXTURE_COMPRESSION_FORMAT": 0, "ETC1_RGB8": 1, "PALETTED": 2, "THREE_DC": 3, "ATC": 4, "LATC": 5,
            "DXT1": 6, "S3TC": 7, "PVRTC": 8, "ASTC": 9, "ETC2": 10,
        ]
        static let sanitizer: [String: Int32] = ["NONE": 0, "HWADDRESS": 1]
        static let delivery: [String: Int32] = ["UNKNOWN_DELIVERY_TYPE": 0, "INSTALL_TIME": 1, "ON_DEMAND": 2, "FAST_FOLLOW": 3]
        static let moduleType: [String: Int32] = ["UNKNOWN_MODULE_TYPE": 0, "FEATURE_MODULE": 1, "ML_MODULE": 2, "SDK_MODULE": 3]
        static let splitDimension: [String: Int32] = [
            "UNSPECIFIED_VALUE": 0, "ABI": 1, "SCREEN_DENSITY": 2, "LANGUAGE": 3, "TEXTURE_COMPRESSION_FORMAT": 4,
            "DEVICE_TIER": 6, "COUNTRY_SET": 7, "AI_MODEL_VERSION": 8, "DEVICE_GROUP": 9,
        ]
    }
}

/// One JSON object of a toc, read as protobuf's JsonFormat reads a message.
struct JSONMessage {
    let fields: [String: Any]

    init(_ fields: [String: Any]) {
        self.fields = fields
    }

    /// The first of `names` that is present and not null, with the name it came by.
    private func value(_ names: [String]) -> (String, Any)? {
        for name in names {
            if let value = fields[name], !(value is NSNull) { return (name, value) }
        }
        return nil
    }

    func string(_ names: String...) throws -> String? {
        guard let (name, value) = value(names) else { return nil }
        return try string(value, field: name)
    }

    func string(_ value: Any, field: String = "value") throws -> String {
        guard let text = value as? String else { throw ProtoError.invalid("toc.json's \(field) is no string") }
        return text
    }

    func strings(_ names: String...) throws -> [String] {
        guard let (name, value) = value(names) else { return [] }
        guard let list = value as? [Any] else { throw ProtoError.invalid("toc.json's \(name) is no list") }
        return try list.map { try string($0, field: name) }
    }

    func bool(_ names: String...) throws -> Bool? {
        guard let (name, value) = value(names) else { return nil }
        if let number = value as? NSNumber, CFGetTypeID(number) == CFBooleanGetTypeID() { return number.boolValue }
        if let text = value as? String, text == "true" || text == "false" { return text == "true" }
        throw ProtoError.invalid("toc.json's \(name) is no bool")
    }

    func int32(_ names: String...) throws -> Int32? {
        guard let (name, value) = value(names) else { return nil }
        return try int32(value, field: name)
    }

    func int32(_ value: Any, field: String) throws -> Int32 {
        guard let whole = Self.integer(value), let result = Int32(exactly: whole) else {
            throw ProtoError.invalid("toc.json's \(field) is no int32")
        }
        return result
    }

    func uint32(_ names: String...) throws -> UInt32? {
        guard let (name, value) = value(names) else { return nil }
        guard let whole = Self.integer(value), let result = UInt32(exactly: whole) else {
            throw ProtoError.invalid("toc.json's \(name) is no uint32")
        }
        return result
    }

    /// An enum by its name or its number.
    func enumeration(_ known: [String: Int32], _ names: String...) throws -> Int32? {
        guard let (name, value) = value(names) else { return nil }
        if let text = value as? String, Self.integer(text) == nil {
            guard let number = known[text] else { throw ProtoError.invalid("toc.json's \(name) \(text) is no value Minato knows") }
            return number
        }
        return try int32(value, field: name)
    }

    func message(_ names: String...) throws -> JSONMessage? {
        guard let (name, value) = value(names) else { return nil }
        return try message(of: value, name)
    }

    func message(of value: Any, _ field: String) throws -> JSONMessage {
        guard let object = value as? [String: Any] else { throw ProtoError.invalid("toc.json's \(field) is no object") }
        return JSONMessage(object)
    }

    func messages(_ names: String...) throws -> [JSONMessage] {
        guard let (name, value) = value(names) else { return [] }
        guard let list = value as? [Any] else { throw ProtoError.invalid("toc.json's \(name) is no list") }
        return try list.map { try message(of: $0, name) }
    }

    func list(_ name: String) throws -> [Any] {
        guard let (_, value) = value([name]) else { return [] }
        guard let list = value as? [Any] else { throw ProtoError.invalid("toc.json's \(name) is no list") }
        return list
    }

    /// A whole number, from a JSON number without a fraction or a string of digits, as JsonFormat takes one.
    static func integer(_ value: Any) -> Int64? {
        if let number = value as? NSNumber, CFGetTypeID(number) != CFBooleanGetTypeID() {
            let double = number.doubleValue
            guard double.rounded() == double, abs(double) < 9.2e18 else { return nil }
            return number.int64Value
        }
        if let text = value as? String { return Int64(text) }
        return nil
    }
}
