import Foundation

/// bundletool's table of contents, `BuildApksResult` of `commands.proto` and `targeting.proto` at 1.18.3, as far as
/// the choice reads it (ADR-0049 decision 2). Enums keep protobuf's numbers, so a value bundletool adds later stays
/// what it is, as protobuf keeps an unknown enum value.
public struct BundletoolToc: Sendable, Equatable {
    public var packageName = ""
    /// bundletool's version; empty where the toc names none.
    public var bundletoolVersion = ""
    public var variants: [Variant] = []
    public var assetSliceSets: [AssetSliceSet] = []
    /// `default_targeting_value`: a split dimension's number (`config.proto`'s `SplitDimension.Value`) and the value
    /// `extract-apks` takes for a device that names none.
    public var defaultTargeting: [DefaultValue] = []

    public init() {}

    /// A dimension's values and their alternatives, as each targeting message of `targeting.proto` has them.
    public struct Values<T: Sendable & Equatable>: Sendable, Equatable {
        public var value: [T] = []
        public var alternatives: [T] = []

        public init(value: [T] = [], alternatives: [T] = []) {
            self.value = value
            self.alternatives = alternatives
        }

        public var isEmpty: Bool { value.isEmpty && alternatives.isEmpty }
    }

    /// `ScreenDensity`: an alias (`DensityAlias`) or dots per inch.
    public enum Density: Sendable, Equatable {
        case alias(Int32)
        case dpi(Int32)
    }

    public struct VariantTargeting: Sendable, Equatable {
        /// `SdkVersion.min`, nil where it has none.
        public var sdk = Values<Int32?>()
        public var abi = Values<Int32>()
        public var multiAbi = Values<[Int32]>()
        public var density = Values<Density>()
        public var textureFormat = Values<Int32>()
        public var requiresSdkRuntime = false

        public init() {}
    }

    public struct ApkTargeting: Sendable, Equatable {
        public var abi = Values<Int32>()
        public var language = Values<String>()
        public var density = Values<Density>()
        public var sdk = Values<Int32?>()
        public var textureFormat = Values<Int32>()
        public var multiAbi = Values<[Int32]>()
        public var sanitizer: [Int32] = []
        public var deviceTier = Values<Int32>()
        public var countrySet = Values<String>()
        public var deviceGroup = Values<String>()

        public init() {}
    }

    public struct DeviceFeature: Sendable, Equatable {
        public var name: String
        public var version: Int32

        public init(name: String, version: Int32) {
            self.name = name
            self.version = version
        }
    }

    public struct ModuleTargeting: Sendable, Equatable {
        public var sdk = Values<Int32?>()
        public var deviceFeatures: [DeviceFeature] = []
        public var deviceGroups: [String] = []

        public init() {}
    }

    /// What `apk_metadata_oneof_value` names an APK.
    public enum Kind: Sendable, Equatable {
        case split(id: String, isMaster: Bool)
        case standalone(id: String)
        case instant(id: String, isMaster: Bool)
        case system
        case assetSlice(id: String, isMaster: Bool)
        case apex
        case archived
        case none
    }

    public struct ApkDescription: Sendable, Equatable {
        public var targeting = ApkTargeting()
        public var path = ""
        public var kind = Kind.none

        public init(targeting: ApkTargeting = ApkTargeting(), path: String = "", kind: Kind = .none) {
            self.targeting = targeting
            self.path = path
            self.kind = kind
        }
    }

    public struct ModuleMetadata: Sendable, Equatable {
        public var name = ""
        /// `DeliveryType`: install-time 1, on-demand 2, fast-follow 3.
        public var deliveryType: Int32 = 0
        public var onDemandDeprecated = false
        public var isInstant = false
        public var dependencies: [String] = []
        public var targeting = ModuleTargeting()
        public var moduleType: Int32 = 0

        public init() {}
    }

    public struct ApkSet: Sendable, Equatable {
        public var module = ModuleMetadata()
        public var apks: [ApkDescription] = []

        public init(module: ModuleMetadata = ModuleMetadata(), apks: [ApkDescription] = []) {
            self.module = module
            self.apks = apks
        }
    }

    public struct Variant: Sendable, Equatable {
        public var targeting = VariantTargeting()
        public var apkSets: [ApkSet] = []
        public var number: UInt32 = 0

        public init(targeting: VariantTargeting = VariantTargeting(), apkSets: [ApkSet] = [], number: UInt32 = 0) {
            self.targeting = targeting
            self.apkSets = apkSets
            self.number = number
        }
    }

    public struct AssetModuleMetadata: Sendable, Equatable {
        public var name = ""
        public var deliveryType: Int32 = 0

        public init(name: String = "", deliveryType: Int32 = 0) {
            self.name = name
            self.deliveryType = deliveryType
        }
    }

    public struct AssetSliceSet: Sendable, Equatable {
        public var module = AssetModuleMetadata()
        public var apks: [ApkDescription] = []

        public init(module: AssetModuleMetadata = AssetModuleMetadata(), apks: [ApkDescription] = []) {
            self.module = module
            self.apks = apks
        }
    }

    public struct DefaultValue: Sendable, Equatable {
        public var dimension: Int32
        public var value: String

        public init(dimension: Int32, value: String) {
            self.dimension = dimension
            self.value = value
        }
    }
}

// MARK: - protobuf's numbers (targeting.proto, commands.proto, config.proto at 1.18.3)

extension BundletoolToc {
    public enum DeliveryType {
        public static let installTime: Int32 = 1, onDemand: Int32 = 2, fastFollow: Int32 = 3
    }

    /// `SplitDimension.Value`.
    public enum SplitDimension {
        public static let deviceTier: Int32 = 6, countrySet: Int32 = 7, deviceGroup: Int32 = 9
    }

    /// `Abi.AbiAlias`, by Android's names.
    public static let abiAliases: [String: Int32] = [
        "armeabi": 1, "armeabi-v7a": 2, "arm64-v8a": 3, "x86": 4, "x86_64": 5, "mips": 6, "mips64": 7, "riscv64": 8,
    ]

    /// `ScreenDensity.DensityAlias` and the dots per inch bundletool reads each as (`ResourcesUtils.convertToDpi`).
    public static let densityAliasDPI: [Int32: Int32] = [1: 0xFFFF, 2: 120, 3: 160, 4: 213, 5: 240, 6: 320, 7: 480, 8: 640]

    /// `TextureCompressionFormat.TextureCompressionFormatAlias`, by bundletool's split suffixes.
    public static let textureFormatAliases: [String: Int32] = [
        "etc1_rgb8": 1, "paletted": 2, "three_dc": 3, "atc": 4, "latc": 5, "dxt1": 6, "s3tc": 7, "pvrtc": 8, "astc": 9,
        "etc2": 10,
    ]
}

// MARK: - toc.pb

extension BundletoolToc {
    /// Decodes `toc.pb`. A field of a known number but another wire type is skipped as an unknown one, as protobuf's
    /// Java parser does.
    public init(proto data: Data) throws {
        self.init()
        var reader = try ProtoReader(data)
        while let field = try reader.next() {
            switch (field.number, field.value) {
            case (4, .bytes): packageName = try field.value.string()
            case (1, .bytes(let bytes)): variants.append(try Self.variant(reader.message(bytes)))
            case (2, .bytes(let bytes)):
                var bundletool = try reader.message(bytes)
                while let field = try bundletool.next() {
                    if case (2, .bytes) = (field.number, field.value) { bundletoolVersion = try field.value.string() }
                }
            case (3, .bytes(let bytes)): assetSliceSets.append(try Self.assetSliceSet(reader.message(bytes)))
            case (7, .bytes(let bytes)):
                var message = try reader.message(bytes)
                var value = DefaultValue(dimension: 0, value: "")
                while let field = try message.next() {
                    switch (field.number, field.value) {
                    case (1, .varint): value.dimension = try field.value.int32()
                    case (2, .bytes): value.value = try field.value.string()
                    default: break
                    }
                }
                defaultTargeting.append(value)
            default: break
            }
        }
    }

    static func variant(_ reader: ProtoReader) throws -> Variant {
        var reader = reader, variant = Variant()
        while let field = try reader.next() {
            switch (field.number, field.value) {
            case (1, .bytes(let bytes)): variant.targeting = try variantTargeting(reader.message(bytes))
            case (2, .bytes(let bytes)): variant.apkSets.append(try apkSet(reader.message(bytes)))
            case (3, .varint(let number)): variant.number = UInt32(truncatingIfNeeded: number)
            default: break
            }
        }
        return variant
    }

    static func apkSet(_ reader: ProtoReader) throws -> ApkSet {
        var reader = reader, set = ApkSet()
        while let field = try reader.next() {
            switch (field.number, field.value) {
            case (1, .bytes(let bytes)): set.module = try moduleMetadata(reader.message(bytes))
            case (2, .bytes(let bytes)): set.apks.append(try apkDescription(reader.message(bytes)))
            default: break
            }
        }
        return set
    }

    static func moduleMetadata(_ reader: ProtoReader) throws -> ModuleMetadata {
        var reader = reader, module = ModuleMetadata()
        while let field = try reader.next() {
            switch (field.number, field.value) {
            case (1, .bytes): module.name = try field.value.string()
            case (2, .varint): module.onDemandDeprecated = try field.value.bool()
            case (3, .varint): module.isInstant = try field.value.bool()
            case (4, .bytes): module.dependencies.append(try field.value.string())
            case (5, .bytes(let bytes)): module.targeting = try moduleTargeting(reader.message(bytes))
            case (6, .varint): module.deliveryType = try field.value.int32()
            case (7, .varint): module.moduleType = try field.value.int32()
            default: break
            }
        }
        return module
    }

    static func assetSliceSet(_ reader: ProtoReader) throws -> AssetSliceSet {
        var reader = reader, set = AssetSliceSet()
        while let field = try reader.next() {
            switch (field.number, field.value) {
            case (1, .bytes(let bytes)):
                var metadata = try reader.message(bytes)
                while let field = try metadata.next() {
                    switch (field.number, field.value) {
                    case (1, .bytes): set.module.name = try field.value.string()
                    case (4, .varint): set.module.deliveryType = try field.value.int32()
                    default: break
                    }
                }
            case (2, .bytes(let bytes)): set.apks.append(try apkDescription(reader.message(bytes)))
            default: break
            }
        }
        return set
    }

    static func apkDescription(_ reader: ProtoReader) throws -> ApkDescription {
        var reader = reader, apk = ApkDescription()
        while let field = try reader.next() {
            switch (field.number, field.value) {
            case (1, .bytes(let bytes)): apk.targeting = try apkTargeting(reader.message(bytes))
            case (2, .bytes): apk.path = try field.value.string()
            case (3, .bytes(let bytes)):
                let (id, master) = try splitMetadata(reader.message(bytes))
                apk.kind = .split(id: id, isMaster: master)
            case (4, .bytes(let bytes)):
                var metadata = try reader.message(bytes), id = ""
                while let field = try metadata.next() {
                    if case (3, .bytes) = (field.number, field.value) { id = try field.value.string() }
                }
                apk.kind = .standalone(id: id)
            case (5, .bytes(let bytes)):
                let (id, master) = try splitMetadata(reader.message(bytes))
                apk.kind = .instant(id: id, isMaster: master)
            case (6, .bytes): apk.kind = .system
            case (7, .bytes(let bytes)):
                let (id, master) = try splitMetadata(reader.message(bytes))
                apk.kind = .assetSlice(id: id, isMaster: master)
            case (8, .bytes): apk.kind = .apex
            case (9, .bytes): apk.kind = .archived
            default: break
            }
        }
        return apk
    }

    static func splitMetadata(_ reader: ProtoReader) throws -> (String, Bool) {
        var reader = reader, id = "", master = false
        while let field = try reader.next() {
            switch (field.number, field.value) {
            case (1, .bytes): id = try field.value.string()
            case (2, .varint): master = try field.value.bool()
            default: break
            }
        }
        return (id, master)
    }

    static func variantTargeting(_ reader: ProtoReader) throws -> VariantTargeting {
        var reader = reader, targeting = VariantTargeting()
        while let field = try reader.next() {
            guard case .bytes(let bytes) = field.value else { continue }
            let message = try reader.message(bytes)
            switch field.number {
            case 1: targeting.sdk = try values(message, sdkVersion)
            case 2: targeting.abi = try values(message, abi)
            case 3: targeting.density = try values(message, density)
            case 4: targeting.multiAbi = try values(message, multiAbi)
            case 5: targeting.textureFormat = try values(message, textureFormat)
            case 6:
                var runtime = message
                while let field = try runtime.next() {
                    if case (1, .varint) = (field.number, field.value) { targeting.requiresSdkRuntime = try field.value.bool() }
                }
            default: break
            }
        }
        return targeting
    }

    static func apkTargeting(_ reader: ProtoReader) throws -> ApkTargeting {
        var reader = reader, targeting = ApkTargeting()
        while let field = try reader.next() {
            guard case .bytes(let bytes) = field.value else { continue }
            let message = try reader.message(bytes)
            switch field.number {
            case 1: targeting.abi = try values(message, abi)
            case 3: targeting.language = try values(message) { try $1.string() }
            case 4: targeting.density = try values(message, density)
            case 5: targeting.sdk = try values(message, sdkVersion)
            case 6: targeting.textureFormat = try values(message, textureFormat)
            case 7: targeting.multiAbi = try values(message, multiAbi)
            case 8:
                var sanitizers = message
                while let field = try sanitizers.next() {
                    if case (1, .bytes(let bytes)) = (field.number, field.value) {
                        targeting.sanitizer.append(try alias(sanitizers.message(bytes)))
                    }
                }
            case 9: targeting.deviceTier = try values(message, valueField: 3, alternativesField: 4, int32Value)
            case 10: targeting.countrySet = try values(message) { try $1.string() }
            case 11: targeting.deviceGroup = try values(message) { try $1.string() }
            default: break
            }
        }
        return targeting
    }

    static func moduleTargeting(_ reader: ProtoReader) throws -> ModuleTargeting {
        var reader = reader, targeting = ModuleTargeting()
        while let field = try reader.next() {
            guard case .bytes(let bytes) = field.value else { continue }
            var message = try reader.message(bytes)
            switch field.number {
            case 1: targeting.sdk = try values(message, sdkVersion)
            case 2:
                // DeviceFeatureTargeting's required_feature.
                while let field = try message.next() {
                    guard case (1, .bytes(let bytes)) = (field.number, field.value) else { continue }
                    var feature = try message.message(bytes), name = "", version: Int32 = 0
                    while let field = try feature.next() {
                        switch (field.number, field.value) {
                        case (1, .bytes): name = try field.value.string()
                        case (2, .varint): version = try field.value.int32()
                        default: break
                        }
                    }
                    targeting.deviceFeatures.append(DeviceFeature(name: name, version: version))
                }
            case 5:
                while let field = try message.next() {
                    if case (1, .bytes) = (field.number, field.value) { targeting.deviceGroups.append(try field.value.string()) }
                }
            default: break
            }
        }
        return targeting
    }

    /// A targeting message's `value` and `alternatives`, each element read by `element` from its field's value.
    static func values<T>(_ reader: ProtoReader, valueField: Int = 1, alternativesField: Int = 2,
                          _ element: (ProtoReader, ProtoReader.Value) throws -> T) throws -> Values<T> {
        var reader = reader, values = Values<T>()
        while let field = try reader.next() {
            guard case .bytes = field.value else { continue }
            if field.number == valueField { values.value.append(try element(reader, field.value)) }
            if field.number == alternativesField { values.alternatives.append(try element(reader, field.value)) }
        }
        return values
    }

    /// An alias message (`Abi`, `TextureCompressionFormat`, `Sanitizer`): its field 1.
    static func alias(_ reader: ProtoReader) throws -> Int32 {
        var reader = reader, alias: Int32 = 0
        while let field = try reader.next() {
            if case (1, .varint) = (field.number, field.value) { alias = try field.value.int32() }
        }
        return alias
    }

    static func abi(_ parent: ProtoReader, _ value: ProtoReader.Value) throws -> Int32 {
        try alias(parent.message(value.data()))
    }

    static func textureFormat(_ parent: ProtoReader, _ value: ProtoReader.Value) throws -> Int32 {
        try alias(parent.message(value.data()))
    }

    static func multiAbi(_ parent: ProtoReader, _ value: ProtoReader.Value) throws -> [Int32] {
        var reader = try parent.message(value.data()), abis: [Int32] = []
        while let field = try reader.next() {
            if case (1, .bytes(let bytes)) = (field.number, field.value) { abis.append(try alias(reader.message(bytes))) }
        }
        return abis
    }

    static func density(_ parent: ProtoReader, _ value: ProtoReader.Value) throws -> Density {
        var reader = try parent.message(value.data()), density = Density.alias(0)
        while let field = try reader.next() {
            switch (field.number, field.value) {
            case (1, .varint): density = .alias(try field.value.int32())
            case (2, .varint): density = .dpi(try field.value.int32())
            default: break
            }
        }
        return density
    }

    /// `SdkVersion`: its `min`, a `google.protobuf.Int32Value`, nil where absent.
    static func sdkVersion(_ parent: ProtoReader, _ value: ProtoReader.Value) throws -> Int32? {
        var reader = try parent.message(value.data()), min: Int32?
        while let field = try reader.next() {
            if case (1, .bytes) = (field.number, field.value) { min = try int32Value(reader, field.value) }
        }
        return min
    }

    /// `google.protobuf.Int32Value`: its field 1, 0 where absent.
    static func int32Value(_ parent: ProtoReader, _ value: ProtoReader.Value) throws -> Int32 {
        var reader = try parent.message(value.data()), result: Int32 = 0
        while let field = try reader.next() {
            if case (1, .varint) = (field.number, field.value) { result = try field.value.int32() }
        }
        return result
    }
}
