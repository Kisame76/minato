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

    // MARK: - The signer as Android 15 decides it (ADR-0034, decision 2)

    static let old = TestCertificates.signer
    static let oldSHA256 = TestCertificates.signerSHA256
    static let new = TestCertificates.rotatedSigner
    static let newSHA256 = TestCertificates.rotatedSignerSHA256
    /// Any other certificate: only its SHA-256 counts here.
    static let third = TestCertificates.googlePlayStamp
    static let thirdSHA256 = SigningCertificate(der: TestCertificates.googlePlayStamp).sha256

    static func scheme(_ id: UInt32, _ signers: [Data]) -> (id: UInt32, value: Data) {
        (id, SigningBlockBuilder.schemeValue(signers))
    }

    @Test func theSignerOfV2() throws {
        let block = SigningBlockBuilder.block([Self.scheme(SigningBlockBuilder.v2ID, [SigningBlockBuilder.v2Signer(certificates: [Self.old])])])
        let signer = APKSignatures.read(try Self.apk(block: block, v1: TestCertificates.signerPKCS7)).signer

        #expect(signer == SignerIdentity(current: [Self.oldSHA256], lineage: []))
    }

    @Test func theSignerOfV1Only() throws {
        #expect(APKSignatures.read(try Self.apk(v1: TestCertificates.signerPKCS7)).signer == SignerIdentity(current: [Self.oldSHA256]))
    }

    @Test func anUnsignedAPKHasNoCurrentCertificate() throws {
        #expect(APKSignatures.read(try Self.apk()).signer == SignerIdentity(current: []))
    }

    /// Lineage nodes with the flags SigningBlockBuilder gives every level.
    static func nodes(_ certificates: [String]) -> [LineageNode] {
        certificates.map { LineageNode(sha256: $0, flags: 0x1F) }
    }

    @Test func v3WithItsLineage() throws {
        // A rotated key: v2 still carries the old key, v3 the new one with the old one in its lineage.
        let block = SigningBlockBuilder.block([
            Self.scheme(SigningBlockBuilder.v2ID, [SigningBlockBuilder.v2Signer(certificates: [Self.old])]),
            Self.scheme(SigningBlockBuilder.v3ID, [SigningBlockBuilder.v3Signer(certificates: [Self.new], lineage: [Self.old, Self.new])]),
        ])
        let info = APKSignatures.read(try Self.apk(block: block, v1: TestCertificates.signerPKCS7))

        #expect(info.signer == SignerIdentity(current: [Self.newSHA256], lineage: Self.nodes([Self.oldSHA256])))
        // The list of every certificate stays as it was.
        #expect(info.certificates.map(\.sha256) == [Self.newSHA256, Self.oldSHA256])
    }

    @Test func aLineageOfThreeKeysOldestFirst() throws {
        let block = SigningBlockBuilder.block([
            Self.scheme(SigningBlockBuilder.v3ID, [SigningBlockBuilder.v3Signer(certificates: [Self.new], lineage: [Self.third, Self.old, Self.new])]),
        ])

        #expect(APKSignatures.read(try Self.apk(block: block)).signer
            == SignerIdentity(current: [Self.newSHA256], lineage: Self.nodes([Self.thirdSHA256, Self.oldSHA256])))
    }

    @Test func v31BeforeV3() throws {
        // Rotation for Android 13 and later: v3 keeps the old key for older versions, v3.1 has the new one.
        let block = SigningBlockBuilder.block([
            Self.scheme(SigningBlockBuilder.v3ID, [SigningBlockBuilder.v3Signer(certificates: [Self.old], maxSdk: 32)]),
            Self.scheme(SigningBlockBuilder.v31ID, [SigningBlockBuilder.v3Signer(certificates: [Self.new], minSdk: 33, lineage: [Self.old, Self.new])]),
        ])

        #expect(APKSignatures.read(try Self.apk(block: block)).signer == SignerIdentity(current: [Self.newSHA256], lineage: Self.nodes([Self.oldSHA256])))
    }

    @Test func aV3SignerForOtherAndroidVersionsDoesNotCount() throws {
        // A v3.1 signer only for versions after Android 15, and v3 signers per range: Android 15 (API 35)
        // takes the v3 signer whose range holds it.
        let block = SigningBlockBuilder.block([
            Self.scheme(SigningBlockBuilder.v2ID, [SigningBlockBuilder.v2Signer(certificates: [Self.third])]),
            Self.scheme(SigningBlockBuilder.v3ID, [
                SigningBlockBuilder.v3Signer(certificates: [Self.third], maxSdk: 34),
                SigningBlockBuilder.v3Signer(certificates: [Self.old], minSdk: 35),
            ]),
            Self.scheme(SigningBlockBuilder.v31ID, [SigningBlockBuilder.v3Signer(certificates: [Self.new], minSdk: 36)]),
        ])

        #expect(APKSignatures.read(try Self.apk(block: block)).signer == SignerIdentity(current: [Self.oldSHA256]))
    }

    @Test func severalSignersOfV2() throws {
        let block = SigningBlockBuilder.block([Self.scheme(SigningBlockBuilder.v2ID, [
            SigningBlockBuilder.v2Signer(certificates: [Self.new]),
            SigningBlockBuilder.v2Signer(certificates: [Self.old, Self.third]), // a chain: the first is the signer's
        ])])

        #expect(APKSignatures.read(try Self.apk(block: block)).signer == SignerIdentity(current: [Self.newSHA256, Self.oldSHA256].sorted()))
    }

    @Test func aReportFromBeforeTheSignerDecodes() throws {
        let json = #"{"schemes":["v2"],"certificates":[{"sha256":"aa"}],"hasGooglePlayStamp":false}"#
        #expect(try JSONDecoder().decode(SignatureInfo.self, from: Data(json.utf8)).signer == nil)
    }

    @Test func aSignerFromStepBDecodesWithoutCapabilities() throws {
        let json = #"{"current":["bb"],"lineage":["aa"]}"#
        let signer = try JSONDecoder().decode(SignerIdentity.self, from: Data(json.utf8))
        #expect(signer == SignerIdentity(current: ["bb"], lineage: [LineageNode(sha256: "aa", flags: nil)]))
        #expect(!signer.knowsCapabilities)
        // And written again, with them.
        let written = SignerIdentity(current: ["bb"], lineage: Self.nodes(["aa"]))
        #expect(try JSONDecoder().decode(SignerIdentity.self, from: JSONEncoder().encode(written)) == written)
    }

    @Test func survivesCorruptedSigningBlock() throws {
        let valid = SigningBlockBuilder.block([
            (SigningBlockBuilder.v2ID, SigningBlockBuilder.schemeValue([SigningBlockBuilder.v2Signer(certificates: [TestCertificates.signer])])),
            (SigningBlockBuilder.v3ID, SigningBlockBuilder.schemeValue([SigningBlockBuilder.v3Signer(
                certificates: [TestCertificates.rotatedSigner], lineage: [TestCertificates.signer, TestCertificates.rotatedSigner])])),
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
