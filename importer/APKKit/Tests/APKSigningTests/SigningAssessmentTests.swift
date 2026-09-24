import Foundation
import Testing

@testable import APKSigning

@Suite("Signing state machine")
struct SigningAssessmentTests {
    static let now = Date(timeIntervalSince1970: 1_790_000_000)
    static let own = CertificateInfo(sha256: "aa", serialNumber: "0A", team: "ABCDE12345", commonName: "C", notAfter: now.addingTimeInterval(86_400))
    static let older = CertificateInfo(sha256: "bb", serialNumber: "0B", team: "ABCDE12345", commonName: "C", notAfter: now.addingTimeInterval(86_400))
    static let passed = SelfTestRecord(sha256: "aa", passed: true, message: "42", date: now)

    /// The app signed with `own`, which is imported and has passed its self-test, unless `change` says otherwise.
    static func inputs(imported: CertificateInfo? = own, _ change: (inout SigningInputs) -> Void = { _ in }) -> SigningInputs {
        var inputs = SigningInputs(own: own, imported: imported, now: now)
        inputs.selfTest = passed
        change(&inputs)
        return inputs
    }

    @Test func adHocAppNeedsNothing() {
        let result = assess(SigningInputs(own: nil, imported: Self.older, now: Self.now))
        #expect(result.state == .notNeeded)
        #expect(result.next == .none)
    }

    @Test func unreadableOwnCertificateIsReportedNotTakenForAdHoc() {
        var inputs = SigningInputs(own: nil, imported: Self.older, now: Self.now)
        inputs.ownCertificateProblem = "the signer is named by its key identifier"
        inputs.hasEmbeddedCertificate = true
        let result = assess(inputs)
        #expect(result.state == .ownCertificateUnreadable("the signer is named by its key identifier"))
        #expect(result.next == .none)
    }

    @Test func validWithPassedSelfTest() {
        let result = assess(Self.inputs())
        #expect(result.state == .valid(Self.own))
        #expect(result.next == .none)
    }

    @Test func missingTakesTheEmbeddedFileFirst() {
        let result = assess(Self.inputs(imported: nil) {
            $0.hasEmbeddedCertificate = true
            $0.installer = .store(.sideStore)
            $0.reachableStores = [.sideStore]
        })
        #expect(result.state == .missing)
        #expect(result.next == .importEmbedded)
    }

    @Test func missingAsksTheInstallingStoreByItselfOncePerLaunch() {
        let first = assess(Self.inputs(imported: nil) {
            $0.installer = .store(.altStore)
            $0.reachableStores = [.altStore]
        })
        #expect(first.next == .requestExport(.altStore, automatically: true))
        let again = assess(Self.inputs(imported: nil) {
            $0.installer = .store(.altStore)
            $0.reachableStores = [.altStore]
            $0.openedStoreThisLaunch = true
        })
        #expect(again.next == .requestExport(.altStore, automatically: false))
    }

    @Test func failedEmbeddedFileFallsBackToTheExport() {
        let result = assess(Self.inputs(imported: nil) {
            $0.hasEmbeddedCertificate = true
            $0.embeddedCertificateFailed = true
            $0.installer = .store(.sideStore)
            $0.reachableStores = [.sideStore]
        })
        #expect(result.next == .requestExport(.sideStore, automatically: true))
    }

    @Test func anotherStoresCertificateWouldNotFit() {
        // Installed by AltStore, which is gone; SideStore's certificate is not in the app's profile.
        let result = assess(Self.inputs(imported: nil) {
            $0.installer = .store(.altStore)
            $0.reachableStores = [.sideStore]
        })
        #expect(result.next == .offerFileImport)
    }

    @Test func xcodeBuildWithoutHandoverOffersTheFile() {
        #expect(assess(Self.inputs(imported: nil)).next == .offerFileImport)
    }

    @Test func changedCertificateIsFetchedAgain() {
        let result = assess(Self.inputs(imported: Self.older) { $0.hasEmbeddedCertificate = true })
        #expect(result.state == .changed(old: Self.older, new: Self.own))
        #expect(result.next == .importEmbedded)
    }

    @Test func markedStaleCountsAsChanged() {
        let result = assess(Self.inputs {
            $0.markedStale = true
            $0.installer = .store(.altStore)
            $0.reachableStores = [.altStore]
        })
        #expect(result.state == .changed(old: Self.own, new: Self.own))
        #expect(result.next == .requestExport(.altStore, automatically: true))
    }

    @Test func expiredCertificate() {
        var expired = Self.own
        expired.notAfter = Self.now.addingTimeInterval(-1)
        var inputs = SigningInputs(own: expired, imported: expired, now: Self.now)
        inputs.selfTest = Self.passed
        let result = assess(inputs)
        #expect(result.state == .expired(expired))
        #expect(result.next == .none)
    }

    @Test func certificateOfAnotherImportRunsTheSelfTest() {
        let result = assess(Self.inputs { $0.selfTest = SelfTestRecord(sha256: "bb", passed: true, message: "42", date: Self.now) })
        #expect(result.state == .valid(Self.own))
        #expect(result.next == .runSelfTest)
    }

    @Test func neverTestedRunsTheSelfTest() {
        #expect(assess(Self.inputs { $0.selfTest = nil }).next == .runSelfTest)
    }

    @Test func failedSelfTest() {
        let result = assess(Self.inputs {
            $0.selfTest = SelfTestRecord(sha256: "aa", passed: false, message: "code signature invalid", date: Self.now)
        })
        #expect(result.state == .selfTestFailed(Self.own, reason: "code signature invalid"))
        #expect(result.next == .none)
    }

    @Test func pendingResignResumes() {
        let result = assess(Self.inputs { $0.pendingResign = 3 })
        #expect(result.state == .valid(Self.own))
        #expect(result.next == .resign(count: 3))
    }
}
