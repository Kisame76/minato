import APKFormats
import APKKit
import APKTestSupport
import Foundation
import Testing

/// Android 15's checks of one install session over a set (PackageInstallerSession.java, android-15.0.0_r1), with
/// its texts as the reasons, and Minato's departures of ADR-0048 decision 3.
@Suite("Set checks")
struct SetChecksTests {
    static func member(_ data: Data, _ fileName: String) throws -> SetMember {
        try SetMember.read(ZipArchive(data: data), fileName: fileName, sha256: fileName)
    }

    static func base(_ fileName: String = "base.apk", versionCode: Int32 = 1, manifestSplitRequired: Bool = false,
                     applicationSplitRequired: Bool = false, requiredSplitTypes: String? = nil,
                     signers: [TestKey] = [.ec]) throws -> SetMember {
        try member(SplitAPK.make(versionCode: versionCode, manifestSplitRequired: manifestSplitRequired,
                                 applicationSplitRequired: applicationSplitRequired, requiredSplitTypes: requiredSplitTypes,
                                 signers: signers), fileName)
    }

    static func split(_ name: String, versionCode: Int32 = 1, requiredSplitTypes: String? = nil, splitTypes: String? = nil,
                      signers: [TestKey] = [.ec]) throws -> SetMember {
        try member(SplitAPK.make(versionCode: versionCode, split: name, requiredSplitTypes: requiredSplitTypes,
                                 splitTypes: splitTypes, signers: signers), "split_\(name).apk")
    }

    static func refusal(_ members: [SetMember]) -> String? {
        if case .failure(let refusal) = SetChecks.check(members) { refusal.reason } else { nil }
    }

    static func checked(_ members: [SetMember]) throws -> CheckedSet {
        try SetChecks.check(members).get()
    }

    // MARK: - The set

    @Test func aBaseWithItsSplitsInSplitNameOrder() throws {
        let set = try Self.checked([Self.split("config.xxhdpi"), Self.base(), Self.split("config.arm64_v8a")])
        #expect(set.base?.fileName == "base.apk")
        #expect(set.splits.map(\.splitName) == ["config.arm64_v8a", "config.xxhdpi"])
        #expect(!set.missingSplit && !set.needsSplits)
    }

    @Test func splitsWithoutTheirBaseMayOnlyJoinAnInstalledOne() throws {
        let set = try Self.checked([Self.split("config.de"), Self.split("config.en")])
        #expect(set.base == nil)
        #expect(set.splits.count == 2)
    }

    @Test func aSplitNameDefinedTwice() throws {
        #expect(try Self.refusal([Self.base(), Self.split("config.de"), Self.split("config.de")])
            == "Split config.de was defined multiple times")
        #expect(try Self.refusal([Self.base("a.apk"), Self.base("b.apk")]) == "Split base was defined multiple times")
    }

    @Test func splitNamesThatDifferOnlyInCase() throws {
        // Android's ext4 tells them apart, APFS does not (the decision).
        #expect(try Self.refusal([Self.base(), Self.split("config.de"), Self.split("config.De")])
            == "Splits config.De and config.de differ only in case")
        #expect(try Self.refusal([Self.base(), Self.split("config.De"), Self.split("config.DE")])
            == "Splits config.DE and config.De differ only in case")
    }

    @Test func anotherVersionCode() throws {
        #expect(try Self.refusal([Self.base(), Self.split("config.de", versionCode: 2)])
            == "split_config.de.apk version code 2 inconsistent with 1")
        let major = try Self.member(SplitAPK.make(versionCodeMajor: 1), "base.apk")
        let otherMajor = try Self.member(SplitAPK.make(versionCodeMajor: 2, split: "config.de"), "split_config.de.apk")
        #expect(Self.refusal([major, otherMajor]) == "split_config.de.apk version code 8589934593 inconsistent with 4294967297")
    }

    @Test func noApkAtAll() {
        #expect(Self.refusal([]) == "No packages staged")
    }

    @Test func aSplitWhoseFileNameWouldBeTooLong() throws {
        // Android names the file split_<name>.apk and refuses a name over 255 bytes (:3508-3513).
        let longest = "config." + String(repeating: "a", count: 238)
        #expect(try Self.checked([Self.base(), Self.split(longest)]).splits.count == 1)
        let tooLong = longest + "a"
        #expect(try Self.refusal([Self.base(), Self.split(tooLong)]) == "Invalid filename: split_\(tooLong).apk")
    }

    @Test func anotherPackage() throws {
        let other = try Self.member(SplitAPK.make(package: "org.example.other", split: "config.de"), "other.apk")
        #expect(try Self.refusal([Self.base(), other]) == "other.apk package org.example.other inconsistent with \(SplitAPK.package)")
    }

    // MARK: - Signers

    @Test func everySplitIsSignedExactlyAsItsBase() throws {
        #expect(try Self.checked([Self.base(), Self.split("config.de")]).signing
            == .verified(Self.base().signature.signer!))
        #expect(try Self.refusal([Self.base(), Self.split("config.de", signers: [.rsa])])
            == "split_config.de.apk signatures are inconsistent")
        #expect(try Self.refusal([Self.base(), Self.split("config.de", signers: [.ec, .rsa])])
            == "split_config.de.apk signatures are inconsistent")
        // Signature.areExactArraysMatch: the same certificates in any order, and as many (Signature.java:331-334).
        #expect(try Self.checked([Self.base(signers: [.ec, .rsa]), Self.split("config.de", signers: [.rsa, .ec])]).splits.count == 1)
        #expect(try Self.refusal([Self.base(), Self.split("config.de", signers: [.ec, .ec])])
            == "split_config.de.apk signatures are inconsistent")
    }

    @Test func anApkMinatoCannotVerifyIsComparedByTheCertificatesItClaims() throws {
        // Android compares every APK with the session's first signer (:3501-3506). Minato cannot check DSA, so it
        // compares the certificates such an APK names: a split of another key refuses the set (decision 6).
        #expect(try Self.refusal([Self.base(), Self.split("config.de", signers: [.dsa])])
            == "split_config.de.apk signatures are inconsistent")
        #expect(try Self.refusal([Self.base(signers: [.dsa]), Self.split("config.de"), Self.split("config.en", signers: [.rsa])])
            == "split_config.de.apk signatures are inconsistent")
        #expect(try Self.refusal([Self.base(signers: [.dsa]), Self.split("config.de", signers: [.dsa, .ec])])
            == "split_config.de.apk signatures are inconsistent")
        #expect(try Self.refusal([Self.split("config.de", signers: [.dsa]), Self.split("config.en"),
                                  Self.split("config.fr", signers: [.rsa])])
            == "split_config.en.apk signatures are inconsistent")
    }

    @Test func everyApkMatchesExactlyBehindOneMinatoCannotVerify() throws {
        // A base whose proof Minato cannot keep names its certificate; every APK still counts its certificates.
        var base = try Self.base()
        base.signature = SignatureVerification(check: .unverified("a key Security.framework cannot read"))
        #expect(try Self.refusal([base, Self.split("config.de"), Self.split("config.en", signers: [.ec, .ec])])
            == "split_config.en.apk signatures are inconsistent")
        #expect(try Self.refusal([base, Self.split("config.en", signers: [.ec, .ec])]) == "split_config.en.apk signatures are inconsistent")
    }

    static func namingTheBase(_ name: String, files: [String: Data] = [:], count: Int = 1) -> Data {
        SplitAPK.make(split: name, files: files,
                      v2: Array(repeating: TestSigner.Signer(.dsa, certificate: TestKey.ec.certificate), count: count))
    }

    @Test func theExactMatchCountsAndContainsButIsNoMultiset() throws {
        // Signature.areExactArraysMatch: as many on both sides, and each side's in the other.
        #expect(try Self.checked([Self.base(signers: [.ec, .ec, .rsa]), Self.split("config.de", signers: [.ec, .rsa, .rsa])])
            .splits.count == 1)
        #expect(try Self.refusal([Self.base(signers: [.ec, .rsa]), Self.split("config.de", signers: [.ec, .ec, .rsa])])
            == "split_config.de.apk signatures are inconsistent")
    }

    @Test func anUnverifiableApkNamingTheBasesCertificateTwiceIsRefused() throws {
        // A probe: named twice is not the base's one signer (Signature.java:331-334).
        #expect(try Self.refusal([Self.base(), Self.member(Self.namingTheBase("config.de", count: 2), "split_config.de.apk")])
            == "split_config.de.apk signatures are inconsistent")
    }

    @Test func verifiedAndUnverifiableApksTogetherAsk() throws {
        // A probe: a DSA signer that names the base's certificate proves nothing about the content, so a set
        // that mixes verified and unverifiable APKs asks even on a fresh install, naming them (the decision).
        var data = Self.namingTheBase("feature", files: ["classes.dex": Data(repeating: 7, count: 64)])
        let archive = try ZipArchive(data: data)
        data[Int(try archive.dataOffset(of: try #require(archive.entry(named: "classes.dex"))))] ^= 0xFF
        let split = try Self.member(data, "split_feature.apk")
        let base = try Self.base()
        #expect(split.certificates == base.certificates)
        #expect(try Self.checked([base, split]).signing == .partlyVerified(
            base.signature.signer!, unverifiable: [UnverifiableFile(fileName: "split_feature.apk", reason: "signed with DSA with SHA-256")]))
        // Every APK unverifiable: as one such APK (ADR-0034).
        #expect(try Self.checked([Self.base(signers: [.dsa]), Self.split("config.de", signers: [.dsa])]).signing
            == .unverified("signed with DSA with SHA-256"))
    }

    @Test func aTamperedSplitOfAnotherKeyMinatoCannotVerify() throws {
        // A probe: DSA leaves the content unchecked, so only its certificate tells.
        var data = SplitAPK.make(split: "feature", isFeatureSplit: true, files: ["classes.dex": Data(repeating: 7, count: 64)],
                                 signers: [.dsa])
        let archive = try ZipArchive(data: data)
        let entry = try #require(archive.entry(named: "classes.dex"))
        data[Int(try archive.dataOffset(of: entry))] ^= 0xFF
        let split = try Self.member(data, "split_feature.apk")
        #expect(split.signature.check != .verified)
        #expect(try Self.refusal([Self.base(), split]) == "split_feature.apk signatures are inconsistent")
    }

    @Test func aSplitSignedWithV1OnlyCountsAsItsBasesSigner() throws {
        // Minato checks the minimum scheme on the base alone. Android checks it on the session's signer, the first
        // APK it stages, in no fixed order (PackageInstallerSession.java:1541, 3501-3503): Minato's rule is that
        // order with the base first, a departure of ADR-0048.
        let base = try Self.member(SplitAPK.make(targetSdk: 34), "base.apk")
        let split = try Self.member(SplitAPK.make(split: "config.de", targetSdk: 34, v1Only: true), "split_config.de.apk")
        #expect(split.signature.check == .verified)
        #expect(try Self.checked([base, split]).signing == .verified(base.signature.signer!))
        let v1Base = try Self.member(SplitAPK.make(targetSdk: 34, v1Only: true), "base.apk")
        #expect(v1Base.signature.check.isFailure)
    }

    @Test func aSetWithoutAnySignatureAsks() throws {
        // Android refuses it; Minato asks, as for one unsigned APK (ADR-0034).
        #expect(try Self.checked([Self.base(signers: []), Self.split("config.de", signers: [])]).signing == .unsigned)
    }

    @Test func signedAndUnsignedApksMixedAreRefused() throws {
        #expect(try Self.refusal([Self.base(), Self.split("config.de", signers: [])])
            == "split_config.de.apk signatures are inconsistent")
        #expect(try Self.refusal([Self.base(signers: []), Self.split("config.de")])
            == "split_config.de.apk signatures are inconsistent")
        // An APK Minato cannot check is still signed, even one whose certificates cannot be read.
        #expect(try Self.refusal([Self.base(signers: []), Self.split("config.de", signers: [.dsa])])
            == "split_config.de.apk signatures are inconsistent")
        var anonymous = try Self.split("config.de", signers: [.dsa])
        anonymous.certificates = []
        #expect(try Self.refusal([Self.base(signers: []), anonymous]) == "split_config.de.apk signatures are inconsistent")
    }

    @Test func anApkThatCannotBeVerifiedMakesTheSetUnverified() throws {
        let set = try Self.checked([Self.base(signers: [.dsa]), Self.split("config.de", signers: [.dsa])])
        guard case .unverified = set.signing else {
            Issue.record("expected an unverified set, got \(set.signing)")
            return
        }
    }

    @Test func aDamagedSignatureRefusesTheSet() throws {
        // A byte of an entry the reader never opens: only the v2 digest notices it.
        var data = SplitAPK.make(split: "config.de", files: ["assets/data.bin": Data(repeating: 7, count: 64)])
        let archive = try ZipArchive(data: data)
        let entry = try #require(archive.entry(named: "assets/data.bin"))
        data[Int(try archive.dataOffset(of: entry))] ^= 0xFF
        let damaged = try Self.member(data, "split_config.de.apk")
        let reason = try #require(Self.refusal([Self.base(), damaged]))
        #expect(reason.hasPrefix("split_config.de.apk: "))
    }

    // MARK: - Required splits

    @Test func aBaseThatRequiresSplitsNeedsAtLeastOne() throws {
        let alone = try Self.checked([Self.base(manifestSplitRequired: true)])
        #expect(alone.missingSplit && alone.needsSplits)
        let with = try Self.checked([Self.base(manifestSplitRequired: true), Self.split("config.de")])
        #expect(!with.missingSplit && !with.needsSplits)
    }

    @Test func requiredSplitTypesMustBeCoveredByTheSetsSplitTypes() throws {
        let base = try Self.base(requiredSplitTypes: "base__abi,base__density")
        let abi = try Self.split("config.arm64_v8a", splitTypes: "base__abi")
        let density = try Self.split("config.xxhdpi", splitTypes: "base__density")
        #expect(try Self.checked([base, abi]).missingSplit)
        #expect(!(try Self.checked([base, abi, density]).missingSplit))
        // The base's own split types count too, but a base alone never satisfies the rule (size() <= 1).
        let covered = try Self.member(SplitAPK.make(requiredSplitTypes: "base__abi", splitTypes: "base__abi"), "base.apk")
        #expect(try Self.checked([covered]).missingSplit)
        #expect(!(try Self.checked([covered, Self.split("config.de")]).missingSplit))
    }

    @Test func splitTypesOnlyTheResourceIDFindsCoverNothing() throws {
        // Android reads splitTypes by name alone, so a renamed attribute gives none: INSTALL_FAILED_MISSING_SPLIT.
        let renamed = AXMLBuilder.Attribute.android("st", 0x0101_064F, .string("base__abi"))
        let split = try Self.member(SplitAPK.make(split: "config.arm64_v8a", attributes: [renamed]), "split_config.arm64_v8a.apk")
        #expect(try Self.checked([Self.base(requiredSplitTypes: "base__abi"), split]).missingSplit)
    }

    @Test func aSplitsOwnRequiredTypesCountOnceTheBaseRequiresSplits() throws {
        // Android unions the required types of every APK, but only checks them when the base requires splits.
        let feature = try Self.split("feature", requiredSplitTypes: "feature__abi", splitTypes: "base__abi")
        #expect(try Self.checked([Self.base(requiredSplitTypes: "base__abi"), feature]).missingSplit)
        #expect(!(try Self.checked([Self.base(), feature]).missingSplit))
    }

    @Test func bundletoolsOlderSignalsNeedOneSplitWithoutRefusingAnything() throws {
        // Minato is stricter than Android 15 here: "Needs split APKs", never a refusal.
        let alone = try Self.checked([Self.base(applicationSplitRequired: true)])
        #expect(!alone.missingSplit && alone.needsSplits)
        #expect(!(try Self.checked([Self.base(applicationSplitRequired: true), Self.split("config.de")]).needsSplits))
    }

    // MARK: - Joining the installed version (decision 7)

    @Test func aLoneSplitJoinsTheInstalledVersion() throws {
        // Android's inherit mode stages the installed base and splits with the new ones (:3687-3718).
        let joined = try SetChecks.joining([Self.split("config.de")], installed: [Self.base(), Self.split("config.arm64_v8a")])
        #expect(joined?.map(\.fileName) == ["base.apk", "split_config.arm64_v8a.apk", "split_config.de.apk"])
    }

    @Test func aSplitTakesThePlaceOfTheInstalledOneOfItsName() throws {
        let new = try Self.member(SplitAPK.make(split: "config.de", files: ["assets/text": Data("new".utf8)]), "de.apk")
        let joined = try SetChecks.joining([new, Self.split("config.en")],
                                           installed: [Self.base(), Self.split("config.de"), Self.split("config.fr")])
        #expect(joined?.map(\.fileName) == ["base.apk", "split_config.fr.apk", "de.apk", "split_config.en.apk"])
    }

    @Test func splitsJoinOnlyAnInstalledBaseOfTheirPackageVersionAndExactSigner() throws {
        // "Missing existing base package", "Existing … inconsistent with …", "Existing signatures are inconsistent"
        // (:3433-3437, 3678-3685): the splits then need their base (decision 3).
        let de = try Self.split("config.de")
        #expect(SetChecks.joining([de], installed: []) == nil)
        #expect(try SetChecks.joining([de], installed: [Self.split("config.arm64_v8a")]) == nil)
        #expect(try SetChecks.joining([Self.split("config.de", versionCode: 2)], installed: [Self.base()]) == nil)
        #expect(try SetChecks.joining([de], installed: [Self.base(versionCode: 2)]) == nil)
        let other = try Self.member(SplitAPK.make(package: "org.example.other", split: "config.de"), "split_config.de.apk")
        #expect(try SetChecks.joining([other], installed: [Self.base()]) == nil)
        #expect(try SetChecks.joining([Self.split("config.de", signers: [.rsa])], installed: [Self.base()]) == nil)
        #expect(try SetChecks.joining([Self.split("config.de", signers: [.ec, .rsa])], installed: [Self.base()]) == nil)
        #expect(try SetChecks.joining([Self.split("config.de", signers: [])], installed: [Self.base()]) == nil)
        #expect(try SetChecks.joining([Self.split("config.de", signers: [])], installed: [Self.base(signers: [])])?.count == 2)
        #expect(try SetChecks.joining([], installed: [Self.base()]) == nil)
    }

    @Test func everySplitMustMatchTheInstalledBase() throws {
        // Not only the first: joining promises it for each of them.
        let de = try Self.split("config.de")
        #expect(try SetChecks.joining([de, Self.split("config.en", signers: [.rsa])], installed: [Self.base()]) == nil)
        #expect(try SetChecks.joining([de, Self.split("config.en", versionCode: 2)], installed: [Self.base()]) == nil)
        let other = try Self.member(SplitAPK.make(package: "org.example.other", split: "config.en"), "split_config.en.apk")
        #expect(try SetChecks.joining([de, other], installed: [Self.base()]) == nil)
    }

    // MARK: - Isolated splits (b8c)

    static var isolatedSplits: AXMLBuilder.Attribute { .android("isolatedSplits", 0x0101_054B, .bool(true)) }

    static func base(isolated: Bool) throws -> SetMember {
        try member(SplitAPK.make(attributes: isolated ? [isolatedSplits] : []), "base.apk")
    }

    static func feature(_ name: String, usesSplit: String? = nil) throws -> SetMember {
        try member(SplitAPK.make(split: name, isFeatureSplit: true, usesSplit: usesSplit), "split_\(name).apk")
    }

    static func config(_ name: String, for target: String?) throws -> SetMember {
        try member(SplitAPK.make(split: name, configForSplit: target), "split_\(name).apk")
    }

    /// What `SplitDependencyLoader.createDependenciesFromPackage` refuses (:161-249, android-15.0.0_r1), which the
    /// package parser runs only where the base sets isolatedSplits (`ParsingPackageUtils.java:358-364`), and Android's
    /// words for it, with INSTALL_PARSE_FAILED_BAD_MANIFEST.
    enum Dependencies: CaseIterable, Sendable {
        case configForMissingFeature, usesMissingSplit, configForConfig, circle, circleOfOne

        func splits() throws -> [SetMember] {
            typealias Make = SetChecksTests
            return switch self {
            case .configForMissingFeature: [try Make.config("feature_missing.config.de", for: "feature_missing")]
            case .usesMissingSplit: [try Make.feature("feature_extra", usesSplit: "feature_missing")]
            case .configForConfig: [try Make.config("config.fr", for: nil), try Make.config("extra.config.de", for: "config.fr")]
            case .circle: [try Make.feature("feature_a", usesSplit: "feature_b"), try Make.feature("feature_b", usesSplit: "feature_a")]
            case .circleOfOne: [try Make.feature("feature_a", usesSplit: "feature_a")]
            }
        }

        var reason: String {
            switch self {
            case .configForMissingFeature: "Split 'feature_missing.config.de' targets split 'feature_missing', which is missing."
            case .usesMissingSplit: "Split 'feature_extra' requires split 'feature_missing', which is missing."
            case .configForConfig:
                "Split 'extra.config.de' declares itself as configuration split for a non-feature split 'config.fr'"
            case .circle, .circleOfOne: "Cycle detected in split dependencies."
            }
        }
    }

    @Test(arguments: Dependencies.allCases)
    func underIsolatedSplitsAndroidsDependencyErrorsAreRefused(_ dependencies: Dependencies) throws {
        #expect(try Self.refusal([Self.base(isolated: true)] + dependencies.splits()) == dependencies.reason)
    }

    @Test(arguments: Dependencies.allCases)
    func withoutIsolatedSplitsAndroidBuildsNoDependencyTree(_ dependencies: Dependencies) throws {
        #expect(try Self.refusal([Self.base(isolated: false)] + dependencies.splits()) == nil)
    }

    @Test func isolatedSplitsCountsOnTheBaseAlone() throws {
        // PackageLite.java:142.
        let split = try Self.member(SplitAPK.make(split: "feature_missing.config.de", configForSplit: "feature_missing",
                                                  attributes: [Self.isolatedSplits]), "split_feature_missing.config.de.apk")

        #expect(try Self.refusal([Self.base(isolated: false), split]) == nil)
    }

    @Test func anIsolatedSetWhoseDependenciesResolveIsAccepted() throws {
        // A <uses-split> may name any split; a configuration split ends the walk as a leaf, and one without configForSplit
        // configures the base.
        let set = try Self.checked([
            Self.base(isolated: true), Self.feature("feature_extra"), Self.config("feature_extra.config.de", for: "feature_extra"),
            Self.feature("feature_more", usesSplit: "feature_extra"), Self.config("config.de", for: nil),
            Self.feature("feature_leaf", usesSplit: "config.de"),
        ])

        #expect(set.splits.count == 5)
    }

    @Test func eachKindOfSplitHasOnlyItsOwnDependency() throws {
        // "Non-feature splits don't have dependencies", and "Feature splits are not configForSplits".
        let config = try Self.member(SplitAPK.make(split: "config.de", usesSplit: "missing"), "split_config.de.apk")
        let feature = try Self.member(SplitAPK.make(split: "feature_extra", configForSplit: "missing", isFeatureSplit: true),
                                      "split_feature_extra.apk")

        #expect(try Self.refusal([Self.base(isolated: true), config, feature]) == nil)
    }

    @Test func theFeatureSplitsComeBeforeTheConfigurationSplits() throws {
        // createDependenciesFromPackage goes through the feature splits first, then the configuration splits, each in
        // split-name order.
        let reason = try Self.refusal([Self.base(isolated: true), Self.config("a.config.de", for: "a_missing"),
                                       Self.feature("z_feature", usesSplit: "z_missing")])

        #expect(reason == "Split 'z_feature' requires split 'z_missing', which is missing.")
    }

    @Test func aMissingSplitComesFirst() throws {
        // The session refuses with INSTALL_FAILED_MISSING_SPLIT (PackageInstallerSession.java:3651-3655) before the
        // package parser builds the dependency tree.
        let base = try Self.member(SplitAPK.make(manifestSplitRequired: true, requiredSplitTypes: "base__abi",
                                                 attributes: [Self.isolatedSplits]), "base.apk")

        let set = try Self.checked([base, Self.feature("feature_extra", usesSplit: "feature_missing")])

        #expect(set.missingSplit)
    }

    @Test func aJoinedSetIsCheckedAgain() throws {
        let installed = try [Self.base(isolated: true), Self.feature("feature_extra")]

        let joined = try #require(SetChecks.joining([Self.config("feature_missing.config.de", for: "feature_missing")],
                                                    installed: installed))

        #expect(Self.refusal(joined) == "Split 'feature_missing.config.de' targets split 'feature_missing', which is missing.")
    }
}
