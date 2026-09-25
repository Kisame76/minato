import APKKit
import APKTestSupport
import Foundation
import Testing

@testable import APKLibrary

/// The rows of ADR-0034's decision 1 that depend on the version and the bytes (step a); the signer
/// comes with step b).
@Suite("Update policy")
struct UpdatePolicyTests {
    static func version(_ code: Int64, _ state: LibraryEntry.State = .ready, sha256: String? = "aa") -> AppVersion {
        AppVersion(state: state, versionCode: code, sha256: sha256)
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

        // The imported file's SHA-256, recorded for every version; before that, only a base.apk's.
        #expect(AppVersion(entry: recorded) == AppVersion(state: .needsSplits, versionCode: 5, sha256: "dd"))
        #expect(AppVersion(entry: entry) == AppVersion(state: .needsSplits, versionCode: 5, sha256: "cc"))
        #expect(AppVersion(entry: withoutCopy) == AppVersion(state: .needsSplits, versionCode: 5, sha256: nil))
        #expect(AppVersion(entry: withoutReport) == nil)
    }
}
