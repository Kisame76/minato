import APKKit
import CryptoKit
import Foundation
import Security

/// A key that signs test APKs, with its certificate. `@unchecked` because a SecKey never changes.
public struct TestKey: @unchecked Sendable {
    public enum Kind: Sendable { case rsa, ec, dsa }

    public let certificate: Data
    public let kind: Kind
    /// Nil for DSA: Security.framework has no DSA, so a DSA signature is made of random bytes.
    let privateKey: SecKey?

    public var sha256: String { SigningCertificate(der: certificate).sha256 }

    public static let ec = TestKey(ec: TestCertificates.signer, pkcs8: TestCertificates.signerKey)
    public static let rotated = TestKey(ec: TestCertificates.rotatedSigner, pkcs8: TestCertificates.rotatedSignerKey)
    public static let rsa = TestKey(certificate: TestCertificates.rsaSigner, kind: .rsa, privateKey: SecKeyCreateWithData(
        TestCertificates.rsaSignerKey as CFData,
        [kSecAttrKeyType: kSecAttrKeyTypeRSA, kSecAttrKeyClass: kSecAttrKeyClassPrivate] as CFDictionary, nil)!)
    public static let dsa = TestKey(certificate: TestCertificates.dsaSigner, kind: .dsa, privateKey: nil)

    init(certificate: Data, kind: Kind, privateKey: SecKey?) {
        self.certificate = certificate
        self.kind = kind
        self.privateKey = privateKey
    }

    init(ec certificate: Data, pkcs8: Data) {
        let key = try! P256.Signing.PrivateKey(derRepresentation: pkcs8)
        let attributes = [kSecAttrKeyType: kSecAttrKeyTypeECSECPrimeRandom, kSecAttrKeyClass: kSecAttrKeyClassPrivate]
        self.init(certificate: certificate, kind: .ec,
                  privateKey: SecKeyCreateWithData(key.x963Representation as CFData, attributes as CFDictionary, nil)!)
    }

    /// The key make-apk signs with (RSA).
    public init(debugKey: DebugKey) {
        self.init(certificate: debugKey.certificate, kind: .rsa, privateKey: debugKey.privateKey)
    }

    /// The v2/v3 signature algorithm this key signs with by default.
    public var defaultAlgorithm: UInt32 {
        switch kind {
        case .rsa: TestSigner.rsaPKCS1SHA256
        case .ec: TestSigner.ecdsaSHA256
        case .dsa: TestSigner.dsaSHA256
        }
    }

    /// A signature over `message` with the v2/v3 algorithm `algorithm`; random bytes for DSA and for
    /// an algorithm that does not fit the key.
    public func sign(_ message: Data, algorithm: UInt32) -> Data {
        remembered("\(algorithm)", message) {
            guard let privateKey, let secAlgorithm = Self.secAlgorithm(algorithm, kind) else {
                return Data((0..<64).map { _ in UInt8.random(in: 0...255) })
            }
            return SecKeyCreateSignature(privateKey, secAlgorithm, message as CFData, nil)! as Data
        }
    }

    private static let lock = NSLock()
    nonisolated(unsafe) private static var signatures: [Data: Data] = [:]

    /// ECDSA signs with a random nonce, so the same APK would come out different each time; tests
    /// that import the same bytes twice need them equal, so each signature is made once per run.
    private func remembered(_ algorithm: String, _ message: Data, _ make: () -> Data) -> Data {
        let id = Data(SHA256.hash(data: certificate + Data(algorithm.utf8) + [0] + message))
        Self.lock.lock()
        defer { Self.lock.unlock() }
        if let signature = Self.signatures[id] { return signature }
        let signature = make()
        Self.signatures[id] = signature
        return signature
    }

    /// A signature over `message` with a digest as the manifest names it ("SHA1", "SHA-256", ...), for PKCS#7.
    public func sign(_ message: Data, digest: String) -> Data {
        let algorithms: [String: (rsa: SecKeyAlgorithm, ec: SecKeyAlgorithm)] = [
            "SHA1": (.rsaSignatureMessagePKCS1v15SHA1, .ecdsaSignatureMessageX962SHA1),
            "SHA-256": (.rsaSignatureMessagePKCS1v15SHA256, .ecdsaSignatureMessageX962SHA256),
            "SHA-384": (.rsaSignatureMessagePKCS1v15SHA384, .ecdsaSignatureMessageX962SHA384),
            "SHA-512": (.rsaSignatureMessagePKCS1v15SHA512, .ecdsaSignatureMessageX962SHA512),
        ]
        return remembered(digest, message) {
            guard let privateKey, let pair = algorithms[digest] else {
                return Data((0..<64).map { _ in UInt8.random(in: 0...255) })
            }
            return SecKeyCreateSignature(privateKey, kind == .rsa ? pair.rsa : pair.ec, message as CFData, nil)! as Data
        }
    }

    static func secAlgorithm(_ id: UInt32, _ kind: Kind) -> SecKeyAlgorithm? {
        switch (id, kind) {
        case (TestSigner.rsaPSSSHA256, .rsa): .rsaSignatureMessagePSSSHA256
        case (TestSigner.rsaPSSSHA512, .rsa): .rsaSignatureMessagePSSSHA512
        case (TestSigner.rsaPKCS1SHA256, .rsa), (TestSigner.rsaPKCS1SHA256Verity, .rsa): .rsaSignatureMessagePKCS1v15SHA256
        case (TestSigner.rsaPKCS1SHA512, .rsa): .rsaSignatureMessagePKCS1v15SHA512
        case (TestSigner.ecdsaSHA256, .ec), (TestSigner.ecdsaSHA256Verity, .ec): .ecdsaSignatureMessageX962SHA256
        case (TestSigner.ecdsaSHA512, .ec): .ecdsaSignatureMessageX962SHA512
        default: nil
        }
    }
}
