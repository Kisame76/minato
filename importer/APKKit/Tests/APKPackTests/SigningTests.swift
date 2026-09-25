import APKKit
import APKPack
import APKTestSupport
import CryptoKit
import Foundation
import Security
import Testing

/// APK Signature Scheme v2 with the debug key (ADR-0031, decisions 5 and 6).
@Suite("APKSigner")
struct SigningTests {
    /// One debug key for the suite, made the way make-apk makes it, in a temporary folder that the
    /// next run uses again.
    static let key: DebugKey = {
        try! DebugKey.load(from: URL.temporaryDirectory.appending(path: "make-apk-test-key", directoryHint: .isDirectory))
    }()

    static let repository = URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        .deletingLastPathComponent().deletingLastPathComponent()

    static func unsigned() throws -> Data {
        try APKLayout.build(manifest: LayoutTests.manifest(),
                            libraries: [PackFile(name: "libactivity-test.so", contents: LayoutTests.library())],
                            assets: LayoutTests.assets)
    }

    static func sha256(_ data: Data) -> String {
        SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
    }

    // MARK: - Content digest

    /// Bytes that Python writes the same way: ((i * 7) + (i >> 8)) & 0xFF.
    static func pattern(_ count: Int) -> Data {
        Data((0..<count).map { UInt8(truncatingIfNeeded: $0 &* 7 &+ ($0 >> 8)) })
    }

    /// Expected values from `content_digest` in tools/apk-check.py (Python's hashlib), computed once on
    /// the same bytes with the signing block at the central directory. The larger one spans three
    /// chunks in its first section (1 MiB, 1 MiB, 0.5 MiB).
    @Test(arguments: [
        (1_000, 700, 978, "f32d9757ad8d505bccf5123ee60473412f9b858b1fc6f81dbf11f0b316f8f9ec"),
        (2_626_462, 2_621_440, 2_626_440, "7b4575665ae86c5b23e6d10dcee27f05afadb317dd5e524ec346e1511b6c82f4"),
    ])
    func computesTheContentDigestAsApkCheckDoes(count: Int, centralDirectory: Int, endRecord: Int, expected: String) {
        let digest = APKSigner.contentDigest(of: Self.pattern(count), centralDirectoryOffset: centralDirectory,
                                             endRecordOffset: endRecord)

        #expect(digest.map { String(format: "%02x", $0) }.joined() == expected)
    }

    // MARK: - Signing block

    @Test func signsWhatAPKKitAndApkCheckAccept() throws {
        let unsigned = try Self.unsigned()
        let signed = try APKSigner.sign(unsigned, with: Self.key)

        let signature = APKSignatures.read(try ZipArchive(data: signed))
        #expect(signature.schemes == [.v2])
        #expect(signature.certificates.map(\.sha256) == [Self.sha256(Self.key.certificate)])

        let archive = try ZipArchive(data: signed)
        let unsignedArchive = try ZipArchive(data: unsigned)
        #expect(archive.entries.map(\.name) == unsignedArchive.entries.map(\.name))
        #expect(try archive.data(named: "assets/a.txt") == Data(repeating: 0x61, count: 300))
        let block = try #require(V2Block(signed))
        #expect(UInt64(block.start) == unsignedArchive.centralDirectoryOffset)
        #expect(archive.centralDirectoryOffset == UInt64(block.end))

        #expect(try Self.apkCheck(signed).contains("RESULT: signatures valid"))
    }

    @Test func buildsTheSignerAsAndroidChecksIt() throws {
        let signed = try APKSigner.sign(try Self.unsigned(), with: Self.key)
        let block = try #require(V2Block(signed))

        // One signer; digests and signatures list the same algorithms (RSASSA-PKCS1-v1_5, SHA-256).
        #expect(block.digestAlgorithms == [0x0103])
        #expect(block.signatureAlgorithms == [0x0103])
        #expect(block.certificates == [Self.key.certificate])
        #expect(block.additionalAttributes.isEmpty)
        // The public key is the certificate's SubjectPublicKeyInfo, byte for byte: for RSA 2048,
        // SEQUENCE { SEQUENCE { rsaEncryption, NULL }, BIT STRING { RSAPublicKey } } (RFC 3279), 294 bytes,
        // around the key that Security.framework reads from the certificate (PKCS#1).
        let certificate = try #require(SecCertificateCreateWithData(nil, Self.key.certificate as CFData))
        let publicKey = try #require(SecCertificateCopyKey(certificate))
        let rsaPublicKey = try #require(SecKeyCopyExternalRepresentation(publicKey, nil) as Data?)
        let header: [UInt8] = [0x30, 0x82, 0x01, 0x22, 0x30, 0x0D, 0x06, 0x09, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D,
                               0x01, 0x01, 0x01, 0x05, 0x00, 0x03, 0x82, 0x01, 0x0F, 0x00]
        #expect(Array(block.publicKey.prefix(header.count)) == header)
        #expect(block.publicKey.count == 294)
        #expect(block.publicKey.suffix(rsaPublicKey.count) == rsaPublicKey)
        #expect(Self.key.certificate.range(of: block.publicKey) != nil)
        // The signature over the signed data verifies with the key of the certificate.
        #expect(SecKeyVerifySignature(publicKey, .rsaSignatureMessagePKCS1v15SHA256, block.signedData as CFData,
                                      block.signature as CFData, nil))
    }

    @Test func signsTheSameInputsToTheSameBytes() throws {
        let unsigned = try Self.unsigned()
        let first = try APKSigner.sign(unsigned, with: Self.key)

        #expect(try APKSigner.sign(unsigned, with: Self.key) == first)
        #expect(V2Block(first) != nil)
    }

    @Test func readsBackSignedAsTheAppReadsIt() throws {
        let report = try APKAnalyzer.analyze(ZipArchive(data: APKSigner.sign(Self.unsigned(), with: Self.key)),
                                             preferredLanguages: ["en"])

        #expect(report.compatibility.status == .ready)
        #expect(report.runtime == .nativeActivity(library: "activity-test"))
        #expect(report.nativeLibraries.allSatisfy { $0.isZipAligned16K == true })
        #expect(report.signature.schemes == [.v2])
        #expect(report.signature.certificates.first?.subject?.contains("Minato Debug") == true)
    }

    @Test(arguments: [Data(), Data("not a ZIP archive at all, but long enough to have 22 bytes".utf8)])
    func refusesWhatIsNotAnArchive(data: Data) {
        #expect(throws: PackError.self) { try APKSigner.sign(data, with: Self.key) }
    }

    // MARK: - Debug key

    @Test func makesTheDebugKeyOnceAndKeepsItPrivate() throws {
        let folder = URL.temporaryDirectory.appending(path: "make-apk-key-\(UUID().uuidString)", directoryHint: .isDirectory)
        defer { try? FileManager.default.removeItem(at: folder) }

        let made = try DebugKey.load(from: folder)
        let again = try DebugKey.load(from: folder)

        #expect(again.certificate == made.certificate)
        let permissions = try FileManager.default.attributesOfItem(atPath: folder.appending(path: "key.der").path)[.posixPermissions]
        #expect(permissions as? Int == 0o600)
        let certificate = try #require(SecCertificateCreateWithData(nil, made.certificate as CFData))
        #expect(SecCertificateCopySubjectSummary(certificate) as String? == "Minato Debug")
        #expect(try FileManager.default.contentsOfDirectory(atPath: folder.path).sorted() == ["certificate.der", "key.der"])
    }

    @Test func refusesAKeyThatDoesNotBelongToItsCertificate() throws {
        let first = URL.temporaryDirectory.appending(path: "make-apk-key-\(UUID().uuidString)", directoryHint: .isDirectory)
        let second = URL.temporaryDirectory.appending(path: "make-apk-key-\(UUID().uuidString)", directoryHint: .isDirectory)
        defer {
            try? FileManager.default.removeItem(at: first)
            try? FileManager.default.removeItem(at: second)
        }
        _ = try DebugKey.load(from: first)
        _ = try DebugKey.load(from: second)
        try FileManager.default.removeItem(at: first.appending(path: "key.der"))
        try FileManager.default.copyItem(at: second.appending(path: "key.der"), to: first.appending(path: "key.der"))

        #expect(throws: PackError.self) { try DebugKey.load(from: first) }
    }

    // MARK: - Helpers

    /// Runs tools/apk-check.py, which recomputes the digest with Python and checks the signature with openssl.
    static func apkCheck(_ apk: Data) throws -> String {
        let file = URL.temporaryDirectory.appending(path: "make-apk-\(UUID().uuidString).apk")
        defer { try? FileManager.default.removeItem(at: file) }
        try apk.write(to: file)
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/env")
        process.arguments = ["python3", repository.appending(path: "tools/apk-check.py").path, file.path]
        let output = Pipe()
        process.standardOutput = output
        process.standardError = output
        try process.run()
        let text = String(decoding: output.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
        process.waitUntilExit()
        return text
    }
}

/// The v2 block of a signed APK with one signer, read here from the specification (source.android.com,
/// APK Signature Scheme v2), independently of APKKit and of the signer.
struct V2Block {
    /// Where the APK Signing Block starts and where it ends (the central directory's offset).
    var start = 0
    var end = 0
    var signedData = Data()
    var digestAlgorithms: [UInt32] = []
    var certificates: [Data] = []
    var additionalAttributes = Data()
    var signatureAlgorithms: [UInt32] = []
    var signature = Data()
    var publicKey = Data()

    init?(_ apk: Data) {
        let bytes = [UInt8](apk)
        func u32(_ data: [UInt8], _ at: Int) -> Int {
            (0..<4).reduce(0) { $0 | Int(data[at + $1]) << (8 * $1) }
        }
        func u64(_ at: Int) -> Int {
            (0..<8).reduce(0) { $0 | Int(bytes[at + $1]) << (8 * $1) }
        }
        /// The length-prefixed field at `at` in `data`; moves `at` past it.
        func field(_ data: [UInt8], _ at: inout Int) -> [UInt8] {
            let count = u32(data, at)
            defer { at += 4 + count }
            return Array(data[at + 4..<at + 4 + count])
        }
        /// The length-prefixed elements of a sequence.
        func elements(_ data: [UInt8]) -> [[UInt8]] {
            var at = 0
            var result: [[UInt8]] = []
            while at < data.count { result.append(field(data, &at)) }
            return result
        }

        // No comment after the end record: it is the last 22 bytes.
        guard bytes.count >= 22, u32(bytes, bytes.count - 22) == 0x0605_4B50 else { return nil }
        end = u32(bytes, bytes.count - 22 + 16)
        guard end >= 32, Array(bytes[end - 16..<end]) == Array("APK Sig Block 42".utf8) else { return nil }
        start = end - u64(end - 24) - 8
        guard u64(start) == u64(end - 24) else { return nil }

        var pair = start + 8
        while pair < end - 24 {
            let length = u64(pair)
            defer { pair += 8 + length }
            guard u32(bytes, pair + 8) == 0x7109_871A else { continue }
            let value = Array(bytes[pair + 12..<pair + 8 + length])
            var at = 0
            let signers = elements(field(value, &at))
            guard signers.count == 1 else { return nil }
            var inSigner = 0
            let signed = field(signers[0], &inSigner)
            let signatures = elements(field(signers[0], &inSigner))
            publicKey = Data(field(signers[0], &inSigner))
            signedData = Data(signed)
            var inSigned = 0
            let digests = elements(field(signed, &inSigned))
            certificates = elements(field(signed, &inSigned)).map { Data($0) }
            additionalAttributes = Data(field(signed, &inSigned))
            digestAlgorithms = digests.map { UInt32(u32($0, 0)) }
            signatureAlgorithms = signatures.map { UInt32(u32($0, 0)) }
            if let first = signatures.first {
                var inRecord = 4
                signature = Data(field(first, &inRecord))
            }
            return
        }
        return nil
    }
}
