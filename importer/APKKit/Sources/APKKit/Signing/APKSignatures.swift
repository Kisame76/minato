import CryptoKit
import Foundation
import Security

public struct SigningCertificate: Sendable, Equatable, Hashable, Codable {
    /// Lowercase hex SHA-256 of the DER encoding; what apksigner calls the certificate digest.
    public let sha256: String
    /// Subject summary, usually the common name.
    public let subject: String?

    init(der: Data) {
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
}

/// Reads signing certificates. Signatures are not verified cryptographically here; this is about
/// knowing who signed the APK, because some apps check their own signature at runtime.
public enum APKSignatures {
    private static let blockIDs: [UInt32: SignatureInfo.Scheme] = [0x7109_871A: .v2, 0xF053_68C0: .v3, 0x1B93_AD61: .v31]
    private static let sourceStampIDs: Set<UInt32> = [0x6DFF_800D, 0x2B09_189E]
    private static let googlePlayStampSHA256 = "3257d599a49d2c961a471ca9843f59d341a405884583fc087df4237b733bbd6d"

    public static func read(_ archive: ZipArchive) -> SignatureInfo {
        var found: [SignatureInfo.Scheme: [Data]] = [:]
        var hasPlayStamp = false

        for entry in archive.entries where isV1SignatureFile(entry.name) {
            found[.v1, default: []] += (try? PKCS7.certificates(in: archive.data(for: entry, maxSize: 1 << 20))) ?? []
        }

        for (id, value) in (try? signingBlockPairs(archive)) ?? [] {
            if let scheme = blockIDs[id] {
                found[scheme, default: []] += (try? signerCertificates(value)) ?? []
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
            hasGooglePlayStamp: hasPlayStamp)
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

    /// Certificates of every signer in a v2/v3/v3.1 block. v3 signed data starts like v2's:
    /// digests, then certificates.
    private static func signerCertificates(_ value: Data) throws -> [Data] {
        var outer = ByteReader(value, context: "signers")
        var signers = ByteReader(try prefixed(&outer), context: "signers")
        var certificates: [Data] = []
        while signers.remaining >= 4 {
            var signer = ByteReader(try prefixed(&signers), context: "signer")
            var signedData = ByteReader(try prefixed(&signer), context: "signed data")
            _ = try prefixed(&signedData) // digests
            var encoded = ByteReader(try prefixed(&signedData), context: "certificates")
            while encoded.remaining >= 4 {
                certificates.append(try prefixed(&encoded))
            }
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
