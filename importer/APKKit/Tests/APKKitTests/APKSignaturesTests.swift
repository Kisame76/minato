import APKTestSupport
import Foundation
import Testing

@testable import APKKit

@Suite("APKSignatures")
struct APKSignaturesTests {
    static let googlePlayStampSHA256 = "3257d599a49d2c961a471ca9843f59d341a405884583fc087df4237b733bbd6d"

    static func apk(block: Data? = nil, v1: Data? = nil) throws -> ZipArchive {
        var zip = ZipBuilder()
        zip.add("AndroidManifest.xml", "manifest")
        if let v1 {
            zip.add("META-INF/MANIFEST.MF", "Manifest-Version: 1.0\n")
            zip.add("META-INF/CERT.SF", "Signature-Version: 1.0\n")
            zip.add("META-INF/CERT.RSA", v1, method: .stored)
        }
        if let block { zip.beforeCentralDirectory = block }
        return try ZipArchive(data: zip.build().data)
    }

    @Test func readsV2Certificate() throws {
        let block = SigningBlockBuilder.block([
            (SigningBlockBuilder.v2ID, SigningBlockBuilder.schemeValue([SigningBlockBuilder.v2Signer(certificates: [TestCertificates.signer])])),
        ])
        let info = APKSignatures.read(try Self.apk(block: block))

        #expect(info.schemes == [.v2])
        #expect(info.certificates.map(\.sha256) == [TestCertificates.signerSHA256])
        #expect(info.certificates.first?.subject?.contains("APKKit Test Signer") == true)
    }

    @Test func readsV1CertificateFromPKCS7() throws {
        let info = APKSignatures.read(try Self.apk(v1: TestCertificates.signerPKCS7))

        #expect(info.schemes == [.v1])
        #expect(info.certificates.map(\.sha256) == [TestCertificates.signerSHA256])
    }

    @Test func listsNewestSchemeSignerFirstAndDeduplicates() throws {
        // Key rotation: v1/v2 carry the old key, v3 the rotated one.
        let block = SigningBlockBuilder.block([
            (SigningBlockBuilder.v2ID, SigningBlockBuilder.schemeValue([SigningBlockBuilder.v2Signer(certificates: [TestCertificates.signer])])),
            (SigningBlockBuilder.v3ID, SigningBlockBuilder.schemeValue([SigningBlockBuilder.v3Signer(certificates: [TestCertificates.rotatedSigner])])),
        ])
        let info = APKSignatures.read(try Self.apk(block: block, v1: TestCertificates.signerPKCS7))

        #expect(info.schemes == [.v1, .v2, .v3])
        #expect(info.certificates.map(\.sha256) == [TestCertificates.rotatedSignerSHA256, TestCertificates.signerSHA256])
    }

    @Test func recognisesV31Block() throws {
        let block = SigningBlockBuilder.block([
            (SigningBlockBuilder.v31ID, SigningBlockBuilder.schemeValue([SigningBlockBuilder.v3Signer(certificates: [TestCertificates.rotatedSigner], minSdk: 33)])),
        ])
        let info = APKSignatures.read(try Self.apk(block: block))

        #expect(info.schemes == [.v31])
        #expect(info.certificates.map(\.sha256) == [TestCertificates.rotatedSignerSHA256])
    }

    @Test func detectsGooglePlaySourceStamp() throws {
        let signer = (SigningBlockBuilder.v2ID, SigningBlockBuilder.schemeValue([SigningBlockBuilder.v2Signer(certificates: [TestCertificates.signer])]))
        let play = APKSignatures.read(try Self.apk(block: SigningBlockBuilder.block([
            signer, (SigningBlockBuilder.sourceStampV2ID, SigningBlockBuilder.sourceStampValue(certificate: TestCertificates.googlePlayStamp)),
        ])))
        let other = APKSignatures.read(try Self.apk(block: SigningBlockBuilder.block([
            signer, (SigningBlockBuilder.sourceStampV2ID, SigningBlockBuilder.sourceStampValue(certificate: TestCertificates.signer)),
        ])))

        #expect(play.hasGooglePlayStamp)
        #expect(!other.hasGooglePlayStamp)
        #expect(play.certificates.map(\.sha256) == [TestCertificates.signerSHA256])
    }

    @Test func reportsUnsignedAPK() throws {
        let info = APKSignatures.read(try Self.apk())

        #expect(info.schemes.isEmpty)
        #expect(info.certificates.isEmpty)
        #expect(!info.hasGooglePlayStamp)
    }

    @Test func survivesCorruptedSigningBlock() throws {
        let valid = SigningBlockBuilder.block([
            (SigningBlockBuilder.v2ID, SigningBlockBuilder.schemeValue([SigningBlockBuilder.v2Signer(certificates: [TestCertificates.signer])])),
            (SigningBlockBuilder.v3ID, SigningBlockBuilder.schemeValue([SigningBlockBuilder.v3Signer(certificates: [TestCertificates.rotatedSigner])])),
        ])
        var generator = SplitMix64(seed: 0xC0FFEE)
        for _ in 0..<1_000 {
            var corrupted = valid
            for _ in 0..<Int.random(in: 1...6, using: &generator) {
                let position = Int.random(in: 0..<corrupted.count, using: &generator)
                corrupted[position] = UInt8.random(in: 0...255, using: &generator)
            }
            _ = APKSignatures.read(try Self.apk(block: corrupted, v1: TestCertificates.signerPKCS7.prefix(Int.random(in: 0...300, using: &generator))))
        }
    }
}
