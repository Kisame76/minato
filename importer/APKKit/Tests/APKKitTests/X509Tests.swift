import APKTestSupport
import Foundation
import Security
import Testing

import APKKit

@Suite("X509")
struct X509Tests {
    @Test func cutsTheSubjectPublicKeyInfoOutOfTheCertificate() throws {
        let spki = try X509.subjectPublicKeyInfo(of: TestCertificates.signer)

        // SEQUENCE (0x59 bytes) { SEQUENCE { id-ecPublicKey, prime256v1 }, BIT STRING (0x42 bytes) }, RFC 5480.
        let algorithm: [UInt8] = [0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01,
                                  0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00]
        #expect(Array(spki.prefix(algorithm.count)) == algorithm)
        #expect(spki.count == 0x5B)
        // The point Security.framework reads from the same certificate: 04 || X || Y.
        let certificate = try #require(SecCertificateCreateWithData(nil, TestCertificates.signer as CFData))
        let key = try #require(SecCertificateCopyKey(certificate))
        let point = try #require(SecKeyCopyExternalRepresentation(key, nil) as Data?)
        #expect(spki.suffix(point.count) == point)
        #expect(TestCertificates.signer.range(of: spki) != nil)
    }

    @Test(arguments: [Data(), Data([0x30, 0x03, 0x02, 0x01, 0x01]), Data("not a certificate".utf8), TestCertificates.signer.prefix(100)])
    func refusesWhatIsNotACertificate(data: Data) {
        #expect(throws: APKError.self) { try X509.subjectPublicKeyInfo(of: data) }
    }
}
