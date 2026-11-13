import Foundation

/// The device a set is chosen for, as bundletool's device spec describes one (ADR-0048, decision 4).
public struct DeviceProfile: Sendable, Equatable {
    /// In order of preference, as Android's `Build.SUPPORTED_ABIS`.
    public var abis: [String]
    public var densityDpi: Int
    /// BCP 47 tags in order of preference.
    public var languages: [String]
    /// bundletool's split suffixes of the texture formats the device decodes.
    public var textureFormats: [String]
    /// What `extract-apks` takes without one in the spec or the table of contents: tier 0, no country set, no device
    /// group (`ExtractApksCommand.java:374-413`).
    public var deviceTier = 0
    public var countrySet: String?
    public var deviceGroups: [String] = []
    /// bundletool's device features, which its conditional modules go by. None means that every condition fits, as
    /// `extract-apks` takes a spec without them.
    public var features: [String] = []

    public init(abis: [String], densityDpi: Int, languages: [String], textureFormats: [String]) {
        self.abis = abis
        self.densityDpi = densityDpi
        self.languages = languages
        self.textureFormats = textureFormats
    }

    /// An iPhone: arm64-v8a, the density the guest is told (`runtime/android/configuration.cpp:214`), its
    /// preferred languages, the texture formats of the GLES and Vulkan paths (assumed: ASTC and ETC2), with
    /// ETC1, which ETC2 decoders read (the decision), and the features Minato's PackageManager reports, with
    /// the OpenGL ES version its sessions give (ADR-0049 decision 2's follow-up); `vulkan` says whether they have
    /// a Vulkan driver.
    public static func iPhone(nativeScale: Double, languages: [String],
                              vulkan: Bool = sessionsHaveVulkan) -> DeviceProfile {
        var profile = DeviceProfile(abis: ["arm64-v8a"], densityDpi: Int(nativeScale * 160 + 0.5),
                                    languages: languages, textureFormats: ["astc", "etc2", "etc1_rgb8"])
        profile.features = SystemFeatures.deviceSpec(vulkan: vulkan)
        return profile
    }

    /// Whether this build's sessions have a Vulkan driver: MoltenVK has no Simulator slice (ADR-0030, decision 8).
    public static var sessionsHaveVulkan: Bool {
        #if targetEnvironment(simulator)
        false
        #else
        true
        #endif
    }
}

/// Which splits of a set Minato installs (ADR-0048, decision 4).
public struct SplitChoice: Sendable, Equatable {
    public struct Skipped: Sendable, Equatable {
        public var member: SetMember
        public var reason: String
    }

    /// In split-name order.
    public var chosen: [SetMember]
    public var skipped: [Skipped]
    /// Why the set cannot run on the device, if it cannot.
    public var incompatibility: String?
}

/// Chooses a set's configuration splits as bundletool's `extract-apks` does for one device spec
/// (`device/ApkMatcher.java` at 1.18.3), module by module; every language split stays (your decision 1), and a
/// split whose configuration Minato does not know stays as well.
public enum SplitSelector {
    enum Dimension: Hashable {
        case abi, density, textureFormat, deviceTier, countrySet, deviceGroup
    }

    /// bundletool's ABI suffixes (`AbiNativeLibrariesSplitter`), as Android names the ABIs.
    static let abis = ["armeabi": "armeabi", "armeabi_v7a": "armeabi-v7a", "arm64_v8a": "arm64-v8a", "x86": "x86",
                       "x86_64": "x86_64", "mips": "mips", "mips64": "mips64", "riscv64": "riscv64"]
    /// bundletool's density buckets (`ResourcesUtils`).
    static let densities = ["ldpi": 120, "mdpi": 160, "tvdpi": 213, "hdpi": 240, "xhdpi": 320, "xxhdpi": 480,
                            "xxxhdpi": 640]
    /// bundletool's texture formats, best first (`TargetingComparators`).
    static let textureFormats = ["astc", "pvrtc", "s3tc", "dxt1", "latc", "atc", "three_dc", "etc2", "etc1_rgb8",
                                 "paletted"]
    /// The suffixes of bundletool's fallbacks, for the devices none of the other values serves (`ModuleSplit.java:
    /// 165-246`).
    static let otherABIs = "other_abis", otherTextureFormats = "other_tcf", otherCountrySets = "other_countries"

    public static func choose(_ set: CheckedSet, for device: DeviceProfile) -> SplitChoice {
        let splits = set.splits
        // Groups in the order of their first split, so the first incompatible one names the reason.
        var groups: [(group: Group, indices: [Int])] = []
        for (index, split) in splits.enumerated() {
            guard case .config(let module, let suffix) = split.role, let dimension = dimension(of: suffix) else { continue }
            let group = Group(module: module, dimension: dimension)
            if let position = groups.firstIndex(where: { $0.group == group }) {
                groups[position].indices.append(index)
            } else {
                groups.append((group, [index]))
            }
        }

        var reasons: [Int: String] = [:]
        var incompatibility: String?
        for (group, indices) in groups {
            let suffixes = indices.map { suffix(of: splits[$0]) }
            let match = match(group.dimension, suffixes: suffixes, device: device)
            for (index, suffix) in zip(indices, suffixes) where !match.chosen.contains(suffix) {
                reasons[index] = match.reason(suffix)
            }
            if incompatibility == nil { incompatibility = match.incompatibility }
        }

        var choice = SplitChoice(chosen: [], skipped: [], incompatibility: incompatibility)
        for (index, split) in splits.enumerated() {
            if let reason = reasons[index] {
                choice.skipped.append(SplitChoice.Skipped(member: split, reason: reason))
            } else {
                choice.chosen.append(split)
            }
        }
        // Android checks the APKs it installs (PackageInstallerSession.java:3652-3656), and Minato F1's signals: a
        // split type only skipped splits gave goes missing. A set that lacks it already needs splits instead.
        if choice.incompatibility == nil, let base = set.base, !set.needsSplits,
           SetChecks.needsSplits(base: base, splits: choice.chosen) {
            let missing = SetChecks.missingSplitTypes(base: base, splits: choice.chosen)
            choice.incompatibility = "The splits for this iPhone lack the split types \(missing.joined(separator: ", "))"
        }
        return choice
    }

    /// One group's choice: the suffixes that stay, why each other one goes, and why none fits, if none does.
    struct Match {
        var chosen: Set<String>
        var reason: (String) -> String
        var incompatibility: String?
    }

    static func match(_ dimension: Dimension, suffixes: [String], device: DeviceProfile) -> Match {
        switch dimension {
        case .abi:
            // AbiMatcher.java:43-117: the device's first ABI the splits offer; its fallback serves no device whose
            // ABI it lacks.
            let runs = device.abis.first ?? "none of them"
            let reason = { (suffix: String) in
                suffix == otherABIs ? "For other ABIs; this iPhone runs \(runs)" : "For the ABI \(abis[suffix]!); this iPhone runs \(runs)"
            }
            guard let wanted = device.abis.first(where: { abi in suffixes.contains { abis[$0] == abi } }) else {
                return Match(chosen: [], reason: reason, incompatibility: "No \(device.abis.first ?? "matching ABI") split")
            }
            return Match(chosen: Set(suffixes.filter { abis[$0] == wanted }), reason: reason)
        case .density:
            let best = suffixes.max { better(densities[$1]!, than: densities[$0]!, at: device.densityDpi) }!
            return Match(chosen: [best], reason: { "For the density \($0); \(best) fits this iPhone" })
        case .textureFormat:
            // TextureCompressionFormatMatcher.java:94-116, 148-154: the best format the device decodes, else the
            // fallback; without one bundletool refuses the device.
            guard let best = textureFormats.first(where: { suffixes.contains($0) && device.textureFormats.contains($0) }) else {
                let reason = { (suffix: String) in "For the texture format \(suffix), which this iPhone does not decode" }
                return suffixes.contains(otherTextureFormats)
                    ? Match(chosen: [otherTextureFormats], reason: reason)
                    : Match(chosen: [], reason: reason, incompatibility: "No texture format this iPhone decodes")
            }
            return Match(chosen: [best], reason: { suffix in
                suffix == otherTextureFormats ? "For other texture formats; \(best) fits this iPhone"
                    : "For the texture format \(suffix); \(best) fits this iPhone"
            })
        // Tiers, country sets and groups: where no split fits and no fallback serves, extract-apks refuses or skips.
        // A set pulled from a phone holds that phone's one split, which Minato keeps; of several, a tier takes the
        // lowest, and a country set or group without the fallback bundletool requires makes the set incompatible
        // (the decision).
        case .deviceTier:
            // DeviceTierApkMatcher.java:56-107: the device's tier.
            let wanted = "tier_\(device.deviceTier)"
            if suffixes.contains(wanted) {
                return Match(chosen: [wanted], reason: {
                    "For the device tier \($0.dropFirst("tier_".count)); this iPhone is tier \(device.deviceTier)"
                })
            }
            let lowest = suffixes.min { Int($0.dropFirst("tier_".count))! < Int($1.dropFirst("tier_".count))! }!
            return Match(chosen: [lowest], reason: {
                "For the device tier \($0.dropFirst("tier_".count)); this iPhone takes the lowest, tier \(lowest.dropFirst("tier_".count))"
            })
        case .countrySet:
            // CountrySetApkMatcher.java:53-99: the device's country set, else the fallback, which is all a device
            // without one takes.
            let countrySet = device.countrySet ?? ""
            let chosen = ["countries_\(countrySet)", otherCountrySets].first(where: suffixes.contains)
            let reason = { (suffix: String) in
                countrySet.isEmpty ? "For the country set \(suffix.dropFirst("countries_".count)); this iPhone has no country set"
                    : suffix == otherCountrySets ? "For other country sets; this iPhone is in \(countrySet)"
                    : "For the country set \(suffix.dropFirst("countries_".count)); this iPhone is in \(countrySet)"
            }
            if let chosen { return Match(chosen: [chosen], reason: reason) }
            return suffixes.count == 1 ? Match(chosen: Set(suffixes), reason: reason)
                : Match(chosen: [], reason: reason, incompatibility: "No country set split fits this iPhone")
        case .deviceGroup:
            // DeviceGroupApkMatcher.java:36-51: any group the device is in.
            let matching = Set(suffixes.filter { device.deviceGroups.contains(String($0.dropFirst("group_".count))) })
            let groups = device.deviceGroups.isEmpty ? "no device group" : device.deviceGroups.joined(separator: ", ")
            let reason = { (suffix: String) in "For the device group \(suffix.dropFirst("group_".count)); this iPhone is in \(groups)" }
            if !matching.isEmpty || suffixes.count == 1 {
                return Match(chosen: matching.isEmpty ? Set(suffixes) : matching, reason: reason)
            }
            return Match(chosen: [], reason: reason, incompatibility: "No device group split fits this iPhone")
        }
    }

    struct Group: Hashable {
        var module: String?
        var dimension: Dimension
    }

    static func dimension(of suffix: String) -> Dimension? {
        if abis[suffix] != nil || suffix == otherABIs { return .abi }
        if densities[suffix] != nil { return .density }
        if textureFormats.contains(suffix) || suffix == otherTextureFormats { return .textureFormat }
        if suffix.hasPrefix("tier_"), Int(suffix.dropFirst("tier_".count)) != nil { return .deviceTier }
        if suffix.hasPrefix("countries_") && suffix.count > "countries_".count || suffix == otherCountrySets {
            return .countrySet
        }
        if suffix.hasPrefix("group_") && suffix.count > "group_".count { return .deviceGroup }
        return nil
    }

    static func suffix(of split: SetMember) -> String {
        if case .config(_, let suffix) = split.role { suffix } else { "" }
    }

    /// Whether `dpi` fits `desired` better than `other`, by Android's resource rule that bundletool follows
    /// (`ResTable_config::isBetterThan`, `ScreenDensitySelector`): the exact one wins; a device at or above the
    /// higher one takes it, at or below the lower one takes that; in between, the lower one only if scaling it down
    /// costs less, (2 × lower − desired) × higher > desired².
    static func better(_ dpi: Int, than other: Int, at desired: Int) -> Bool {
        if dpi == other { return false }
        let (lower, higher) = dpi < other ? (dpi, other) : (other, dpi)
        let winner: Int
        if desired >= higher {
            winner = higher
        } else if desired <= lower {
            winner = lower
        } else {
            winner = (2 * lower - desired) * higher > desired * desired ? lower : higher
        }
        return winner == dpi
    }
}
