import APKTestSupport
import Foundation
import Testing

@testable import APKKit

/// signers.json (ADR-0034 decision 2, ADR-0039 b3.5b1): the import writes what the signature check proved into the
/// version's folder, and the guest's PackageManager answers from it. The format is the one framework/java's
/// SignersFile reads, and only in this form:
/// {"format":1,"lineage":[{"certificate":"<base64 DER>","flags":N},...],"scheme":N,"signers":["<base64 DER>",...]}.
@Suite("SignersFile")
struct SignersFileTests {
    struct Content: Decodable, Equatable {
        struct Node: Decodable, Equatable {
            var certificate: String
            var flags: Int32
        }

        var format: Int
        var scheme: Int
        var signers: [String]
        var lineage: [Node]
    }

    /// The file of a really signed APK whose manifest targets SDK 29; nil when the check proved nothing.
    static func content(_ apk: Data) throws -> Content? {
        guard let proof = SignatureVerifier.verify(try ZipArchive(data: apk), targetSdk: 29).proof else { return nil }
        let data = try SignersFile.content(of: proof)
        // Exactly the four keys the guest's reader accepts.
        let keys = try #require(JSONSerialization.jsonObject(with: data) as? [String: Any]).keys.sorted()
        #expect(keys == ["format", "lineage", "scheme", "signers"])
        return try JSONDecoder().decode(Content.self, from: data)
    }

    static func zip() -> ZipBuilder { SignatureVerifierTests.zip() }

    static func base64(_ key: TestKey) -> String { key.certificate.base64EncodedString() }

    @Test func oneSignerOfV2() throws {
        let content = try #require(try Self.content(TestSigner.sign(Self.zip(), v2: [.init(.ec)])))
        #expect(content == Content(format: 1, scheme: 2, signers: [Self.base64(.ec)], lineage: []))
    }

    @Test func severalSignersInTheBlocksOrder() throws {
        let content = try #require(try Self.content(TestSigner.sign(Self.zip(), v2: [.init(.rotated), .init(.ec)])))
        #expect(content.signers == [Self.base64(.rotated), Self.base64(.ec)])
        #expect(content.lineage.isEmpty)
    }

    @Test func eachSignersOwnCertificateNotItsChain() throws {
        let content = try #require(try Self.content(TestSigner.sign(Self.zip(), v2: [.init(.rotated, chain: [TestKey.ec.certificate])])))
        #expect(content.signers == [Self.base64(.rotated)])
    }

    @Test func v1Only() throws {
        let content = try #require(try Self.content(TestSigner.sign(Self.zip(), v1: TestSigner.V1([.ec]))))
        #expect(content == Content(format: 1, scheme: 1, signers: [Self.base64(.ec)], lineage: []))
    }

    @Test func v1ReSignedWithTheDevelopersCertificateFirstInTheBlock() throws {
        // The reviewer's case: signed with M, the developer's D first among the block's certificates. Only M signed.
        var v1 = TestSigner.V1([.rotated])
        v1.certificatesBefore = [TestKey.ec.certificate]
        let content = try #require(try Self.content(TestSigner.sign(Self.zip(), v1: v1)))
        #expect(content.signers == [Self.base64(.rotated)])
    }

    @Test func v1AnOrphanSignatureBlockSignsNothing() throws {
        // The reviewer's case: a META-INF/AAA.RSA without its .SF, carrying D, before the real block of M.
        var zip = Self.zip()
        zip.add("META-INF/AAA.RSA", TestSigner.pkcs7(signing: Data(), with: .ec, TestSigner.V1([.ec])))
        zip.entries += TestSigner.jarSignatureFiles(for: Self.zip().entries, TestSigner.V1([.rotated]))
        let content = try #require(try Self.content(zip.build().data))
        #expect(content.signers == [Self.base64(.rotated)])
    }

    @Test func v3WithItsWholeLineageEndingWithTheSigner() throws {
        let apk = TestSigner.sign(Self.zip(), v2: [.init(.rsa)],
                                  v3: [.init(.rotated, lineage: [.init(.rsa), .init(.ec), .init(.rotated)])])
        let content = try #require(try Self.content(apk))
        #expect(content.scheme == 3)
        #expect(content.signers == [Self.base64(.rotated)])
        #expect(content.lineage == [TestKey.rsa, .ec, .rotated].map {
            Content.Node(certificate: Self.base64($0), flags: Int32(TestSigner.defaultCapabilities))
        })
    }

    @Test func v31BeforeV3() throws {
        let apk = TestSigner.sign(Self.zip(), v2: [.init(.ec)],
                                  v3: [.init(.ec, maxSdk: 32, attributes: [(TestSigner.rotationMinSdkID, TestSigner.u32(33))])],
                                  v31: [.init(.rotated, minSdk: 33, lineage: [.init(.ec), .init(.rotated)])])
        let content = try #require(try Self.content(apk))
        #expect(content.scheme == 31)
        #expect(content.signers == [Self.base64(.rotated)])
        #expect(content.lineage.map(\.certificate) == [Self.base64(.ec), Self.base64(.rotated)])
    }

    @Test func aV31SignerForLaterAndroidVersionsLeavesV3() throws {
        let apk = TestSigner.sign(Self.zip(), v2: [.init(.ec)],
                                  v3: [.init(.ec, attributes: [(TestSigner.rotationMinSdkID, TestSigner.u32(36))])],
                                  v31: [.init(.rotated, minSdk: 36, lineage: [.init(.ec), .init(.rotated)])])
        #expect(try Self.content(apk) == Content(format: 1, scheme: 3, signers: [Self.base64(.ec)], lineage: []))
    }

    @Test func aV31SignerForTheDevelopmentPreviewLeavesV3() throws {
        let apk = TestSigner.sign(Self.zip(), v2: [.init(.ec)],
                                  v3: [.init(.ec, attributes: [(TestSigner.rotationMinSdkID, TestSigner.u32(35))])],
                                  v31: [.init(.rotated, minSdk: 35, lineage: [.init(.ec), .init(.rotated)],
                                              attributes: [(TestSigner.rotationOnDevReleaseID, Data())])])
        #expect(try Self.content(apk) == Content(format: 1, scheme: 3, signers: [Self.base64(.ec)], lineage: []))
    }

    @Test func flagsKeepTheirBitsAsAnInt() throws {
        let apk = TestSigner.sign(Self.zip(), v2: [.init(.ec)],
                                  v3: [.init(.rotated, lineage: [.init(.ec, flags: 0x8000_0001), .init(.rotated, flags: 0x1F)])])
        let content = try #require(try Self.content(apk))
        #expect(content.lineage.map(\.flags) == [Int32(bitPattern: 0x8000_0001), 0x1F])
    }

    @Test func anAPKAndroidRefusesHasNoFile() throws {
        // A lineage that does not end with the signer, and two v3 signers for Android 15 (ApkSignatureSchemeV3Verifier.java
        // 243-245): the check fails, so nothing is written.
        #expect(try Self.content(TestSigner.sign(Self.zip(), v2: [.init(.ec)],
                                                 v3: [.init(.rotated, lineage: [.init(.ec), .init(.rsa)])])) == nil)
        #expect(try Self.content(TestSigner.sign(Self.zip(), v2: [.init(.ec)], v3: [
            .init(.rotated, lineage: [.init(.ec), .init(.rotated)]), .init(.rsa),
        ])) == nil)
    }

    @Test func anUnsignedOrUnverifiedAPKHasNoFile() throws {
        #expect(try Self.content(Self.zip().build().data) == nil)
        // Verity only, and DSA: Android checks them, Minato cannot, so the guest sees the APK unsigned.
        #expect(try Self.content(TestSigner.sign(Self.zip(), v2: [.init(.ec, algorithms: [TestSigner.ecdsaSHA256Verity])])) == nil)
        #expect(try Self.content(TestSigner.sign(Self.zip(), v1: TestSigner.V1([.dsa]))) == nil)
    }

    @Test func theSignersAreTheVerifiersSigner() throws {
        var v1 = TestSigner.V1([.rotated])
        v1.certificatesBefore = [TestKey.ec.certificate]
        for apk in [
            TestSigner.sign(Self.zip(), v1: v1),
            TestSigner.sign(Self.zip(), v2: [.init(.ec)], v3: [.init(.rotated, lineage: [.init(.ec), .init(.rotated)])]),
        ] {
            let verification = SignatureVerifier.verify(try ZipArchive(data: apk), targetSdk: 29)
            let signer = try #require(verification.signer)
            let content = try #require(try Self.content(apk))
            #expect(Set(content.signers.map { SigningCertificate.sha256(of: Data(base64Encoded: $0)!) }) == Set(signer.current))
            #expect(content.lineage.dropLast().map { SigningCertificate.sha256(of: Data(base64Encoded: $0.certificate)!) }
                == signer.lineage.map(\.sha256))
        }
    }

    @Test func theOneFormTheGuestReads() throws {
        // Keys sorted, nothing spaced or escaped: framework/tests' SignersFileTest reads these same bytes.
        let proof = SignerProof(scheme: .v3, certificates: [Data([0x30, 0x01, 0xFF])], lineage: [
            .init(certificate: Data([0x30, 0x01, 0xFB]), flags: 0x8000_0000),
            .init(certificate: Data([0x30, 0x01, 0xFF]), flags: 23),
        ])
        let text = String(decoding: try SignersFile.content(of: proof), as: UTF8.self)
        #expect(text == #"{"format":1,"lineage":[{"certificate":"MAH7","flags":-2147483648},{"certificate":"MAH/","flags":23}],"#
            + #""scheme":3,"signers":["MAH/"]}"#)
    }

    // MARK: - The reader's limits

    static let certificate = Data([0x30, 0x01, 0x00])

    @Test func theWriterKeepsTheReadersLimits() throws {
        let one = Self.certificate
        let other = Data([0x30, 0x01, 0x01])
        let refused: [(SignerProof, String)] = [
            (SignerProof(scheme: .v2, certificates: []), "no signer"),
            (SignerProof(scheme: .v1, certificates: Array(repeating: one, count: 65)), "65 signers"),
            (SignerProof(scheme: .v3, certificates: [one], lineage: (0..<65).map { .init(certificate: Data([0x30, UInt8($0)]), flags: 0) }),
             "65 lineage levels"),
            (SignerProof(scheme: .v2, certificates: [Data([0x02, 0x01, 0x00])]), "not DER"),
            (SignerProof(scheme: .v2, certificates: [one], lineage: [.init(certificate: one, flags: 0)]), "belongs to one signer of v3 or v3.1, not to 1 of v2"),
            (SignerProof(scheme: .v3, certificates: [one, other], lineage: [.init(certificate: one, flags: 0)]), "not to 2 of v3"),
            (SignerProof(scheme: .v3, certificates: [one], lineage: [.init(certificate: other, flags: 0)]), "does not end with"),
            (SignerProof(scheme: .v3, certificates: [one], lineage: [.init(certificate: one, flags: 0), .init(certificate: one, flags: 0)]),
             "twice"),
            (SignerProof(scheme: .v2, certificates: [Data([0x30]) + Data(repeating: 0, count: 800_000)]), "larger than the 1 MiB"),
        ]
        for (proof, reason) in refused {
            let problem = try #require(SignersFile.problem(with: proof), "accepted: \(reason)")
            #expect(problem.contains(reason), "\(problem)")
            #expect(throws: SignersFile.Refusal.self) { try SignersFile.content(of: proof) }
        }
        // At the limits: 64 entries each, and close to 1 MiB.
        let most = SignerProof(scheme: .v3, certificates: [one], lineage: (0..<63).map {
            .init(certificate: Data([0x30, 0x01, UInt8($0 + 1)]), flags: 0)
        } + [.init(certificate: one, flags: 0)])
        #expect(SignersFile.problem(with: most) == nil)
        #expect(SignersFile.problem(with: SignerProof(scheme: .v1, certificates: Array(repeating: one, count: 64))) == nil)
        let large = SignerProof(scheme: .v2, certificates: [Data([0x30]) + Data(repeating: 0, count: 786_350)])
        #expect(try SignersFile.content(of: large).count <= SignersFile.maximumBytes)
    }

    @Test func noEscapedSlashes() throws {
        let text = String(decoding: try SignersFile.content(of: SignerProof(scheme: .v2, certificates: [Data([0x30, 0x01, 0xFF])])),
                          as: UTF8.self)
        #expect(text.contains("MAH/"))
        #expect(!text.contains("\\/"))
    }
}
