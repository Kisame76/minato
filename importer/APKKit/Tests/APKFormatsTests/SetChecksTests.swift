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
        // Android's ext4 tells them apart, APFS does not (the lead's decision).
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
        // Signature.areExactArraysMatch: as many on both sides, and each side's in the other (App's review).
        #expect(try Self.checked([Self.base(signers: [.ec, .ec, .rsa]), Self.split("config.de", signers: [.ec, .rsa, .rsa])])
            .splits.count == 1)
        #expect(try Self.refusal([Self.base(signers: [.ec, .rsa]), Self.split("config.de", signers: [.ec, .ec, .rsa])])
            == "split_config.de.apk signatures are inconsistent")
    }

    @Test func anUnverifiableApkNamingTheBasesCertificateTwiceIsRefused() throws {
        // The lead's probe: named twice is not the base's one signer (Signature.java:331-334).
        #expect(try Self.refusal([Self.base(), Self.member(Self.namingTheBase("config.de", count: 2), "split_config.de.apk")])
            == "split_config.de.apk signatures are inconsistent")
    }

    @Test func verifiedAndUnverifiableApksTogetherAsk() throws {
        // The lead's probe: a DSA signer that names the base's certificate proves nothing about the content, so a set
        // that mixes verified and unverifiable APKs asks even on a fresh install, naming them (the lead's decision).
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
        // The lead's probe: DSA leaves the content unchecked, so only its certificate tells.
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
        // Minato is stricter than Android 15 here (F1): "Needs split APKs", never a refusal.
        let alone = try Self.checked([Self.base(applicationSplitRequired: true)])
        #expect(!alone.missingSplit && alone.needsSplits)
        #expect(!(try Self.checked([Self.base(applicationSplitRequired: true), Self.split("config.de")]).needsSplits))
    }
}
