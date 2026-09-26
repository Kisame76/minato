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

    @Test(arguments: [LibraryEntry.State.needsSplits, .incompatible])
    func protectsOnlyAReadyVersion(installed: LibraryEntry.State) {
        // An installed version that cannot run anyway is updated even by a file that cannot run either.
        #expect(UpdatePolicy.decide(installed: Self.version(1, installed), new: Self.version(2, .incompatible, sha256: "bb")) == .update)
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

    @Test func asksWhenEitherOneIsUnsigned() {
        let unsigned = SignerIdentity(current: [])
        #expect(UpdatePolicy.decide(installed: Self.version(1), new: Self.version(2, sha256: "bb", signer: unsigned)) == .ask([.otherSigner]))
        #expect(UpdatePolicy.decide(installed: Self.version(1, signer: unsigned), new: Self.version(2, sha256: "bb")) == .ask([.otherSigner]))
        #expect(UpdatePolicy.decide(installed: Self.version(1, signer: unsigned), new: Self.version(2, sha256: "bb", signer: unsigned))
            == .ask([.otherSigner]))
    }

    @Test func aRotatedKeyIsTheSameSigner() {
        let rotated = SignerIdentity(current: ["k2"], lineage: ["k1"])
        #expect(UpdatePolicy.decide(installed: Self.version(1), new: Self.version(2, sha256: "bb", signer: rotated)) == .update)
        // Rotated twice since the installed version.
        let twice = SignerIdentity(current: ["k3"], lineage: ["k1", "k2"])
        #expect(UpdatePolicy.decide(installed: Self.version(1), new: Self.version(2, sha256: "bb", signer: twice)) == .update)
        // Both carry the lineage.
        #expect(UpdatePolicy.decide(installed: Self.version(1, signer: rotated), new: Self.version(2, sha256: "bb", signer: twice)) == .update)
    }

    @Test func aKeyBackToAnOlderOneIsTheSameSigner() {
        let rotated = SignerIdentity(current: ["k2"], lineage: ["k1"])
        #expect(UpdatePolicy.decide(installed: Self.version(1, signer: rotated), new: Self.version(2, sha256: "bb")) == .update)
    }

    @Test func aLineageWithoutTheInstalledKeyIsAnotherSigner() {
        let foreign = SignerIdentity(current: ["k2"], lineage: ["k9"])
        #expect(UpdatePolicy.decide(installed: Self.version(1), new: Self.version(2, sha256: "bb", signer: foreign)) == .ask([.otherSigner]))
        // Sharing only an older key is not enough: the one's current key must be in the other's lineage.
        let sibling = SignerIdentity(current: ["k3"], lineage: ["k1"])
        #expect(UpdatePolicy.decide(installed: Self.version(1, signer: SignerIdentity(current: ["k2"], lineage: ["k1"])),
                                    new: Self.version(2, sha256: "bb", signer: sibling)) == .ask([.otherSigner]))
    }

    @Test func severalSignersMustAllMatch() {
        let both = SignerIdentity(current: ["k1", "k2"])
        #expect(UpdatePolicy.decide(installed: Self.version(1, signer: both), new: Self.version(2, sha256: "bb", signer: both)) == .update)
        #expect(UpdatePolicy.decide(installed: Self.version(1, signer: both), new: Self.version(2, sha256: "bb")) == .ask([.otherSigner]))
        #expect(UpdatePolicy.decide(installed: Self.version(1), new: Self.version(2, sha256: "bb", signer: both)) == .ask([.otherSigner]))
        // None of several may rotate: a lineage counts only for one signer on each side.
        let rotatedInto = SignerIdentity(current: ["k3"], lineage: ["k1"])
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
}
