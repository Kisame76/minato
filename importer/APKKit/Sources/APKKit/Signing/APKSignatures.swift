import CryptoKit
import Foundation
import Security

public struct SigningCertificate: Sendable, Equatable, Hashable, Codable {
    /// Lowercase hex SHA-256 of the DER encoding; what apksigner calls the certificate digest.
    public let sha256: String
    /// Subject summary, usually the common name.
    public let subject: String?

    public init(der: Data) {
        sha256 = SHA256.hash(data: der).map { String(format: "%02x", $0) }.joined()
        subject = SecCertificateCreateWithData(nil, der as CFData)
            .flatMap { SecCertificateCopySubjectSummary($0) as String? }
    }
}

public struct SignatureInfo: Sendable, Equatable, Codable {
    public enum Scheme: String, Sendable, Codable, CaseIterable {
        case v1
        case v2
        case v3
        case v31 = "v3.1"
    }

    public var schemes: [Scheme]
    /// Certificates of all signers, the newest scheme's signer first (after key rotation that is the current key).
    public var certificates: [SigningCertificate]
    /// The APK carries Google Play's source stamp, i.e. it was distributed through Play.
    public var hasGooglePlayStamp: Bool
    /// Who signed the APK, as Android 15 decides it; nil in a report written before it was recorded.
    public var signer: SignerIdentity?
}

/// Who signed an APK, as Android 15 decides it (ADR-0034, decision 2): the signers of the newest scheme
/// present, and for a rotated key its older certificates.
public struct SignerIdentity: Sendable, Equatable, Hashable, Codable {
    /// SHA-256 of each signer's certificate, sorted; empty for an unsigned APK.
    public var current: [String]
    /// SHA-256 of a rotated key's older certificates, oldest first (v3 and v3.1 with one signer only).
    public var lineage: [String]

    public init(current: [String], lineage: [String] = []) {
        self.current = current.sorted()
        self.lineage = lineage
    }

    /// The same signer, as Android decides an update: the same certificates, or one signer's current
    /// certificate in the other's lineage, a key rotated forward or back. Several signers must all match
    /// and none may rotate; an unsigned APK is nobody's.
    public func isSame(as other: SignerIdentity) -> Bool {
        guard !current.isEmpty, !other.current.isEmpty else { return false }
        if current == other.current { return true }
        guard current.count == 1, other.current.count == 1 else { return false }
        return other.lineage.contains(current[0]) || lineage.contains(other.current[0])
    }
}

/// Reads signing certificates. Signatures are not verified cryptographically here; this is about
/// knowing who signed the APK, because some apps check their own signature at runtime.
public enum APKSignatures {
    private static let blockIDs: [UInt32: SignatureInfo.Scheme] = [0x7109_871A: .v2, 0xF053_68C0: .v3, 0x1B93_AD61: .v31]
    private static let sourceStampIDs: Set<UInt32> = [0x6DFF_800D, 0x2B09_189E]
    private static let googlePlayStampSHA256 = "3257d599a49d2c961a471ca9843f59d341a405884583fc087df4237b733bbd6d"
    private static let proofOfRotationID: UInt32 = 0x3BA0_6F8C
    /// The Android version whose choice of signer counts: Android 15.
    static let platformSDK: UInt32 = 35

    public static func read(_ archive: ZipArchive) -> SignatureInfo {
        var found: [SignatureInfo.Scheme: [Data]] = [:]
        var signers: [SignatureInfo.Scheme: [Signer]] = [:]
        var hasPlayStamp = false

        for entry in archive.entries where isV1SignatureFile(entry.name) {
            let certificates = (try? PKCS7.certificates(in: archive.data(for: entry, maxSize: 1 << 20))) ?? []
            found[.v1, default: []] += certificates
            if let first = certificates.first { signers[.v1, default: []].append(Signer(certificates: [first])) }
        }

        for (id, value) in (try? signingBlockPairs(archive)) ?? [] {
            if let scheme = blockIDs[id] {
                let parsed = (try? parseSigners(value, isV3: scheme != .v2)) ?? []
                found[scheme, default: []] += parsed.flatMap(\.certificates)
                signers[scheme, default: []] += parsed
            } else if sourceStampIDs.contains(id), let certificate = try? stampCertificate(value) {
                hasPlayStamp = hasPlayStamp || SigningCertificate(der: certificate).sha256 == googlePlayStampSHA256
            }
        }

        var certificates: [SigningCertificate] = []
        for scheme in SignatureInfo.Scheme.allCases.reversed() {
            for der in found[scheme] ?? [] {
                let certificate = SigningCertificate(der: der)
                if !certificates.contains(where: { $0.sha256 == certificate.sha256 }) {
                    certificates.append(certificate)
                }
            }
        }
        return SignatureInfo(
            schemes: SignatureInfo.Scheme.allCases.filter { found[$0] != nil },
            certificates: certificates,
            hasGooglePlayStamp: hasPlayStamp,
            signer: identity(signers))
    }

    /// One signer of a scheme: its certificate chain, and for v3 and v3.1 the Android versions it is
    /// for and its lineage.
    struct Signer {
        var certificates: [Data]
        var minSdk: UInt32 = 0
        var maxSdk: UInt32 = .max
        var lineage: [Data] = []

        init(certificates: [Data]) {
            self.certificates = certificates
        }

        func isFor(sdk: UInt32) -> Bool { minSdk <= sdk && sdk <= maxSdk }
    }

    /// The newest scheme with a signer for Android 15 decides: v3.1, v3, v2, then v1.
    private static func identity(_ signers: [SignatureInfo.Scheme: [Signer]]) -> SignerIdentity {
        for scheme in SignatureInfo.Scheme.allCases.reversed() {
            let chosen = (signers[scheme] ?? []).filter { $0.isFor(sdk: platformSDK) && !$0.certificates.isEmpty }
            guard !chosen.isEmpty else { continue }
            let current = chosen.map { SigningCertificate(der: $0.certificates[0]).sha256 }
            var lineage: [String] = []
            if chosen.count == 1 {
                // The lineage ends with the signer's own certificate.
                lineage = chosen[0].lineage.map { SigningCertificate(der: $0).sha256 }.filter { $0 != current[0] }
            }
            return SignerIdentity(current: current, lineage: lineage)
        }
        return SignerIdentity(current: [])
    }

    private static func isV1SignatureFile(_ name: String) -> Bool {
        guard name.hasPrefix("META-INF/"), !name.dropFirst(9).contains("/") else { return false }
        let upper = name.uppercased()
        return upper.hasSuffix(".RSA") || upper.hasSuffix(".DSA") || upper.hasSuffix(".EC")
    }

    /// ID/value pairs of the APK Signing Block, which ends right before the central directory.
    static func signingBlockPairs(_ archive: ZipArchive) throws -> [(UInt32, Data)] {
        let centralDirectory = archive.centralDirectoryOffset
        guard centralDirectory >= 32 else { return [] }
        let footer = ByteReader(try archive.source.read(at: centralDirectory - 24, count: 24), context: "signing block footer")
        guard footer.data.suffix(16) == Data("APK Sig Block 42".utf8) else { return [] }
        let size = try footer.u64(at: 0)
        guard size >= 24, size <= 64 << 20, size + 8 <= centralDirectory else {
            throw APKError.invalidSigningBlock("size \(size)")
        }
        let block = ByteReader(try archive.source.read(at: centralDirectory - size - 8, count: Int(size + 8)), context: "signing block")
        guard try block.u64(at: 0) == size else { throw APKError.invalidSigningBlock("size fields differ") }

        var pairs = ByteReader(block.data.subdata(in: 8..<block.count - 24), context: "signing block pairs")
        var result: [(UInt32, Data)] = []
        while pairs.remaining >= 12 {
            let length = try pairs.u64()
            guard length >= 4, length <= UInt64(pairs.remaining) else { throw APKError.invalidSigningBlock("pair length \(length)") }
            let id = try pairs.u32()
            result.append((id, try pairs.bytes(Int(length) - 4)))
        }
        return result
    }

    /// Every signer in a v2/v3/v3.1 block. v3 signed data starts like v2's, digests, then certificates,
    /// and goes on with the Android versions and the additional attributes, among them the lineage.
    private static func parseSigners(_ value: Data, isV3: Bool) throws -> [Signer] {
        var outer = ByteReader(value, context: "signers")
        var signers = ByteReader(try prefixed(&outer), context: "signers")
        var result: [Signer] = []
        while signers.remaining >= 4 {
            var signer = ByteReader(try prefixed(&signers), context: "signer")
            var signedData = ByteReader(try prefixed(&signer), context: "signed data")
            _ = try prefixed(&signedData) // digests
            var encoded = ByteReader(try prefixed(&signedData), context: "certificates")
            var certificates: [Data] = []
            while encoded.remaining >= 4 {
                certificates.append(try prefixed(&encoded))
            }
            var parsed = Signer(certificates: certificates)
            if isV3 {
                // A damaged rest keeps the certificates, as before the lineage was read.
                parsed.minSdk = (try? signer.u32()) ?? 0
                parsed.maxSdk = (try? signer.u32()) ?? .max
                parsed.lineage = (try? lineage(afterCertificates: &signedData)) ?? []
            }
            result.append(parsed)
        }
        return result
    }

    /// The lineage among a v3 signer's additional attributes; empty without one.
    private static func lineage(afterCertificates signedData: inout ByteReader) throws -> [Data] {
        _ = try signedData.u32() // the Android versions, as in the signer
        _ = try signedData.u32()
        var attributes = ByteReader(try prefixed(&signedData), context: "additional attributes")
        while attributes.remaining >= 4 {
            var attribute = ByteReader(try prefixed(&attributes), context: "attribute")
            if try attribute.u32() == proofOfRotationID {
                return try proofOfRotation(&attribute)
            }
        }
        return []
    }

    /// The certificates of a proof of rotation, oldest first: a version, then one level per key, each
    /// with the certificate in its signed data.
    private static func proofOfRotation(_ reader: inout ByteReader) throws -> [Data] {
        _ = try reader.u32() // version
        var certificates: [Data] = []
        while reader.remaining >= 4 {
            var level = ByteReader(try prefixed(&reader), context: "rotation level")
            var signedData = ByteReader(try prefixed(&level), context: "rotation signed data")
            certificates.append(try prefixed(&signedData))
        }
        return certificates
    }

    private static func stampCertificate(_ value: Data) throws -> Data {
        var outer = ByteReader(value, context: "source stamp")
        var stamp = ByteReader(try prefixed(&outer), context: "source stamp")
        return try prefixed(&stamp)
    }

    private static func prefixed(_ reader: inout ByteReader) throws -> Data {
        try reader.bytes(Int(try reader.u32()))
    }
}
