import APKFormats
import APKKit
import APKTestSupport
import Foundation
import Testing

/// The split choice of ADR-0048 decision 4, as bundletool's `extract-apks` makes it for one device spec.
@Suite("Split choice")
struct SplitSelectorTests {
    static let iPhone = DeviceProfile.iPhone(nativeScale: 3, languages: ["de-DE", "en-US"])

    static func split(_ name: String, splitTypes: String? = nil) throws -> SetMember {
        try SetMember.read(ZipArchive(data: SplitAPK.make(split: name, splitTypes: splitTypes)), fileName: "split_\(name).apk",
                           sha256: name)
    }

    static func choice(_ names: [String], for device: DeviceProfile = Self.iPhone) throws -> SplitChoice {
        try SplitSelector.choose(SetChecks.check(names.map { try Self.split($0) }).get(), for: device)
    }

    static func chosen(_ names: [String], for device: DeviceProfile = Self.iPhone) throws -> [String] {
        try Self.choice(names, for: device).chosen.compactMap(\.splitName)
    }

    @Test func theIPhonesProfile() {
        #expect(Self.iPhone.abis == ["arm64-v8a"])
        #expect(Self.iPhone.densityDpi == 480)
        #expect(DeviceProfile.iPhone(nativeScale: 2, languages: []).densityDpi == 320)
        // Rounded as the guest is told (configuration.cpp:214): 2.88 × 160 = 460.8.
        #expect(DeviceProfile.iPhone(nativeScale: 2.88, languages: []).densityDpi == 461)
        // ETC2 decoders read ETC1 too (the decision).
        #expect(Self.iPhone.textureFormats == ["astc", "etc2", "etc1_rgb8"])
        // extract-apks without a device tier, country set or device group in the spec or its table of contents
        // (ExtractApksCommand.java:374-413).
        #expect(Self.iPhone.deviceTier == 0)
        #expect(Self.iPhone.countrySet == nil)
        #expect(Self.iPhone.deviceGroups.isEmpty)
    }

    @Test func onlyTheArm64Split() throws {
        let choice = try Self.choice(["config.arm64_v8a", "config.armeabi_v7a", "config.x86_64"])
        #expect(choice.chosen.map(\.splitName) == ["config.arm64_v8a"])
        #expect(choice.skipped.map(\.member.splitName) == ["config.armeabi_v7a", "config.x86_64"])
        #expect(choice.incompatibility == nil)
        #expect(try Self.chosen(["config.arm64_v8a", "config.other_abis"]) == ["config.arm64_v8a"])
    }

    @Test func abiSplitsWithoutArm64MakeTheSetIncompatible() throws {
        #expect(try Self.choice(["config.armeabi_v7a", "config.de"]).incompatibility == "No arm64-v8a split")
        // bundletool's ABI fallback never serves a device whose ABI it lacks (AbiMatcher.java:87-117).
        #expect(try Self.choice(["config.other_abis", "config.x86"]).incompatibility == "No arm64-v8a split")
    }

    @Test func eachModuleChoosesOnItsOwn() throws {
        #expect(try Self.chosen(["config.arm64_v8a", "config.x86", "feature.config.arm64_v8a", "feature.config.x86"])
            == ["config.arm64_v8a", "feature.config.arm64_v8a"])
        #expect(try Self.choice(["config.arm64_v8a", "feature.config.x86"]).incompatibility == "No arm64-v8a split")
    }

    @Test func theFirstIncompatibleGroupNamesTheReason() throws {
        #expect(try Self.choice(["config.x86", "pack.config.pvrtc"]).incompatibility == "No arm64-v8a split")
    }

    static let densities = ["config.ldpi", "config.mdpi", "config.tvdpi", "config.hdpi", "config.xhdpi", "config.xxhdpi",
                            "config.xxxhdpi"]

    @Test(arguments: [(100, "config.ldpi"), (160, "config.mdpi"), (320, "config.xhdpi"), (363, "config.xhdpi"),
                      (364, "config.xxhdpi"), (460, "config.xxhdpi"), (480, "config.xxhdpi"), (526, "config.xxhdpi"),
                      (527, "config.xxxhdpi"), (640, "config.xxxhdpi"), (800, "config.xxxhdpi")])
    func oneDensitySplitByBundletoolsComparator(dpi: Int, expected: String) throws {
        var device = Self.iPhone
        device.densityDpi = dpi
        let choice = try Self.choice(Self.densities, for: device)
        #expect(choice.chosen.map(\.splitName) == [expected])
        #expect(choice.skipped.count == Self.densities.count - 1)
    }

    @Test func aDensityBetweenTwoSplitsScalesDownBeforeUp() throws {
        // At 480 dpi between mdpi and xxxhdpi: (2 × 160 − 480) × 640 is below 480², so xxxhdpi wins.
        #expect(try Self.chosen(["config.mdpi", "config.xxxhdpi"]) == ["config.xxxhdpi"])
        // At 200 dpi between mdpi and xhdpi: (2 × 160 − 200) × 320 = 38,400 is below 40,000, so xhdpi wins.
        var device = Self.iPhone
        device.densityDpi = 200
        #expect(try Self.chosen(["config.mdpi", "config.xhdpi"], for: device) == ["config.xhdpi"])
        // At 170 dpi: (320 − 170) × 320 = 48,000 is above 28,900, so mdpi wins.
        device.densityDpi = 170
        #expect(try Self.chosen(["config.mdpi", "config.xhdpi"], for: device) == ["config.mdpi"])
        // A tie at 160 dpi between ldpi and xhdpi: (240 − 160) × 320 = 25,600 = 160², and only "above" keeps the lower.
        device.densityDpi = 160
        #expect(try Self.chosen(["config.ldpi", "config.xhdpi"], for: device) == ["config.xhdpi"])
    }

    @Test func everyLanguageSplitStays() throws {
        // Decision 1: Android resolves among them, and a change of the iPhone's language needs no new import.
        #expect(try Self.chosen(["config.de", "config.en", "config.fr", "config.zh"]) == ["config.de", "config.en", "config.fr", "config.zh"])
    }

    @Test func astcBeforeEtc2() throws {
        #expect(try Self.chosen(["pack.config.astc", "pack.config.etc2", "pack.config.pvrtc"]) == ["pack.config.astc"])
        var etc2Only = Self.iPhone
        etc2Only.textureFormats = ["etc2"]
        #expect(try Self.chosen(["pack.config.astc", "pack.config.etc2"], for: etc2Only) == ["pack.config.etc2"])
        #expect(try Self.chosen(["pack.config.etc1_rgb8", "pack.config.s3tc"]) == ["pack.config.etc1_rgb8"])
    }

    @Test func theTextureFallbackOnlyWhenNoListedFormatFits() throws {
        // bundletool's fallback, from the folder without a suffix (ModuleSplit.java:216-224), holds the same paths as
        // the others once their suffixes are stripped, so only one of them may stay (TextureCompressionFormatMatcher.java:
        // 94-116).
        #expect(try Self.chosen(["pack.config.astc", "pack.config.other_tcf", "pack.config.pvrtc"]) == ["pack.config.astc"])
        #expect(try Self.chosen(["pack.config.other_tcf", "pack.config.pvrtc", "pack.config.s3tc"]) == ["pack.config.other_tcf"])
    }

    @Test func noTextureFormatThisIPhoneDecodes() throws {
        // bundletool refuses the device (TextureCompressionFormatMatcher.java:148-154).
        let choice = try Self.choice(["pack.config.pvrtc", "pack.config.s3tc"])
        #expect(choice.chosen.isEmpty)
        #expect(choice.skipped.count == 2)
        #expect(choice.incompatibility == "No texture format this iPhone decodes")
    }

    @Test func theDeviceTierOfTheProfile() throws {
        #expect(try Self.chosen(["pack.config.tier_0", "pack.config.tier_1", "pack.config.tier_2"]) == ["pack.config.tier_0"])
        var device = Self.iPhone
        device.deviceTier = 2
        #expect(try Self.chosen(["pack.config.tier_0", "pack.config.tier_2"], for: device) == ["pack.config.tier_2"])
    }

    @Test func aSetFromAPhoneKeepsTheOneSplitItHasWhereNoneFitsAndNoFallbackServes() throws {
        // extract-apks refuses a tier the splits lack (DeviceTierApkMatcher.java:93-107) and skips country sets and
        // groups; a set pulled from a phone has only that phone's (the decision, ADR-0048 decision 4).
        let fromAPhone = try Self.choice(["config.arm64_v8a", "config.xxhdpi", "pack.config.tier_2"])
        #expect(fromAPhone.incompatibility == nil)
        #expect(fromAPhone.chosen.map(\.splitName) == ["config.arm64_v8a", "config.xxhdpi", "pack.config.tier_2"])
        #expect(try Self.chosen(["pack.config.countries_latam"]) == ["pack.config.countries_latam"])
        #expect(try Self.chosen(["pack.config.group_high"]) == ["pack.config.group_high"])
    }

    @Test func severalSplitsNoneFittingAndNoFallback() throws {
        // A tier takes the lowest the set has; a country set or group needs a fallback in bundletool, so a set
        // without one is malformed (the decision).
        let tiers = try Self.choice(["pack.config.tier_1", "pack.config.tier_3", "pack.config.tier_2"])
        #expect(tiers.chosen.map(\.splitName) == ["pack.config.tier_1"])
        #expect(tiers.skipped.map(\.reason) == ["For the device tier 2; this iPhone takes the lowest, tier 1",
                                                "For the device tier 3; this iPhone takes the lowest, tier 1"])
        #expect(tiers.incompatibility == nil)
        let countries = try Self.choice(["pack.config.countries_latam", "pack.config.countries_sea"])
        #expect(countries.chosen.isEmpty)
        #expect(countries.skipped.map(\.reason) == ["For the country set latam; this iPhone has no country set",
                                                    "For the country set sea; this iPhone has no country set"])
        #expect(countries.incompatibility == "No country set split fits this iPhone")
        let groups = try Self.choice(["pack.config.group_high", "pack.config.group_low"])
        #expect(groups.chosen.isEmpty)
        #expect(groups.skipped.map(\.reason) == ["For the device group high; this iPhone is in no device group",
                                                 "For the device group low; this iPhone is in no device group"])
        #expect(groups.incompatibility == "No device group split fits this iPhone")
    }

    @Test func theCountrySetOfTheProfileElseTheFallback() throws {
        // CountrySetApkMatcher.java:53-74: without a country set only the fallback matches.
        #expect(try Self.chosen(["pack.config.countries_latam", "pack.config.other_countries"]) == ["pack.config.other_countries"])
        var device = Self.iPhone
        device.countrySet = ""
        #expect(try Self.chosen(["pack.config.countries_latam", "pack.config.other_countries"], for: device)
            == ["pack.config.other_countries"])
        device.countrySet = "latam"
        let latam = try Self.choice(["pack.config.countries_latam", "pack.config.countries_sea", "pack.config.other_countries"],
                                    for: device)
        #expect(latam.chosen.map(\.splitName) == ["pack.config.countries_latam"])
        #expect(latam.skipped.map(\.reason) == ["For the country set sea; this iPhone is in latam",
                                                "For other country sets; this iPhone is in latam"])
        // A set without the profile's country set takes its fallback, where extract-apks would refuse.
        device.countrySet = "eu"
        #expect(try Self.chosen(["pack.config.countries_latam", "pack.config.other_countries"], for: device)
            == ["pack.config.other_countries"])
    }

    @Test func deviceGroupSplitsOnlyForTheProfilesGroups() throws {
        // DeviceGroupApkMatcher.java:36-42.
        var device = Self.iPhone
        device.deviceGroups = ["high"]
        #expect(try Self.chosen(["pack.config.group_high", "pack.config.group_low"], for: device) == ["pack.config.group_high"])
    }

    @Test func theChosenSplitsMustStillGiveTheRequiredSplitTypes() throws {
        // Android checks the APKs it installs (PackageInstallerSession.java:3652-3656), so a split type only skipped
        // splits gave goes missing.
        let base = try SetMember.read(ZipArchive(data: SplitAPK.make(requiredSplitTypes: "base__density")),
                                      fileName: "base.apk", sha256: "base")
        let set = try SetChecks.check([base, Self.split("config.mdpi", splitTypes: "base__density"), Self.split("config.xxhdpi")]).get()
        #expect(!set.missingSplit)
        let choice = SplitSelector.choose(set, for: Self.iPhone)
        #expect(choice.chosen.map(\.splitName) == ["config.xxhdpi"])
        #expect(choice.incompatibility == "The splits for this iPhone lack the split types base__density")
        // A set that already lacks them needs splits rather than being incompatible, also by F1's signals.
        let lacking = try SetChecks.check([base]).get()
        #expect(SplitSelector.choose(lacking, for: Self.iPhone).incompatibility == nil)
        let older = try SetMember.read(ZipArchive(data: SplitAPK.make(applicationSplitRequired: true)), fileName: "base.apk",
                                       sha256: "base")
        #expect(SplitSelector.choose(try SetChecks.check([older]).get(), for: Self.iPhone).incompatibility == nil)
        // An earlier reason stays.
        let abi = try SetMember.read(ZipArchive(data: SplitAPK.make(requiredSplitTypes: "base__abi")), fileName: "base.apk",
                                     sha256: "base")
        let x86 = try SetChecks.check([abi, Self.split("config.x86", splitTypes: "base__abi")]).get()
        #expect(SplitSelector.choose(x86, for: Self.iPhone).incompatibility == "No arm64-v8a split")
    }

    @Test func featureSplitsAndSplitsMinatoCannotPlaceStay() throws {
        #expect(try Self.chosen(["dynamic", "config.arm64_v8a_hwasan", "other"]) == ["config.arm64_v8a_hwasan", "dynamic", "other"])
    }

    @Test func skippedSplitsSayWhy() throws {
        var device = Self.iPhone
        device.deviceGroups = ["high"]
        let choice = try Self.choice(["config.arm64_v8a", "config.armeabi_v7a", "config.other_abis", "config.xhdpi", "config.xxhdpi",
                                      "pack.config.astc", "pack.config.countries_latam", "pack.config.etc2",
                                      "pack.config.group_high", "pack.config.group_low", "pack.config.other_countries",
                                      "pack.config.other_tcf", "pack.config.tier_0", "pack.config.tier_1"], for: device)
        #expect(choice.skipped.map(\.reason) == [
            "For the ABI armeabi-v7a; this iPhone runs arm64-v8a",
            "For other ABIs; this iPhone runs arm64-v8a",
            "For the density xhdpi; xxhdpi fits this iPhone",
            "For the country set latam; this iPhone has no country set",
            "For the texture format etc2; astc fits this iPhone",
            "For the device group low; this iPhone is in high",
            "For other texture formats; astc fits this iPhone",
            "For the device tier 1; this iPhone is tier 0",
        ])
        var bare = Self.iPhone
        bare.textureFormats = []
        #expect(try Self.choice(["pack.config.astc"], for: bare).skipped.map(\.reason) == [
            "For the texture format astc, which this iPhone does not decode",
        ])
    }
}
