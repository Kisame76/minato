import APKKit
import Foundation
import Security
import Testing

@testable import APKPack

/// tools/apk-check.py, the independent judge of make-apk's signatures, refuses what Android refuses and
/// accepts what it accepts: the checks of ApkSigningBlockUtils (the ZIP, the APK Signing Block and its
/// pairs) and of ApkSignatureSchemeV2Verifier (the signer), read 2026-09-25. Each APK here is built from
/// APKSigner's parts, or laid out here by the specification, with one thing changed.
@Suite("apk-check")
struct ApkCheckTests {
    /// A second key, for a certificate that belongs to another key.
    static let otherKey: DebugKey = {
        try! DebugKey.load(from: URL.temporaryDirectory.appending(path: "make-apk-test-key-2", directoryHint: .isDirectory))
    }()

    /// The unsigned APK that every case signs, and its content digest.
    static func unsigned(_ apk: Data? = nil) throws -> (apk: Data, digest: Data) {
        let apk = try apk ?? SigningTests.unsigned()
        let (centralDirectory, endRecord) = try APKSigner.layout(of: apk)
        return (apk, APKSigner.contentDigest(of: apk, centralDirectoryOffset: centralDirectory, endRecordOffset: endRecord))
    }

    /// A v2 signer. Each of its signatures is the debug key's RSA PKCS#1 SHA-256 signature over the signed data.
    static func signer(digest: Data, digestAlgorithms: [UInt32] = [0x0103], signatureAlgorithms: [UInt32] = [0x0103],
                       certificates: [Data] = [SigningTests.key.certificate], additionalAttributes: Data = Data(),
                       publicKey: Data = SigningTests.key.subjectPublicKeyInfo) throws -> Data {
        let signed = APKSigner.signedData(digests: digestAlgorithms.map { ($0, digest) }, certificates: certificates,
                                          additionalAttributes: additionalAttributes)
        let signature = try SigningTests.key.sign(signed)
        return APKSigner.signer(signedData: signed, signatures: signatureAlgorithms.map { ($0, signature) }, publicKey: publicKey)
    }

    /// The APK with one v2 signer, signed as APKSigner signs it except for what is given.
    static func variant(digestAlgorithms: [UInt32] = [0x0103], signatureAlgorithms: [UInt32] = [0x0103],
                        certificates: [Data] = [SigningTests.key.certificate], additionalAttributes: Data = Data(),
                        publicKey: Data = SigningTests.key.subjectPublicKeyInfo, unsigned given: Data? = nil) throws -> Data {
        let (apk, digest) = try Self.unsigned(given)
        let signer = try Self.signer(digest: digest, digestAlgorithms: digestAlgorithms, signatureAlgorithms: signatureAlgorithms,
                                     certificates: certificates, additionalAttributes: additionalAttributes, publicKey: publicKey)
        return APKSigner.insert(APKSigner.signingBlock(v2Signers: [signer]), into: apk)
    }

    /// A v3 signer of the debug key (APK Signature Scheme v3): as in v2, with the platform range, 24 and
    /// up, in the signed data and in the signer.
    static func v3Signer(digest: Data) throws -> Data {
        let range = APKSigner.littleEndian(24) + APKSigner.littleEndian(0x7FFF_FFFF)
        let signed = APKSigner.prefixed(APKSigner.prefixed(APKSigner.littleEndian(0x0103) + APKSigner.prefixed(digest)))
            + APKSigner.prefixed(APKSigner.prefixed(SigningTests.key.certificate)) + range + APKSigner.prefixed(Data())
        let signature = try SigningTests.key.sign(signed)
        return APKSigner.prefixed(signed) + range
            + APKSigner.prefixed(APKSigner.prefixed(APKSigner.littleEndian(0x0103) + APKSigner.prefixed(signature)))
            + APKSigner.prefixed(SigningTests.key.subjectPublicKeyInfo)
    }

    /// One additional attribute: its ID and its value, length-prefixed.
    static func attribute(_ id: UInt32, _ value: UInt32) -> Data {
        APKSigner.prefixed(APKSigner.littleEndian(id) + APKSigner.littleEndian(value))
    }

    // MARK: - An APK Signing Block laid out here, so that one of its fields can be wrong

    static func littleEndian64(_ value: UInt64) -> Data {
        withUnsafeBytes(of: value.littleEndian) { Data($0) }
    }

    /// An ID-value pair of the APK Signing Block.
    static func pair(_ id: UInt32, _ value: Data) -> Data {
        littleEndian64(UInt64(4 + value.count)) + APKSigner.littleEndian(id) + value
    }

    /// The pair of a v2 block with these signers.
    static func v2Pair(_ signers: [Data]) -> Data {
        pair(0x7109_871A, APKSigner.prefixed(signers.map(APKSigner.prefixed).reduce(Data(), +)))
    }

    /// The pair of a valid v2 block for the unsigned APK.
    static func validV2Pair() throws -> Data {
        try v2Pair([signer(digest: unsigned().digest)])
    }

    /// The unsigned APK with an APK Signing Block of these pairs: its size, the pairs, its size again and
    /// the magic. The sizes are right unless others are given.
    static func apk(pairs: Data, headerSize: UInt64? = nil, footerSize: UInt64? = nil) throws -> Data {
        let size = UInt64(pairs.count + 24)
        let block = littleEndian64(headerSize ?? size) + pairs + littleEndian64(footerSize ?? size) + Data("APK Sig Block 42".utf8)
        return APKSigner.insert(block, into: try unsigned().apk)
    }

    static func refuses(_ apk: Data, naming reason: String) throws {
        let output = try SigningTests.apkCheck(apk)
        #expect(output.contains("RESULT: SIGNATURE PROBLEM"), "\(output)")
        #expect(output.contains(reason), "\(output)")
    }

    @discardableResult
    static func accepts(_ apk: Data) throws -> String {
        let output = try SigningTests.apkCheck(apk)
        #expect(output.contains("RESULT: signatures valid"), "\(output)")
        return output
    }

    // MARK: - The signer (ApkSignatureSchemeV2Verifier)

    @Test func refusesAPublicKeyThatIsNotSubjectPublicKeyInfo() throws {
        // PKCS#1, as Security.framework exports the certificate's key: Android's X509EncodedKeySpec refuses it.
        let certificate = try #require(SecCertificateCreateWithData(nil, SigningTests.key.certificate as CFData))
        let key = try #require(SecCertificateCopyKey(certificate))
        let rsaPublicKey = try #require(SecKeyCopyExternalRepresentation(key, nil) as Data?)

        try Self.refuses(Self.variant(publicKey: rsaPublicKey), naming: "SubjectPublicKeyInfo")
    }

    @Test func refusesAPublicKeyThatIsNotTheCertificates() throws {
        // Signed with the debug key and its public key, but listing another key's certificate.
        try Self.refuses(Self.variant(certificates: [Self.otherKey.certificate]), naming: "SubjectPublicKeyInfo")
    }

    @Test func refusesASignerWithoutCertificates() throws {
        try Self.refuses(Self.variant(certificates: []), naming: "no certificate")
    }

    @Test func refusesACertificateThatDoesNotParse() throws {
        // Android decodes every certificate, not only the first. This one is a DER sequence of one integer.
        try Self.refuses(Self.variant(certificates: [SigningTests.key.certificate, Data([0x30, 0x03, 0x02, 0x01, 0x01])]),
                         naming: "certificate #2 does not parse")
    }

    @Test(arguments: [Data([0x30, 0x03, 0x02, 0x01, 0x01]), Data([0x30, 0x02, 0x30, 0x00])])
    func refusesAFirstCertificateThatDoesNotParse(der: Data) throws {
        // A sequence of one integer, and a sequence of an empty one: no SubjectPublicKeyInfo to compare with.
        try Self.refuses(Self.variant(certificates: [der]), naming: "certificate #1 does not parse")
    }

    @Test func refusesAdditionalAttributesWhoseLengthPrefixDoesNotFit() throws {
        // Three bytes cannot hold an attribute's length prefix.
        try Self.refuses(Self.variant(additionalAttributes: Data([1, 2, 3])),
                         naming: "an additional attribute: its length prefix runs past")
    }

    @Test func refusesAnAdditionalAttributeWhoseLengthRunsPastTheList() throws {
        // A known-looking attribute, but its length prefix claims 100 bytes where 8 follow.
        let attributes = APKSigner.littleEndian(100) + APKSigner.littleEndian(0x1234_5678) + APKSigner.littleEndian(7)

        try Self.refuses(Self.variant(additionalAttributes: attributes), naming: "an additional attribute: its length runs past")
    }

    @Test func refusesAnAdditionalAttributeTooShortForItsID() throws {
        // Its length prefix fits, but two bytes cannot hold the attribute's ID.
        try Self.refuses(Self.variant(additionalAttributes: APKSigner.prefixed(Data([0x01, 0x02]))), naming: "too short for its ID")
    }

    @Test func refusesAStrippingProtectionThatPointsToAMissingV3Block() throws {
        try Self.refuses(Self.variant(additionalAttributes: Self.attribute(0xBEEF_F00D, 3)), naming: "stripping protection")
    }

    @Test func refusesAStrippingProtectionWithoutAValue() throws {
        try Self.refuses(Self.variant(additionalAttributes: APKSigner.prefixed(APKSigner.littleEndian(0xBEEF_F00D))),
                         naming: "stripping protection attribute is too short")
    }

    @Test func acceptsAStrippingProtectionThatPointsToAnotherScheme() throws {
        // Android refuses only the value 3, v3.
        try Self.accepts(Self.variant(additionalAttributes: Self.attribute(0xBEEF_F00D, 2)))
    }

    @Test func acceptsAStrippingProtectionWhenTheV3BlockIsThere() throws {
        // Android then verifies v3 and never reads v2.
        let digest = try Self.unsigned().digest
        let v2 = try Self.signer(digest: digest, additionalAttributes: Self.attribute(0xBEEF_F00D, 3))
        let v3 = try Self.pair(0xF053_68C0, APKSigner.prefixed(APKSigner.prefixed(Self.v3Signer(digest: digest))))

        try Self.accepts(Self.apk(pairs: Self.v2Pair([v2]) + v3))
    }

    @Test func acceptsAnAdditionalAttributeItDoesNotKnow() throws {
        // Android moves along past attributes it does not know.
        try Self.accepts(Self.variant(additionalAttributes: Self.attribute(0x1234_5678, 7)))
    }

    @Test func refusesDigestsAndSignaturesWithDifferentAlgorithms() throws {
        try Self.refuses(Self.variant(digestAlgorithms: [0x0103, 0x0201]), naming: "algorithms of digests and signatures differ")
    }

    @Test func refusesTheSameAlgorithmsInAnotherOrder() throws {
        // Android compares lists, not sets. 0x0421 (verity) is signed as 0x0103 is; this script checks only
        // 0x0103, so the order is the one fault it can see.
        try Self.refuses(Self.variant(digestAlgorithms: [0x0421, 0x0103], signatureAlgorithms: [0x0103, 0x0421]),
                         naming: "algorithms of digests and signatures differ")
    }

    @Test func refusesASignerWithoutASupportedAlgorithm() throws {
        try Self.refuses(Self.variant(digestAlgorithms: [0x0999], signatureAlgorithms: [0x0999]), naming: "no supported")
    }

    @Test func refusesASignerWithOnlyVerityAlgorithms() throws {
        // Android supports them, but their digest covers the verity tree, which this script does not
        // compute: it has nothing to check and must not say "valid".
        try Self.refuses(Self.variant(digestAlgorithms: [0x0421], signatureAlgorithms: [0x0421]), naming: "verity")
    }

    @Test(arguments: [2, 4])
    func refusesASignatureRecordTooShort(length: Int) throws {
        // A record holds the algorithm and the signature's length prefix: 8 bytes at least.
        let (apk, digest) = try Self.unsigned()
        let signed = APKSigner.signedData(digests: [(0x0103, digest)], certificates: [SigningTests.key.certificate])
        let signatures = APKSigner.prefixed(APKSigner.prefixed(APKSigner.littleEndian(0x0103).prefix(length)))
        let signer = APKSigner.prefixed(signed) + signatures + APKSigner.prefixed(SigningTests.key.subjectPublicKeyInfo)

        try Self.refuses(APKSigner.insert(APKSigner.signingBlock(v2Signers: [signer]), into: apk), naming: "too short")
    }

    @Test func refusesADigestRecordTooShort() throws {
        // The digests hold one record with the algorithm only.
        let apk = try Self.unsigned().apk
        let signed = APKSigner.prefixed(APKSigner.prefixed(APKSigner.littleEndian(0x0103)))
            + APKSigner.prefixed(APKSigner.prefixed(SigningTests.key.certificate)) + APKSigner.prefixed(Data())
        let signature = try SigningTests.key.sign(signed)
        let signer = APKSigner.signer(signedData: signed, signatures: [(0x0103, signature)], publicKey: SigningTests.key.subjectPublicKeyInfo)

        try Self.refuses(APKSigner.insert(APKSigner.signingBlock(v2Signers: [signer]), into: apk), naming: "too short")
    }

    @Test func refusesAV2BlockWithoutSigners() throws {
        try Self.refuses(Self.apk(pairs: Self.v2Pair([])), naming: "no signer")
    }

    // MARK: - The ZIP and the APK Signing Block (ApkSigningBlockUtils)

    @Test func refusesAFileThatIsNoZIP() throws {
        try Self.refuses(Data("not a ZIP archive".utf8), naming: "no ZIP end record")
    }

    @Test func refusesAZIPThatDoesNotOpen() throws {
        // The first central directory entry loses its signature before signing: the signature is valid,
        // but no zip reader finds the entries.
        var unsigned = try SigningTests.unsigned()
        let (centralDirectory, _) = try APKSigner.layout(of: unsigned)
        unsigned.replaceSubrange(centralDirectory..<centralDirectory + 4, with: Data("PK\u{0}\u{0}".utf8))

        try Self.refuses(Self.variant(unsigned: unsigned), naming: "the ZIP does not open")
    }

    @Test func refusesZIP64() throws {
        // A ZIP64 end of central directory locator right before the end record.
        var apk = try Self.variant()
        let (_, endRecord) = try APKSigner.layout(of: apk)
        apk.insert(contentsOf: Data([0x50, 0x4B, 0x06, 0x07]) + Data(count: 16), at: endRecord)

        try Self.refuses(apk, naming: "ZIP64")
    }

    @Test func refusesACentralDirectoryThatDoesNotEndAtTheEndRecord() throws {
        // The end record says the central directory is one byte longer than it is.
        var unsigned = try SigningTests.unsigned()
        let (_, endRecord) = try APKSigner.layout(of: unsigned)
        let size = unsigned[endRecord + 12..<endRecord + 16].reversed().reduce(UInt32(0)) { $0 << 8 | UInt32($1) }
        unsigned.replaceSubrange(endRecord + 12..<endRecord + 16, with: APKSigner.littleEndian(size + 1))

        try Self.refuses(Self.variant(unsigned: unsigned), naming: "does not end where the end record starts")
    }

    @Test func refusesACentralDirectoryBeforeByte32() throws {
        // Four bytes, the block's magic, and an empty central directory at byte 20, where the end record starts.
        let endRecord = Data([0x50, 0x4B, 0x05, 0x06]) + Data(count: 8) + APKSigner.littleEndian(0) + APKSigner.littleEndian(20)
            + Data(count: 2)

        try Self.refuses(Data(count: 4) + Data("APK Sig Block 42".utf8) + endRecord, naming: "before byte 32")
    }

    @Test func refusesASigningBlockSizeOutOfRange() throws {
        // The footer's size is smaller than the footer.
        try Self.refuses(Self.apk(pairs: Self.validV2Pair(), footerSize: 16), naming: "size is out of range")
    }

    @Test func refusesASigningBlockThatWouldStartBeforeTheFile() throws {
        try Self.refuses(Self.apk(pairs: Self.validV2Pair(), footerSize: 1 << 30), naming: "offset is out of range")
    }

    @Test func refusesDifferentSizesInHeaderAndFooter() throws {
        let pairs = try Self.validV2Pair()

        try Self.refuses(Self.apk(pairs: pairs, headerSize: UInt64(pairs.count + 24 + 8)), naming: "header and footer")
    }

    @Test(arguments: [0, 1 << 20] as [UInt64])
    func refusesAPairThatDoesNotParseBeforeTheV2Block(size: UInt64) throws {
        // Android's search for the v2 block ends at the first pair it cannot read: one under 4 bytes, or one
        // longer than what is left.
        let pairs = try Self.littleEndian64(size) + APKSigner.littleEndian(0x1234_5678) + Self.validV2Pair()
        let apk = try Self.apk(pairs: pairs)

        try Self.refuses(apk, naming: "pair #1")
        try Self.refuses(apk, naming: "no v2, v3 or v3.1 block that Android finds")
    }

    @Test(arguments: [4, 8])
    func acceptsAPairThatDoesNotParseAfterTheV2Block(zeros: Int) throws {
        // Android finds the v2 block first; its search for a v3 block ends at the bad pair as if there were none.
        try Self.accepts(Self.apk(pairs: Self.validV2Pair() + Data(count: zeros)))
    }

    @Test func acceptsASourceStampThatDoesNotParse() throws {
        // Android's signature check does not read the stamp; this script only says who stamped.
        let output = try Self.accepts(Self.apk(pairs: Self.validV2Pair() + Self.pair(0x6DFF_800D, Data([1, 2]))))

        #expect(output.contains("source stamp v2: does not parse"), "\(output)")
    }
}
