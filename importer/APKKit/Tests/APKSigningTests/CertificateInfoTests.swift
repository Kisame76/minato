import Foundation
import Testing

@testable import APKSigning

@Suite("Certificate comparison")
struct CertificateInfoTests {
    static let expiry = Date(timeIntervalSince1970: 1_821_571_200)  // 2027-09-22

    static func info(_ sha256: String, team: String = "ABCDE12345") -> CertificateInfo {
        CertificateInfo(sha256: sha256, serialNumber: "0A1B", team: team, commonName: "Apple Development: Test", notAfter: expiry)
    }

    @Test func fingerprintIsTheSHA256OfTheDER() {
        let info = CertificateInfo(der: Data("abc".utf8), serialNumber: "01", team: "T", commonName: "C", notAfter: Self.expiry)
        #expect(info.sha256 == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
    }

    @Test func adHocAppNeedsNoCertificate() {
        #expect(compare(imported: nil, own: nil) == .adHoc)
        #expect(compare(imported: Self.info("aa"), own: nil) == .adHoc)
    }

    @Test func nothingImported() {
        #expect(compare(imported: nil, own: Self.info("aa")) == .none)
    }

    @Test func sameFingerprintIsTheSameCertificate() {
        #expect(compare(imported: Self.info("aa"), own: Self.info("aa")) == .same)
    }

    @Test func otherFingerprintOfTheSameTeamIsARenewal() {
        #expect(compare(imported: Self.info("aa"), own: Self.info("bb")) == .renewed)
    }

    @Test func otherTeam() {
        #expect(compare(imported: Self.info("aa", team: "OTHERTEAM1"), own: Self.info("bb")) == .otherTeam)
    }
}
