import Foundation

/// The device a table of contents is chosen for: an iPhone's profile and what bundletool's device spec adds to it
/// (ADR-0049 decision 2).
public struct BundletoolDevice: Sendable, Equatable {
    public var profile: DeviceProfile
    /// Android 15's, the guest's.
    public var sdkVersion: Int32 = 35
    /// Minato runs no SDK runtime.
    public var supportsSdkRuntime = false
    /// bundletool's device features, `reqGlEsVersion=0x30000` among them. None means that every conditional module
    /// fits, as `extract-apks` takes a spec without them.
    public var features: [String] = []
    /// nil takes the toc's default, else tier 0, no country set and the group "" (`ExtractApksCommand.java:374-413`).
    public var deviceTier: Int32?
    public var countrySet: String?
    public var deviceGroups: [String] = []

    public init(_ profile: DeviceProfile) {
        self.profile = profile
    }
}

/// What Minato takes from a table of contents (ADR-0049 decision 2).
public struct BundletoolChoice: Sendable, Equatable {
    public struct APK: Sendable, Equatable {
        public var path: String
        public var module: String

        public init(path: String, module: String) {
            self.path = path
            self.module = module
        }
    }

    /// An asset pack Play delivers after the install, fast-follow or on-demand, for `packs/` (format 6).
    public struct Pack: Sendable, Equatable {
        public var name: String
        public var deliveryType: Int32
        public var paths: [String]

        public init(name: String, deliveryType: Int32, paths: [String]) {
            self.name = name
            self.deliveryType = deliveryType
            self.paths = paths
        }
    }

    /// The APKs to install, in the toc's order: the variant's modules', then the install-time asset packs'.
    public var apks: [APK] = []
    public var packs: [Pack] = []
    /// Why nothing fits this iPhone, as `extract-apks` would refuse it.
    public var incompatibility: String?

    public init(apks: [APK] = [], packs: [Pack] = [], incompatibility: String? = nil) {
        self.apks = apks
        self.packs = packs
        self.incompatibility = incompatibility
    }
}

/// Chooses from bundletool's table of contents as `extract-apks` does for one device spec at 1.18.3
/// (`ExtractApksCommand.java`, `device/VariantMatcher.java`, `device/ApkMatcher.java` and the dimensions'
/// matchers), with ADR-0049 decision 2's departures: on-demand feature modules install, as `--modules` would ask for
/// them (the M5 plan's decision 4); every language APK stays (its decision 1); and fast-follow and on-demand asset
/// packs are named for `packs/` (format 6).
public enum BundletoolSelector {
    struct Incompatible: Error {
        var reason: String
    }

    public static func choose(_ toc: BundletoolToc, for device: BundletoolDevice) -> BundletoolChoice {
        do {
            return try Chooser(toc: toc, device: device).choose()
        } catch let incompatible as Incompatible {
            return BundletoolChoice(incompatibility: incompatible.reason)
        } catch {
            return BundletoolChoice(incompatibility: "\(error)")
        }
    }
}

/// One choice's device spec, with `ExtractApksCommand.applyDefaultsToDeviceSpec`'s defaults.
private struct Chooser {
    typealias Toc = BundletoolToc
    typealias Incompatible = BundletoolSelector.Incompatible

    let toc: Toc
    let sdk: Int32
    let supportsSdkRuntime: Bool
    /// In the device's order of preference.
    let abis: [Int32]
    let firstABI: String
    let density: Int
    /// The device's formats, bundletool's best first (`TargetingComparators.sortTextureCompressionFormat`).
    let textureFormats: [Int32]
    let features: Set<String>
    let glVersion: Int
    let deviceTier: Int32
    let countrySet: String
    let deviceGroups: [String]

    init(toc: Toc, device: BundletoolDevice) throws {
        self.toc = toc
        sdk = device.sdkVersion
        supportsSdkRuntime = device.supportsSdkRuntime
        abis = device.profile.abis.compactMap { Toc.abiAliases[$0] }
        firstABI = device.profile.abis.first ?? "matching ABI"
        density = device.profile.densityDpi
        let formats = Set(device.profile.textureFormats.compactMap { Toc.textureFormatAliases[$0] })
        textureFormats = SplitSelector.textureFormats.compactMap { Toc.textureFormatAliases[$0] }.filter(formats.contains)
        features = Set(device.features)
        // OpenGlFeatureMatcher.java: the first reqGlEsVersion, as Integer.decode reads it.
        glVersion = device.features.lazy.compactMap { feature -> Int? in
            guard feature.hasPrefix("reqGlEsVersion=") else { return nil }
            return Self.decode(String(feature.dropFirst("reqGlEsVersion=".count)))
        }.first ?? 0
        func defaultValue(_ dimension: Int32) -> String? {
            toc.defaultTargeting.first { $0.dimension == dimension && !$0.value.isEmpty }?.value
        }
        if let tier = device.deviceTier {
            deviceTier = tier
        } else if let text = defaultValue(Toc.SplitDimension.deviceTier) {
            guard let tier = Int32(text) else {
                throw Incompatible(reason: "Its table of contents names the device tier “\(text)”, which is no number")
            }
            deviceTier = tier
        } else {
            deviceTier = 0
        }
        countrySet = device.countrySet ?? defaultValue(Toc.SplitDimension.countrySet) ?? ""
        deviceGroups = device.deviceGroups.isEmpty ? [defaultValue(Toc.SplitDimension.deviceGroup) ?? ""] : device.deviceGroups
    }

    /// Java's Integer.decode: decimal, 0x or # hexadecimal, 0 octal.
    static func decode(_ text: String) -> Int? {
        let lower = text.lowercased()
        if lower.hasPrefix("0x") { return Int(lower.dropFirst(2), radix: 16) }
        if lower.hasPrefix("#") { return Int(lower.dropFirst(), radix: 16) }
        if lower.count > 1, lower.hasPrefix("0") { return Int(lower.dropFirst(), radix: 8) }
        return Int(lower)
    }

    func choose() throws -> BundletoolChoice {
        var choice = BundletoolChoice()
        if let variant = try matchingVariant() {
            choice.apks = try apks(of: variant)
        }
        for set in toc.assetSliceSets {
            let paths = try set.apks.filter(matchesAPK).map(\.path)
            switch set.module.deliveryType {
            case Toc.DeliveryType.installTime:
                choice.apks += paths.map { BundletoolChoice.APK(path: $0, module: set.module.name) }
            case Toc.DeliveryType.fastFollow, Toc.DeliveryType.onDemand:
                choice.packs.append(BundletoolChoice.Pack(name: set.module.name, deliveryType: set.module.deliveryType, paths: paths))
            default:
                break
            }
        }
        // ExtractApksCommand.java:221-225.
        guard !choice.apks.isEmpty else { throw Incompatible(reason: "It has no APK for this iPhone") }
        return choice
    }

    // MARK: - Variants (VariantMatcher)

    /// VariantMatcher.java:76-100: a device with the SDK runtime takes its variants when one matches, the others
    /// otherwise; at most one may match.
    func matchingVariant() throws -> Toc.Variant? {
        var matching = supportsSdkRuntime ? try allMatching(toc.variants.filter(\.targeting.requiresSdkRuntime)) : []
        if matching.isEmpty { matching = try allMatching(toc.variants.filter { !$0.targeting.requiresSdkRuntime }) }
        guard matching.count <= 1 else { throw Incompatible(reason: "More than one of its variants fits this iPhone") }
        return matching.first
    }

    /// The non-instant variants, each of which must suit the device, that match it.
    func allMatching(_ variants: [Toc.Variant]) throws -> [Toc.Variant] {
        let candidates = try variants.filter(isCandidate)
        for variant in candidates { try checkVariant(variant.targeting) }
        return try candidates.filter { try matchesVariant($0.targeting) }
    }

    /// Not an instant variant, whose every APK is instant.
    func isCandidate(_ variant: Toc.Variant) throws -> Bool {
        !variant.apkSets.flatMap(\.apks).allSatisfy {
            if case .instant = $0.kind { true } else { false }
        }
    }

    func checkVariant(_ targeting: Toc.VariantTargeting) throws {
        try checkSDK(targeting.sdk)
        try checkABI(targeting.abi)
        try checkMultiABI(targeting.multiAbi)
        try checkTextureFormat(targeting.textureFormat)
    }

    /// Each dimension the device has. The SDK runtime's matcher always agrees here: `matchingVariant` passes only
    /// variants the device's SDK runtime takes.
    func matchesVariant(_ targeting: Toc.VariantTargeting) throws -> Bool {
        try matchesSDK(targeting.sdk)
            && (abis.isEmpty || matchesABI(targeting.abi))
            && (abis.isEmpty || matchesMultiABI(targeting.multiAbi))
            && (density == 0 || matchesDensity(targeting.density))
            && (textureFormats.isEmpty || matchesTextureFormat(targeting.textureFormat))
    }

    // MARK: - Modules (ApkMatcher, ModuleMatcher)

    func apks(of variant: Toc.Variant) throws -> [BundletoolChoice.APK] {
        let version = try BundletoolVersion(toc.bundletoolVersion.isEmpty ? "1.18.3" : toc.bundletoolVersion)
        let newDelivery = version >= BundletoolVersion.newDeliveryType
        var wanted = Set<String>()
        for set in variant.apkSets {
            let module = set.module
            let installTime = newDelivery ? module.deliveryType == Toc.DeliveryType.installTime : !module.onDemandDeprecated
            let onDemand = newDelivery ? module.deliveryType == Toc.DeliveryType.onDemand : module.onDemandDeprecated
            // SDK modules belong to the SDK runtime.
            if installTime ? try matchesModule(module.targeting) : onDemand && module.moduleType != 3 {
                wanted.insert(module.name)
            }
        }
        // ModuleDependenciesUtils: each module's dependencies and base, transitively.
        var dependencies: [String: [String]] = [:]
        for set in variant.apkSets { dependencies[set.module.name, default: []] += set.module.dependencies + ["base"] }
        var pending = Array(wanted)
        while let name = pending.popLast() {
            for dependency in dependencies[name] ?? [] where wanted.insert(dependency).inserted { pending.append(dependency) }
        }

        var chosen: [BundletoolChoice.APK] = []
        for set in variant.apkSets where wanted.contains(set.module.name) {
            for apk in set.apks { try checkAPK(apk.targeting) }
            let matched = try set.apks.filter(matchesAPK)
            // ExtractApksCommand.java:213-218 asks for every ABI and density dimension the module's APKs have.
            func dimensions(_ apks: [Toc.ApkDescription]) -> Set<String> {
                Set(apks.flatMap { apk -> [String] in
                    (apk.targeting.abi.isEmpty ? [] : ["ABI"]) + (apk.targeting.density.isEmpty ? [] : ["screen density"])
                })
            }
            let missing = dimensions(set.apks).subtracting(dimensions(matched))
            if let dimension = missing.sorted().first {
                throw Incompatible(reason: "No \(dimension) APK of the module “\(set.module.name)” fits this iPhone")
            }
            chosen += matched.map { BundletoolChoice.APK(path: $0.path, module: set.module.name) }
        }
        return chosen
    }

    /// ModuleMatcher: the SDK, the device features and OpenGL ES version when the spec names features, and the
    /// device groups.
    func matchesModule(_ targeting: Toc.ModuleTargeting) throws -> Bool {
        guard try matchesSDK(targeting.sdk) else { return false }
        if !features.isEmpty {
            let openGL = "android.hardware.opengles.version"
            let required = targeting.deviceFeatures.map(\.name).filter { $0 != openGL }
            guard required.allSatisfy(features.contains) else { return false }
            let versions = targeting.deviceFeatures.filter { $0.name == openGL }.map(\.version)
            guard versions.count <= 1 else {
                throw Incompatible(reason: "A module of it names more than one OpenGL ES version")
            }
            if let version = versions.first, glVersion < Int(version) { return false }
        }
        return targeting.deviceGroups.isEmpty || !Set(targeting.deviceGroups).isDisjoint(with: deviceGroups)
    }

    // MARK: - APKs (ApkMatcher's dimensions)

    func checkAPK(_ targeting: Toc.ApkTargeting) throws {
        try checkSDK(targeting.sdk)
        try checkABI(targeting.abi)
        try checkMultiABI(targeting.multiAbi)
        try checkTextureFormat(targeting.textureFormat)
        // DeviceTierApkMatcher, CountrySetApkMatcher: the device's value must be among the APK's.
        if !targeting.deviceTier.isEmpty, !(targeting.deviceTier.value + targeting.deviceTier.alternatives).contains(deviceTier) {
            throw Incompatible(reason: "No device tier APK fits this iPhone")
        }
        if !targeting.countrySet.isEmpty, !countrySet.isEmpty,
           !(targeting.countrySet.value + targeting.countrySet.alternatives).contains(countrySet) {
            throw Incompatible(reason: "No country set APK fits this iPhone")
        }
    }

    /// Every dimension, also those the device lacks, as ApkMatcher's predicates are; every language stays.
    func matchesAPK(_ apk: Toc.ApkDescription) throws -> Bool {
        let targeting = apk.targeting
        return try matchesSDK(targeting.sdk) && matchesABI(targeting.abi) && matchesMultiABI(targeting.multiAbi)
            && matchesDensity(targeting.density) && matchesTextureFormat(targeting.textureFormat)
            && matchesDeviceGroup(targeting.deviceGroup) && matchesDeviceTier(targeting.deviceTier)
            && matchesCountrySet(targeting.countrySet)
    }

    // MARK: - The dimensions

    /// SdkVersionMatcher: at most one value; no alternative with a higher minimum that the device runs.
    func matchesSDK(_ targeting: Toc.Values<Int32?>) throws -> Bool {
        guard targeting.value.count <= 1 else { throw Incompatible(reason: "Its table of contents names more than one SDK version for an APK") }
        let value = targeting.value.first ?? nil
        guard fitsSDK(value) else { return false }
        return !targeting.alternatives.contains { alternative in
            guard let minimum = alternative, fitsSDK(minimum) else { return false }
            return minimum > (value ?? 0)
        }
    }

    func fitsSDK(_ minimum: Int32?) -> Bool { minimum.map { $0 <= sdk } ?? true }

    func checkSDK(_ targeting: Toc.Values<Int32?>) throws {
        let values = targeting.value.isEmpty ? [nil] : targeting.value
        guard (values + targeting.alternatives).contains(where: fitsSDK) else {
            let minimum = (values + targeting.alternatives).compactMap { $0 }.min() ?? 0
            throw Incompatible(reason: "It needs Android SDK \(minimum) or later, and this iPhone runs SDK \(sdk)")
        }
    }

    /// AbiMatcher: the device's ABIs in order of preference; the first one found decides.
    func matchesABI(_ targeting: Toc.Values<Int32>) throws -> Bool {
        guard !targeting.isEmpty else { return true }
        try exclusive(targeting)
        for abi in abis {
            if targeting.value.contains(abi) { return true }
            if targeting.alternatives.contains(abi) { return false }
        }
        return targeting.value.isEmpty
    }

    func checkABI(_ targeting: Toc.Values<Int32>) throws {
        guard !abis.isEmpty, !targeting.isEmpty else { return }
        if Set(targeting.value + targeting.alternatives).isDisjoint(with: abis) {
            throw Incompatible(reason: "No \(firstABI) APK")
        }
    }

    /// MultiAbiMatcher: a value the device runs whole, and no alternative it runs that ranks above every value.
    func matchesMultiABI(_ targeting: Toc.Values<[Int32]>) throws -> Bool {
        guard !targeting.isEmpty else { return true }
        let device = Set(abis)
        let values = targeting.value.map(Set.init)
        guard values.contains(where: { $0.isSubset(of: device) }) else { return false }
        return !targeting.alternatives.map(Set.init).contains { alternative in
            alternative.isSubset(of: device) && values.allSatisfy { Self.ranks(alternative, above: $0) }
        }
    }

    func checkMultiABI(_ targeting: Toc.Values<[Int32]>) throws {
        guard !abis.isEmpty, !targeting.isEmpty else { return }
        let device = Set(abis)
        if !(targeting.value + targeting.alternatives).contains(where: { Set($0).isSubset(of: device) }) {
            throw Incompatible(reason: "No \(firstABI) APK")
        }
    }

    /// TargetingComparators.MULTI_ABI_ALIAS_COMPARATOR: each set sorted from its best ABI by ARCHITECTURE_ORDERING,
    /// armeabi < armeabi-v7a < arm64-v8a < x86 < x86_64 < mips < mips64 < riscv64, which are the aliases' own
    /// numbers 1 to 8, and compared lexicographically, so a set that holds another ranks above it.
    static func ranks(_ left: Set<Int32>, above right: Set<Int32>) -> Bool {
        let (a, b) = (left.sorted(by: >), right.sorted(by: >))
        for (x, y) in zip(a, b) where x != y { return x > y }
        return a.count > b.count
    }

    /// ScreenDensityMatcher: a value that is the best density of all for the device, by Android's rule.
    func matchesDensity(_ targeting: Toc.Values<Toc.Density>) throws -> Bool {
        let all = targeting.value + targeting.alternatives
        guard !all.isEmpty else { return true }
        let dpis = try all.map(dpi)
        let best = dpis.dropFirst().reduce(dpis[0]) { SplitSelector.better($1, than: $0, at: density) ? $1 : $0 }
        return try targeting.value.map(dpi).contains(best)
    }

    /// ResourcesUtils.convertToDpi.
    func dpi(_ density: Toc.Density) throws -> Int {
        switch density {
        case .dpi(let dpi): return Int(dpi)
        case .alias(let alias):
            guard let dpi = Toc.densityAliasDPI[alias] else {
                throw Incompatible(reason: "Its table of contents names the screen density \(alias), which Minato does not know")
            }
            return Int(dpi)
        }
    }

    /// TextureCompressionFormatMatcher: the device's formats, best first; the first one found decides; a fallback
    /// fits when none does.
    func matchesTextureFormat(_ targeting: Toc.Values<Int32>) throws -> Bool {
        guard !targeting.isEmpty else { return true }
        try exclusive(targeting)
        for format in textureFormats {
            if targeting.value.contains(format) { return true }
            if targeting.alternatives.contains(format) { return false }
        }
        // A fallback, whose alternatives are all there is.
        return targeting.value.isEmpty
    }

    func checkTextureFormat(_ targeting: Toc.Values<Int32>) throws {
        guard !textureFormats.isEmpty, !targeting.isEmpty, !targeting.value.isEmpty else { return }
        if Set(targeting.value + targeting.alternatives).isDisjoint(with: textureFormats) {
            throw Incompatible(reason: "No texture format this iPhone decodes")
        }
    }

    func matchesDeviceGroup(_ targeting: Toc.Values<String>) -> Bool {
        targeting.isEmpty || !Set(targeting.value).isDisjoint(with: deviceGroups)
    }

    func matchesDeviceTier(_ targeting: Toc.Values<Int32>) throws -> Bool {
        guard !targeting.isEmpty else { return true }
        try exclusive(targeting)
        return targeting.value.contains(deviceTier)
    }

    /// CountrySetApkMatcher: without a country set, only a fallback fits.
    func matchesCountrySet(_ targeting: Toc.Values<String>) throws -> Bool {
        guard !targeting.isEmpty else { return true }
        try exclusive(targeting)
        if countrySet.isEmpty { return targeting.value.isEmpty }
        return targeting.value.contains(countrySet)
    }

    /// bundletool's checkArgument: values and alternatives exclude each other.
    func exclusive<T: Hashable>(_ targeting: Toc.Values<T>) throws {
        if !Set(targeting.value).isDisjoint(with: targeting.alternatives) {
            throw Incompatible(reason: "Its table of contents names a value both as targeted and as an alternative")
        }
    }
}

/// bundletool's `Version`: `<major>.<minor>.<revision>[-<qualifier>]`, a "dev" qualifier first and none last.
struct BundletoolVersion: Comparable {
    static let newDeliveryType = try! BundletoolVersion("0.10.2")

    let numbers: [Int]
    let qualifier: String?

    init(_ text: String) throws {
        let parts = text.split(separator: "-", maxSplits: 1, omittingEmptySubsequences: false)
        let numbers = parts[0].split(separator: ".", omittingEmptySubsequences: false)
        guard numbers.count == 3, numbers.allSatisfy({ !$0.isEmpty && $0.allSatisfy { ("0"..."9").contains($0) } }),
              parts.count == 1 || !parts[1].isEmpty, let values = Optional(numbers.compactMap { Int($0) }),
              values.count == 3 else {
            throw BundletoolSelector.Incompatible(reason: "Its table of contents names bundletool “\(text)”, which is no version")
        }
        self.numbers = values
        qualifier = parts.count == 2 ? String(parts[1]) : nil
    }

    static func < (left: Self, right: Self) -> Bool {
        if left.numbers != right.numbers { return left.numbers.lexicographicallyPrecedes(right.numbers) }
        switch (left.qualifier.map { $0 == "dev" ? "" : $0 }, right.qualifier.map { $0 == "dev" ? "" : $0 }) {
        case (nil, _): return false
        case (_, nil): return true
        case let (a?, b?): return a < b
        }
    }
}
