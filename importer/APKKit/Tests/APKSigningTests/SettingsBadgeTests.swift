import Foundation
import Testing

@testable import APKSigning

/// The Settings tab's badge (docs/plans/2026-09-24-app-navigation-design.md, section 4): only when a step by the
/// user is needed; otherwise signing stays invisible (ADR-0023).
struct SettingsBadgeTests {
    static let certificate = CertificateInfo(sha256: "aa", serialNumber: "0A", team: "ABCDE12345", commonName: "C",
                                             notAfter: Date(timeIntervalSince1970: 2_000_000_000))

    @Test func nothingToDoIsNoBadge() {
        #expect(!SigningState.notNeeded.needsUser(next: .none))
        #expect(!SigningState.valid(Self.certificate).needsUser(next: .none))
        // The automation is at work: importing, a store's export it opens by itself, signing again.
        #expect(!SigningState.missing.needsUser(next: .importEmbedded))
        #expect(!SigningState.missing.needsUser(next: .requestExport(.sideStore, automatically: true)))
        #expect(!SigningState.changed(old: Self.certificate, new: Self.certificate)
            .needsUser(next: .requestExport(.altStore, automatically: true)))
        #expect(!SigningState.valid(Self.certificate).needsUser(next: .resign(count: 2)))
        #expect(!SigningState.valid(Self.certificate).needsUser(next: .runSelfTest))
    }

    @Test func aStepByTheUserIsABadge() {
        // The setup put off with Not Now: the store's export waits for a tap.
        #expect(SigningState.missing.needsUser(next: .requestExport(.sideStore, automatically: false)))
        #expect(SigningState.missing.needsUser(next: .offerFileImport))
        #expect(SigningState.expired(Self.certificate).needsUser(next: .none))
        #expect(SigningState.selfTestFailed(Self.certificate, reason: "rejected").needsUser(next: .none))
        #expect(SigningState.ownCertificateUnreadable("no leaf").needsUser(next: .none))
    }
}
