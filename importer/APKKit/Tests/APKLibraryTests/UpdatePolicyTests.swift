import APKFormats
import APKKit
import APKTestSupport
import Foundation
import Testing

@testable import APKLibrary

/// The rows of ADR-0034's decision 1, and the same signer of decision 2.
@Suite("Update policy")
struct UpdatePolicyTests {
    static let signer = SignerIdentity(current: ["k1"])

    static func version(_ code: Int64, _ state: LibraryEntry.State = .ready, sha256: String? = "aa",
                        signer: SignerIdentity = Self.signer) -> AppVersion {
        AppVersion(state: state, versionCode: code, sha256: sha256, signer: signer)
    }

    @Test(arguments: [LibraryEntry.State.ready, .needsSplits, .incompatible])
    func installsWhenNothingIsInstalled(state: LibraryEntry.State) {
        #expect(UpdatePolicy.decide(installed: nil, new: Self.version(1, state)) == .install)
    }

    @Test(arguments: [LibraryEntry.State.incompatible, .needsSplits])
    func rejectsAnUnusableFileWhileAReadyVersionIsInstalled(state: LibraryEntry.State) {
        // Newer, the same or older: an unusable file never replaces a working version.
        for code: Int64 in [1, 2, 3] {
            #expect(UpdatePolicy.decide(installed: Self.version(2), new: Self.version(code, state, sha256: "bb")) == .reject(state))
        }
    }

    @Test(arguments: [LibraryEntry.State.ready, .needsSplits, .incompatible])
    func updatesToAHigherVersion(installed: LibraryEntry.State) {
        #expect(UpdatePolicy.decide(installed: Self.version(1, installed), new: Self.version(2, sha256: "bb")) == .update)
    }

    @Test func leavesAnIncompatibleVersionUnprotected() {
        // An installed version that cannot run anyway is updated even by a file that cannot run either.
        #expect(UpdatePolicy.decide(installed: Self.version(1, .incompatible), new: Self.version(2, .incompatible, sha256: "bb")) == .update)
    }

    @Test func rejectsAnIncompatibleFileWhileAVersionThatNeedsSplitsIsInstalled() {
        // That version is a base waiting for its split APKs: a lone split APK, or any other file that cannot run,
        // never replaces it, whatever its version (the row ADR-0034 decision 1 gains for M5's split sets).
        for code: Int64 in [1, 2, 3] {
            #expect(UpdatePolicy.decide(installed: Self.version(2, .needsSplits), new: Self.version(code, .incompatible, sha256: "bb"))
                == .reject(.incompatible))
        }
        // Before any question about the signer.
        var unproven = Self.version(2, .incompatible, sha256: "bb", signer: SignerIdentity(current: ["k2"]))
        #expect(UpdatePolicy.decide(installed: Self.version(2, .needsSplits), new: unproven) == .reject(.incompatible))
        unproven.signerProven = false
        #expect(UpdatePolicy.decide(installed: Self.version(2, .needsSplits), new: unproven) == .reject(.incompatible))
    }

    @Test(arguments: [LibraryEntry.State.ready, .needsSplits, .incompatible])
    func leavesTheSameBytesUnchanged(state: LibraryEntry.State) {
        #expect(UpdatePolicy.decide(installed: Self.version(3, state), new: Self.version(3, state)) == .unchanged)
    }

    @Test(arguments: [LibraryEntry.State.ready, .needsSplits, .incompatible])
    func installsTheSameVersionWithOtherBytesAgain(state: LibraryEntry.State) {
        #expect(UpdatePolicy.decide(installed: Self.version(3, state), new: Self.version(3, state, sha256: "bb")) == .reinstall)
    }

    @Test func installsAgainWhatItKnowsNoBytesOf() {
        // An entry from before the file's SHA-256 was recorded, without a base.apk: its bytes are unknown.
        #expect(UpdatePolicy.decide(installed: Self.version(3, .incompatible, sha256: nil), new: Self.version(3, .incompatible)) == .reinstall)
    }

    @Test(arguments: [LibraryEntry.State.ready, .needsSplits, .incompatible])
    func asksBeforeAnOlderVersion(state: LibraryEntry.State) {
        #expect(UpdatePolicy.decide(installed: Self.version(3, state), new: Self.version(2, state, sha256: "bb")) == .ask([.olderVersion]))
    }

    @Test func comparesTheWholeVersionCode() {
        // With versionCodeMajor a version code has 64 bits.
        #expect(UpdatePolicy.decide(installed: Self.version(0x1_0000_0000), new: Self.version(0xFFFF_FFFF, sha256: "bb"))
            == .ask([.olderVersion]))
        #expect(UpdatePolicy.decide(installed: Self.version(0xFFFF_FFFF), new: Self.version(0x1_0000_0000, sha256: "bb")) == .update)
    }

    // MARK: - The signer (step b)

    @Test(arguments: [LibraryEntry.State.ready, .needsSplits, .incompatible])
    func asksBeforeAnotherSigner(state: LibraryEntry.State) {
        let other = SignerIdentity(current: ["k2"])
        #expect(UpdatePolicy.decide(installed: Self.version(1, state), new: Self.version(2, state, sha256: "bb", signer: other))
            == .ask([.otherSigner]))
        #expect(UpdatePolicy.decide(installed: Self.version(1, state), new: Self.version(1, state, sha256: "bb", signer: other))
            == .ask([.otherSigner]))
        // Both warnings for another signer's older version.
        #expect(UpdatePolicy.decide(installed: Self.version(3, state), new: Self.version(2, state, sha256: "bb", signer: other))
            == .ask([.otherSigner, .olderVersion]))
    }

    @Test func rejectsAnUnusableFileOfAnotherSignerToo() {
        let other = SignerIdentity(current: ["k2"])
        #expect(UpdatePolicy.decide(installed: Self.version(1), new: Self.version(2, .incompatible, sha256: "bb", signer: other))
            == .reject(.incompatible))
    }

    @Test(arguments: [LibraryEntry.State.ready, .needsSplits, .incompatible])
    func rejectsAFileWhoseCheckFailedWhateverIsInstalled(installed: LibraryEntry.State) {
        // A damaged or forged file never replaces an app, not even one that cannot run.
        var new = Self.version(2, .incompatible, sha256: "bb")
        new.signatureFailed = true
        #expect(UpdatePolicy.decide(installed: Self.version(1, installed), new: new) == .reject(.incompatible))
    }

    @Test func asksWhenEitherOneIsUnsigned() {
        let unsigned = SignerIdentity(current: [])
        #expect(UpdatePolicy.decide(installed: Self.version(1), new: Self.version(2, sha256: "bb", signer: unsigned)) == .ask([.otherSigner]))
        #expect(UpdatePolicy.decide(installed: Self.version(1, signer: unsigned), new: Self.version(2, sha256: "bb")) == .ask([.otherSigner]))
        #expect(UpdatePolicy.decide(installed: Self.version(1, signer: unsigned), new: Self.version(2, sha256: "bb", signer: unsigned))
            == .ask([.otherSigner]))
    }

    static func node(_ sha256: String, _ flags: UInt32 = TestSigner.defaultCapabilities) -> LineageNode {
        LineageNode(sha256: sha256, flags: flags)
    }

    @Test func aRotatedKeyIsTheSameSigner() {
        let rotated = SignerIdentity(current: ["k2"], lineage: [Self.node("k1")])
        #expect(UpdatePolicy.decide(installed: Self.version(1), new: Self.version(2, sha256: "bb", signer: rotated)) == .update)
        // Rotated twice since the installed version.
        let twice = SignerIdentity(current: ["k3"], lineage: [Self.node("k1"), Self.node("k2")])
        #expect(UpdatePolicy.decide(installed: Self.version(1), new: Self.version(2, sha256: "bb", signer: twice)) == .update)
        // Both carry the lineage.
        #expect(UpdatePolicy.decide(installed: Self.version(1, signer: rotated), new: Self.version(2, sha256: "bb", signer: twice)) == .update)
    }

    @Test func aRotatedKeyNeedsTheInstalledKeysInstalledDataCapability() {
        // Android's checkCapability(INSTALLED_DATA): the old key no longer vouches for its apps' data.
        let revoked = SignerIdentity(current: ["k2"], lineage: [Self.node("k1", TestSigner.defaultCapabilities & ~TestSigner.installedData)])
        #expect(UpdatePolicy.decide(installed: Self.version(1), new: Self.version(2, sha256: "bb", signer: revoked)) == .ask([.otherSigner]))
        // A report from step b) knows no capabilities; the library reads them from the base.apk first.
        let unknown = SignerIdentity(current: ["k2"], lineage: [LineageNode(sha256: "k1", flags: nil)])
        #expect(UpdatePolicy.decide(installed: Self.version(1), new: Self.version(2, sha256: "bb", signer: unknown)) == .ask([.otherSigner]))
    }

    @Test func aKeyBackToAnOlderOneNeedsTheRollbackCapability() {
        // The installed app's lineage decides, as old.checkCapability(new, ROLLBACK) on Android.
        let rotated = SignerIdentity(current: ["k2"], lineage: [Self.node("k1")])
        #expect(UpdatePolicy.decide(installed: Self.version(1, signer: rotated), new: Self.version(2, sha256: "bb")) == .ask([.otherSigner]))
        let mayRollBack = SignerIdentity(current: ["k2"], lineage: [Self.node("k1", TestSigner.defaultCapabilities | TestSigner.rollback)])
        #expect(UpdatePolicy.decide(installed: Self.version(1, signer: mayRollBack), new: Self.version(2, sha256: "bb")) == .update)
        // The new file's own lineage cannot grant itself the way back: its ROLLBACK counts for nothing.
        let claims = SignerIdentity(current: ["k1"], lineage: [Self.node("k2", TestSigner.rollback)])
        #expect(UpdatePolicy.decide(installed: Self.version(1, signer: rotated), new: Self.version(2, sha256: "bb", signer: claims))
            == .ask([.otherSigner]))
    }

    @Test func aLineageWithoutTheInstalledKeyIsAnotherSigner() {
        let foreign = SignerIdentity(current: ["k2"], lineage: [Self.node("k9")])
        #expect(UpdatePolicy.decide(installed: Self.version(1), new: Self.version(2, sha256: "bb", signer: foreign)) == .ask([.otherSigner]))
        // Sharing only an older key is not enough: the one's current key must be in the other's lineage.
        let sibling = SignerIdentity(current: ["k3"], lineage: [Self.node("k1")])
        #expect(UpdatePolicy.decide(installed: Self.version(1, signer: SignerIdentity(current: ["k2"], lineage: [Self.node("k1")])),
                                    new: Self.version(2, sha256: "bb", signer: sibling)) == .ask([.otherSigner]))
    }

    @Test func anUnprovenInstalledSignerAsks() {
        // A forged file installed fresh names anyone; the real app must not update it silently.
        var installed = Self.version(1)
        installed.signerProven = false
        #expect(UpdatePolicy.decide(installed: installed, new: Self.version(2, sha256: "bb")) == .ask([.otherSigner]))
    }

    @Test func anUnprovenSignerAsks() {
        // Signed with an algorithm the check cannot verify (ADR-0034, step d): the same certificate proves nothing.
        var unproven = Self.version(2, sha256: "bb")
        unproven.signerProven = false
        #expect(UpdatePolicy.decide(installed: Self.version(1), new: unproven) == .ask([.otherSigner]))
        unproven.versionCode = 1
        #expect(UpdatePolicy.decide(installed: Self.version(2), new: unproven) == .ask([.otherSigner, .olderVersion]))
    }

    @Test func severalSignersMustAllMatch() {
        let both = SignerIdentity(current: ["k1", "k2"])
        #expect(UpdatePolicy.decide(installed: Self.version(1, signer: both), new: Self.version(2, sha256: "bb", signer: both)) == .update)
        #expect(UpdatePolicy.decide(installed: Self.version(1, signer: both), new: Self.version(2, sha256: "bb")) == .ask([.otherSigner]))
        #expect(UpdatePolicy.decide(installed: Self.version(1), new: Self.version(2, sha256: "bb", signer: both)) == .ask([.otherSigner]))
        // None of several may rotate: a lineage counts only for one signer on each side.
        let rotatedInto = SignerIdentity(current: ["k3"], lineage: [Self.node("k1")])
        #expect(UpdatePolicy.decide(installed: Self.version(1, signer: both), new: Self.version(2, sha256: "bb", signer: rotatedInto))
            == .ask([.otherSigner]))
    }

    @Test func readsTheInstalledVersionFromItsEntry() throws {
        let report = try APKAnalyzer.analyze(ZipArchive(data: SampleAPK.make(versionCode: 5)), preferredLanguages: ["en"])
        let entry = LibraryEntry(
            id: report.packageName, displayName: report.label, state: .needsSplits, report: report,
            base: PackageFile(fileName: "base.apk", splitName: nil, size: 1, sha256: "cc"), splits: [], obbs: [],
            source: SourceFile(fileName: "game.apk", size: 1, modifiedMilliseconds: 0), iconFile: nil, importedAt: Date())
        var withoutCopy = entry
        withoutCopy.base = nil
        var recorded = withoutCopy
        recorded.source.sha256 = "dd"
        var withoutReport = entry
        withoutReport.report = nil

        let signer = SignerIdentity(current: [TestCertificates.signerSHA256])
        // The imported file's SHA-256, recorded for every version; before that, only a base.apk's.
        #expect(AppVersion(entry: recorded) == AppVersion(state: .needsSplits, versionCode: 5, sha256: "dd", signer: signer))
        #expect(AppVersion(entry: entry) == AppVersion(state: .needsSplits, versionCode: 5, sha256: "cc", signer: signer))
        #expect(AppVersion(entry: withoutCopy) == AppVersion(state: .needsSplits, versionCode: 5, sha256: nil, signer: signer))
        #expect(AppVersion(entry: withoutReport) == nil)
        // A report from before the signer: every certificate it lists counts as current. The library
        // reads the signer from the base.apk instead where it can.
        var beforeTheSigner = recorded
        beforeTheSigner.report?.signature.signer = nil
        beforeTheSigner.report?.signature.certificates.append(SigningCertificate(der: TestCertificates.rotatedSigner))
        #expect(AppVersion(entry: beforeTheSigner)?.signer
            == SignerIdentity(current: [TestCertificates.signerSHA256, TestCertificates.rotatedSignerSHA256].sorted()))
    }

    // MARK: - Sets (ADR-0048 b3)

    static func member(_ data: Data, _ fileName: String) throws -> SetMember {
        try SetMember.read(ZipArchive(data: data), fileName: fileName, sha256: fileName)
    }

    /// A checked set of a base by `.ec` and one split, `split`.
    static func set(base: Data = SplitAPK.make(), split: Data) throws -> CheckedSet {
        try SetChecks.check([member(base, "base.apk"), member(split, "split_feature.apk")]).get()
    }

    @Test func aSetWithFilesMinatoCannotCheckAsksAlsoOnAFreshInstall() throws {
        // A DSA signer naming the base's certificate proves nothing about its APK's content, so the set asks and
        // names it, also where nothing is installed (the lead's decision); its signer is the verified APKs', proven.
        let partly = try Self.set(split: SplitAPK.make(split: "feature", v2: [TestSigner.Signer(.dsa, certificate: TestKey.ec.certificate)]))
        let new = AppVersion(set: partly, state: .ready, sha256: "set")
        #expect(new == AppVersion(state: .ready, versionCode: 1, sha256: "set", signer: SignerIdentity(current: [TestKey.ec.sha256]),
                                  uncheckedFiles: ["split_feature.apk"]))
        #expect(UpdatePolicy.decide(installed: nil, new: new) == .ask([.uncheckedContent]))
        #expect(UpdatePolicy.decide(installed: Self.version(0, signer: new.signer), new: new) == .ask([.uncheckedContent]))
        #expect(UpdatePolicy.decide(installed: Self.version(0), new: new) == .ask([.uncheckedContent, .otherSigner]))
    }

    @Test func aSetOfAPKsMinatoCannotCheckIsOneSuchAPK() throws {
        // The certificates its APKs name, unproven (ADR-0034).
        let unverified = try Self.set(base: SplitAPK.make(signers: [.dsa]), split: SplitAPK.make(split: "feature", signers: [.dsa]))
        let new = AppVersion(set: unverified, state: .ready, sha256: "set")
        #expect(new == AppVersion(state: .ready, versionCode: 1, sha256: "set", signer: SignerIdentity(current: [TestKey.dsa.sha256]),
                                  signerProven: false))
        #expect(UpdatePolicy.decide(installed: nil, new: new) == .install)
        #expect(UpdatePolicy.decide(installed: Self.version(0, signer: new.signer), new: new) == .ask([.otherSigner]))
    }

    @Test func aVerifiedOrUnsignedSetIsDecidedAsOneSuchAPK() throws {
        let verified = AppVersion(set: try Self.set(split: SplitAPK.make(split: "feature")), state: .ready, sha256: "set")
        #expect(verified == AppVersion(state: .ready, versionCode: 1, sha256: "set", signer: SignerIdentity(current: [TestKey.ec.sha256])))
        #expect(UpdatePolicy.decide(installed: nil, new: verified) == .install)
        #expect(UpdatePolicy.decide(installed: Self.version(0, signer: verified.signer), new: verified) == .update)

        let unsigned = try Self.set(base: SplitAPK.make(signers: []), split: SplitAPK.make(split: "feature", signers: []))
        let bare = AppVersion(set: unsigned, state: .ready, sha256: "set")
        #expect(bare == AppVersion(state: .ready, versionCode: 1, sha256: "set", signer: SignerIdentity(current: []), signerProven: false))
        #expect(UpdatePolicy.decide(installed: nil, new: bare) == .install)
        #expect(UpdatePolicy.decide(installed: Self.version(0), new: bare) == .ask([.otherSigner]))
    }
}
