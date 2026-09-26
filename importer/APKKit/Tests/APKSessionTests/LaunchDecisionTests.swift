import APKLibrary
import Testing

@testable import APKSession

/// ADR-0032, decision 3 and test 2: what one tap on an app does.
@Suite("Launch decision")
struct LaunchDecisionTests {
    typealias Entry = LaunchRequestTests

    @Test func startsAReadyAppSignedWithTheCurrentCertificate() throws {
        let entry = try Entry.entry(signedWith: "aa")

        #expect(LaunchDecision(entry: entry, isUpdating: false, signer: .certificate(sha256: "aa")) == .start)
    }

    @Test(arguments: ["bb", nil] as [String?])
    func signsCodeThatAnotherCertificateOrNoneSignedFirst(signedWith: String?) throws {
        // Then it starts without another tap.
        let entry = try Entry.entry(signedWith: signedWith)

        #expect(LaunchDecision(entry: entry, isUpdating: false, signer: .certificate(sha256: "aa")) == .signFirst)
    }

    @Test func signsAdHocInTheSimulator() throws {
        #expect(LaunchDecision(entry: try Entry.entry(signedWith: nil), isUpdating: false, signer: .adHoc) == .signFirst)
        #expect(LaunchDecision(entry: try Entry.entry(signedWith: CurrentSigner.adHoc.fingerprint), isUpdating: false,
                               signer: .adHoc) == .start)
        #expect(LaunchDecision(entry: try Entry.entry(signedWith: "aa"), isUpdating: false, signer: .adHoc) == .signFirst)
    }

    @Test func opensTheSigningFlowWithoutACertificate() throws {
        #expect(LaunchDecision(entry: try Entry.entry(), isUpdating: false, signer: .none) == .openSigning)
    }

    @Test func explainsWhyOtherAppsDoNotStart() throws {
        #expect(LaunchDecision(entry: try Entry.entry(nativeActivity: false), isUpdating: false, signer: .certificate(sha256: "aa"))
            == .explain(.needsART))
        #expect(LaunchDecision(entry: try Entry.entry(), isUpdating: true, signer: .certificate(sha256: "aa")) == .explain(.updating))
        #expect(LaunchDecision(entry: try Entry.entry(state: .needsSplits), isUpdating: false, signer: .certificate(sha256: "aa"))
            == .explain(.needsSplits))
        #expect(LaunchDecision(entry: try Entry.entry(state: .incompatible), isUpdating: false, signer: .certificate(sha256: "aa"))
            == .explain(.incompatible))
        #expect(LaunchDecision(entry: try Entry.entry(state: .invalid("broken")), isUpdating: false, signer: .certificate(sha256: "aa"))
            == .explain(.invalid))
    }

    @Test func explainsBeforeAskingForACertificate() throws {
        // An app that cannot start anyway needs no certificate.
        #expect(LaunchDecision(entry: try Entry.entry(nativeActivity: false), isUpdating: false, signer: .none) == .explain(.needsART))
        #expect(LaunchDecision(entry: try Entry.entry(state: .incompatible), isUpdating: false, signer: .none) == .explain(.incompatible))
    }

    @Test func namesTheAdHocSigner() {
        #expect(CurrentSigner.certificate(sha256: "aa").fingerprint == "aa")
        #expect(CurrentSigner.adHoc.fingerprint == "ad hoc")
        #expect(CurrentSigner.none.fingerprint == nil)
    }
}
