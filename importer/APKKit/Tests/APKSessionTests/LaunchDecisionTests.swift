import APKLibrary
import Testing

@testable import APKSession

/// ADR-0032, decision 3 and test 2: what one tap on an app does.
@Suite("Launch decision")
struct LaunchDecisionTests {
    typealias Entry = LaunchRequestTests
    /// The format version of the containers the converter writes now, as APK_CONTAINER_VERSION is; the cases name the
    /// versions around it, so that they hold for any value (C5 makes it 4).
    static let current: UInt32 = 3

    /// A container of an older format, or one signed before the version was recorded, is converted again from its
    /// originals and signed, then it starts without another tap; so is one of a newer format, which an older Minato
    /// cannot be sure to read.
    @Test(arguments: [current - 1, nil, current + 1] as [UInt32?])
    func convertsAContainerOfAnotherFormatAgainFirst(version: UInt32?) throws {
        let entry = try Entry.entry(signedWith: "aa", containerVersion: version)

        #expect(LaunchDecision(entry: entry, isUpdating: false, signer: .certificate(sha256: "aa"),
                               containerVersion: Self.current) == .signFirst)
        #expect(LaunchDecision(entry: entry, isUpdating: false, signer: .adHoc, containerVersion: Self.current)
            == .signFirst)
    }

    @Test func startsAReadyAppSignedWithTheCurrentCertificate() throws {
        let entry = try Entry.entry(signedWith: "aa")

        #expect(LaunchDecision(entry: entry, isUpdating: false, signer: .certificate(sha256: "aa"), containerVersion: Self.current) == .start)
    }

    @Test(arguments: ["bb", nil] as [String?])
    func signsCodeThatAnotherCertificateOrNoneSignedFirst(signedWith: String?) throws {
        // Then it starts without another tap.
        let entry = try Entry.entry(signedWith: signedWith)

        #expect(LaunchDecision(entry: entry, isUpdating: false, signer: .certificate(sha256: "aa"), containerVersion: Self.current) == .signFirst)
    }

    @Test func signsAdHocInTheSimulator() throws {
        #expect(LaunchDecision(entry: try Entry.entry(signedWith: nil), isUpdating: false, signer: .adHoc, containerVersion: Self.current) == .signFirst)
        #expect(LaunchDecision(entry: try Entry.entry(signedWith: CurrentSigner.adHoc.fingerprint), isUpdating: false,
                               signer: .adHoc, containerVersion: Self.current) == .start)
        #expect(LaunchDecision(entry: try Entry.entry(signedWith: "aa"), isUpdating: false, signer: .adHoc, containerVersion: Self.current) == .signFirst)
    }

    @Test func opensTheSigningFlowWithoutACertificate() throws {
        #expect(LaunchDecision(entry: try Entry.entry(), isUpdating: false, signer: .none, containerVersion: Self.current) == .openSigning)
    }

    @Test func waitsForTheFirstLookAtTheCertificateBeforeAskingForOne() throws {
        // A tap right after launch, before the first look ended: that look may still bring a signer
        // without a tap (SideStore's embedded certificate), so the tap waits and is decided again.
        #expect(LaunchDecision(entry: try Entry.entry(), isUpdating: false, signer: .pending, containerVersion: Self.current) == .waitForSigner)
        #expect(LaunchDecision(entry: try Entry.entry(signedWith: "aa"), isUpdating: false, signer: .pending, containerVersion: Self.current) == .waitForSigner)
    }

    @Test func explainsWithoutWaitingForTheFirstLook() throws {
        #expect(LaunchDecision(entry: try Entry.entry(), isUpdating: true, signer: .pending, containerVersion: Self.current) == .explain(.updating))
        #expect(LaunchDecision(entry: try Entry.entry(state: .incompatible), isUpdating: false, signer: .pending, containerVersion: Self.current) == .explain(.incompatible))
    }

    /// ADR-0039 c5: a ready app with Java code starts on ART as a NativeActivity app starts, once the current signer
    /// signed its JNI libraries, also when it has none.
    @Test func aReadyAppWithJavaCodeStartsAsANativeActivityAppStarts() throws {
        let java = { (signedWith: String?) in try Entry.entry(nativeActivity: false, signedWith: signedWith) }
        #expect(LaunchDecision(entry: try java("aa"), isUpdating: false, signer: .certificate(sha256: "aa"), containerVersion: Self.current) == .start)
        #expect(LaunchDecision(entry: try java(nil), isUpdating: false, signer: .certificate(sha256: "aa"), containerVersion: Self.current) == .signFirst)
        #expect(LaunchDecision(entry: try java(nil), isUpdating: false, signer: .adHoc, containerVersion: Self.current) == .signFirst)
        #expect(LaunchDecision(entry: try java("aa"), isUpdating: false, signer: .pending, containerVersion: Self.current) == .waitForSigner)
        #expect(LaunchDecision(entry: try java("aa"), isUpdating: false, signer: .none, containerVersion: Self.current) == .openSigning)
        #expect(LaunchDecision(entry: try java("aa"), isUpdating: true, signer: .certificate(sha256: "aa"), containerVersion: Self.current) == .explain(.updating))
    }

    @Test func explainsWhyOtherAppsDoNotStart() throws {
        #expect(LaunchDecision(entry: try Entry.entry(), isUpdating: true, signer: .certificate(sha256: "aa"), containerVersion: Self.current) == .explain(.updating))
        #expect(LaunchDecision(entry: try Entry.entry(state: .needsSplits), isUpdating: false, signer: .certificate(sha256: "aa"), containerVersion: Self.current)
            == .explain(.needsSplits))
        #expect(LaunchDecision(entry: try Entry.entry(state: .incompatible), isUpdating: false, signer: .certificate(sha256: "aa"), containerVersion: Self.current)
            == .explain(.incompatible))
        #expect(LaunchDecision(entry: try Entry.entry(state: .invalid("broken")), isUpdating: false, signer: .certificate(sha256: "aa"), containerVersion: Self.current)
            == .explain(.invalid))
    }

    @Test func explainsBeforeAskingForACertificate() throws {
        // An app that cannot start anyway needs no certificate.
        #expect(LaunchDecision(entry: try Entry.entry(state: .incompatible), isUpdating: false, signer: .none, containerVersion: Self.current) == .explain(.incompatible))
        #expect(LaunchDecision(entry: try Entry.entry(state: .needsSplits), isUpdating: false, signer: .none, containerVersion: Self.current) == .explain(.needsSplits))
    }

    @Test func namesTheAdHocSigner() {
        #expect(CurrentSigner.certificate(sha256: "aa").fingerprint == "aa")
        #expect(CurrentSigner.adHoc.fingerprint == "ad hoc")
        #expect(CurrentSigner.none.fingerprint == nil)
        #expect(CurrentSigner.pending.fingerprint == nil)
    }
}
