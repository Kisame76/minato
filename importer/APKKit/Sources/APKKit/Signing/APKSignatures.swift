import CryptoKit
import Foundation
import Security

public struct SigningCertificate: Sendable, Equatable, Hashable, Codable {
    /// Lowercase hex SHA-256 of the DER encoding; what apksigner calls the certificate digest.
    public let sha256: String
    /// Subject summary, usually the common name.
    public let subject: String?

    public init(der: Data) {
        sha256 = Self.sha256(of: der)
        subject = SecCertificateCreateWithData(nil, der as CFData)
            .flatMap { SecCertificateCopySubjectSummary($0) as String? }
    }

    /// Lowercase hex SHA-256 of a DER encoding.
    public static func sha256(of der: Data) -> String {
        SHA256.hash(data: der).map { String(format: "%02x", $0) }.joined()
    }

    init(sha256: String, subject: String?) {
        self.sha256 = sha256
        self.subject = subject
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
    /// The APK carries a source stamp with Google Play's certificate. The stamp's signature is not
    /// verified, so it does not prove that the file came through Play.
    public var hasGooglePlayStamp: Bool
    /// Who signed the APK, as Android 15 decides it; nil in a report written before it was recorded.
    public var signer: SignerIdentity?
    /// What the cryptographic check found; nil in a report written before it (ADR-0034, step d).
    public var check: SignatureCheck?
    /// What the check proved, which signers.json holds (ADR-0034 decision 2); nil unless `check` is
    /// `.verified`, and in a report written before it was recorded.
    public var proof: SignerProof?

    /// The signer as the details show it (ADR-0034, step d): proven only when the check verified, and then
    /// with the certificates it proved.
    public var signerDetails: SignerDetails {
        if check == .verified, let proof {
            var seen: Set<Data> = []
            return SignerDetails(isProven: true,
                                 certificates: proof.certificates.filter { seen.insert($0).inserted }.map(SigningCertificate.init(der:)),
                                 earlier: proof.lineage.dropLast().map { SigningCertificate(der: $0.certificate) })
        }
        guard let signer else {
            return SignerDetails(isProven: false, certificates: certificates, earlier: [])
        }
        let known = Dictionary(certificates.map { ($0.sha256, $0) }, uniquingKeysWith: { first, _ in first })
        let certificate = { (sha256: String) in known[sha256] ?? SigningCertificate(sha256: sha256, subject: nil) }
        return SignerDetails(isProven: check == .verified, certificates: signer.current.map(certificate),
                             earlier: signer.lineage.map { certificate($0.sha256) })
    }
}

/// An APK's signer for the details (ADR-0034, step d).
public struct SignerDetails: Sendable, Equatable {
    /// The signature check verified the signer; otherwise the certificates are only named by the file.
    public var isProven: Bool
    /// The signer's certificates, one per signer of the newest scheme.
    public var certificates: [SigningCertificate]
    /// A rotated key's older certificates, oldest first.
    public var earlier: [SigningCertificate]
}

/// Who signed an APK, as Android 15 decides it (ADR-0034, decision 2): the signers of the newest scheme
/// present, and for a rotated key its older certificates.
public struct SignerIdentity: Sendable, Equatable, Hashable, Codable {
    /// SHA-256 of each signer's certificate, sorted; empty for an unsigned APK.
    public var current: [String]
    /// A rotated key's older certificates, oldest first (v3 and v3.1 with one signer only).
    public var lineage: [LineageNode]

    public init(current: [String], lineage: [LineageNode] = []) {
        // Android compares signers as sets: the same certificate twice is one signer.
        self.current = Set(current).sorted()
        self.lineage = lineage
    }

    /// Whether an APK of this signer may take over the data of an app `installed` signed, as Android
    /// decides an update (ADR-0034, step d): the same certificates; a key rotated since, whose lineage
    /// grants the installed key INSTALLED_DATA; or a key back to an older one, which the installed
    /// lineage grants ROLLBACK. Several signers must all match and none may rotate; an unsigned APK is
    /// nobody's.
    public func canUpdate(_ installed: SignerIdentity) -> Bool {
        guard !current.isEmpty, !installed.current.isEmpty else { return false }
        if current == installed.current { return true }
        guard current.count == 1, installed.current.count == 1 else { return false }
        if lineage.contains(where: { $0.sha256 == installed.current[0] && $0.grants(LineageNode.installedData) }) {
            return true
        }
        return installed.lineage.contains { $0.sha256 == current[0] && $0.grants(LineageNode.rollback) }
    }

    /// Whether every older certificate comes with its capabilities; a report from step b) has none.
    public var knowsCapabilities: Bool { lineage.allSatisfy { $0.flags != nil } }

    private enum CodingKeys: String, CodingKey { case current, lineage }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        current = try container.decode([String].self, forKey: .current)
        if let nodes = try? container.decode([LineageNode].self, forKey: .lineage) {
            lineage = nodes
        } else {
            // Written by step b): the certificates only.
            lineage = try container.decode([String].self, forKey: .lineage).map { LineageNode(sha256: $0, flags: nil) }
        }
    }
}

/// An older certificate of a rotated key and what Android still lets it do (SigningCertificateLineage).
public struct LineageNode: Sendable, Equatable, Hashable, Codable {
    /// Apps signed with it may be updated by the rotated key and keep their data.
    public static let installedData: UInt32 = 1
    /// An APK signed with it may replace an app of the rotated key.
    public static let rollback: UInt32 = 8

    public var sha256: String
    /// Nil in a report written before the capabilities were recorded.
    public var flags: UInt32?

    public init(sha256: String, flags: UInt32?) {
        self.sha256 = sha256
        self.flags = flags
    }

    public func grants(_ capability: UInt32) -> Bool {
        guard let flags else { return false }
        return flags & capability == capability
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
    /// The most certificates, lineage levels, algorithms or attributes a signer may list, and pairs a
    /// signing block may hold: far above a real APK's one to three certificates and handful of pairs.
    static let maximumListLength = 64

    public static func read(_ archive: ZipArchive) -> SignatureInfo {
        let (found, signers, hasPlayStamp) = collect(archive)
        var certificates: [SigningCertificate] = []
        var seen: Set<Data> = []
        for scheme in SignatureInfo.Scheme.allCases.reversed() {
            for der in found[scheme] ?? [] where seen.insert(der).inserted {
                certificates.append(SigningCertificate(der: der))
            }
        }
        return SignatureInfo(
            schemes: SignatureInfo.Scheme.allCases.filter { found[$0] != nil },
            certificates: certificates,
            hasGooglePlayStamp: hasPlayStamp,
            signer: identity(signers))
    }

    /// The SHA-256 of each signer's certificate in the scheme that decides for Android 15, as `read`'s signer, but in
    /// the APK's order and with a certificate named twice kept twice, as Android's exact match counts them
    /// (`Signature.java:331-334`). Unproven: ADR-0048's set check compares what an APK Minato cannot verify names.
    public static func signerCertificates(_ archive: ZipArchive) -> [String] {
        deciding(collect(archive).signers).map { SigningCertificate(der: $0.certificates[0]).sha256 }
    }

    /// Every scheme's certificates and signers, and whether Google Play's source stamp is there.
    private static func collect(_ archive: ZipArchive)
        -> (found: [SignatureInfo.Scheme: [Data]], signers: [SignatureInfo.Scheme: [Signer]], hasPlayStamp: Bool) {
        var found: [SignatureInfo.Scheme: [Data]] = [:]
        var signers: [SignatureInfo.Scheme: [Signer]] = [:]
        var hasPlayStamp = false

        // No more signature blocks than the check reads.
        for entry in archive.entries.lazy.filter({ isV1SignatureFile($0.name) }).prefix(JarVerifier.maximumSigners) {
            let certificates = ((try? PKCS7.certificates(in: archive.data(for: entry, maxSize: 1 << 20))) ?? []).prefix(maximumListLength)
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
        return (found, signers, hasPlayStamp)
    }

    /// One signer of a scheme: its certificate chain, and for v3 and v3.1 the Android versions it is
    /// for and its lineage.
    struct Signer {
        var certificates: [Data]
        var minSdk: UInt32 = 0
        var maxSdk: UInt32 = .max
        var lineage: [(certificate: Data, flags: UInt32)] = []

        init(certificates: [Data]) {
            self.certificates = certificates
        }

        func isFor(sdk: UInt32) -> Bool { minSdk <= sdk && sdk <= maxSdk }
    }

    /// The newest scheme with a signer for Android 15 decides: v3.1, v3, v2, then v1. Its signers, or none.
    private static func deciding(_ signers: [SignatureInfo.Scheme: [Signer]]) -> [Signer] {
        for scheme in SignatureInfo.Scheme.allCases.reversed() {
            let chosen = (signers[scheme] ?? []).filter { $0.isFor(sdk: platformSDK) && !$0.certificates.isEmpty }
            if !chosen.isEmpty { return chosen }
        }
        return []
    }

    private static func identity(_ signers: [SignatureInfo.Scheme: [Signer]]) -> SignerIdentity {
        let chosen = deciding(signers)
        guard !chosen.isEmpty else { return SignerIdentity(current: []) }
        let current = chosen.map { SigningCertificate(der: $0.certificates[0]).sha256 }
        var lineage: [LineageNode] = []
        if chosen.count == 1 {
            // The lineage ends with the signer's own certificate.
            lineage = chosen[0].lineage
                .map { LineageNode(sha256: SigningCertificate(der: $0.certificate).sha256, flags: $0.flags) }
                .filter { $0.sha256 != current[0] }
        }
        return SignerIdentity(current: current, lineage: lineage)
    }

    /// A v1 signature block, as StrictJarVerifier finds them: case matters, and META-INF/'s folders count.
    static func isV1SignatureFile(_ name: String) -> Bool {
        let utf8 = Array(name.utf8)
        guard utf8.starts(with: "META-INF/".utf8) else { return false }
        return [".RSA", ".DSA", ".EC"].contains { utf8.reversed().starts(with: $0.utf8.reversed()) }
    }

    /// ID/value pairs of the APK Signing Block, which ends right before the central directory.
    static func signingBlockPairs(_ archive: ZipArchive) throws -> [(UInt32, Data)] {
        try signingBlock(archive)?.pairs ?? []
    }

    /// The APK Signing Block: where it starts, and its ID/value pairs; nil without one.
    static func signingBlock(_ archive: ZipArchive) throws -> (offset: UInt64, pairs: [(UInt32, Data)])? {
        let centralDirectory = archive.centralDirectoryOffset
        guard centralDirectory >= 32 else { return nil }
        let footer = ByteReader(try archive.source.read(at: centralDirectory - 24, count: 24), context: "signing block footer")
        guard footer.data.suffix(16) == Data("APK Sig Block 42".utf8) else { return nil }
        let size = try footer.u64(at: 0)
        guard size >= 24, size <= 64 << 20, size + 8 <= centralDirectory else {
            throw APKError.invalidSigningBlock("size \(size)")
        }
        let offset = centralDirectory - size - 8
        let block = ByteReader(try archive.source.read(at: offset, count: Int(size + 8)), context: "signing block")
        guard try block.u64(at: 0) == size else { throw APKError.invalidSigningBlock("size fields differ") }

        var pairs = ByteReader(block.data.subdata(in: 8..<block.count - 24), context: "signing block pairs")
        var result: [(UInt32, Data)] = []
        while pairs.remaining >= 12 {
            guard result.count < maximumListLength else { throw APKError.invalidSigningBlock("more than \(maximumListLength) pairs") }
            let length = try pairs.u64()
            guard length >= 4, length <= UInt64(pairs.remaining) else { throw APKError.invalidSigningBlock("pair length \(length)") }
            let id = try pairs.u32()
            result.append((id, try pairs.bytes(Int(length) - 4)))
        }
        return (offset, result)
    }

    /// Every signer in a v2/v3/v3.1 block. v3 signed data starts like v2's, digests, then certificates,
    /// and goes on with the Android versions and the additional attributes, among them the lineage.
    private static func parseSigners(_ value: Data, isV3: Bool) throws -> [Signer] {
        var outer = ByteReader(value, context: "signers")
        var signers = ByteReader(try prefixed(&outer), context: "signers")
        var result: [Signer] = []
        while signers.remaining >= 4, result.count < JarVerifier.maximumSigners {
            var signer = ByteReader(try prefixed(&signers), context: "signer")
            var signedData = ByteReader(try prefixed(&signer), context: "signed data")
            _ = try prefixed(&signedData) // digests
            var encoded = ByteReader(try prefixed(&signedData), context: "certificates")
            var certificates: [Data] = []
            while encoded.remaining >= 4, certificates.count < maximumListLength {
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
    private static func lineage(afterCertificates signedData: inout ByteReader) throws -> [(certificate: Data, flags: UInt32)] {
        _ = try signedData.u32() // the Android versions, as in the signer
        _ = try signedData.u32()
        var attributes = ByteReader(try prefixed(&signedData), context: "additional attributes")
        for _ in 0..<maximumListLength where attributes.remaining >= 4 {
            var attribute = ByteReader(try prefixed(&attributes), context: "attribute")
            if try attribute.u32() == proofOfRotationID {
                return try proofOfRotation(&attribute)
            }
        }
        return []
    }

    /// The certificates of a proof of rotation with their capabilities, oldest first: a version, then
    /// one level per key, each with the certificate in its signed data and its flags after them.
    private static func proofOfRotation(_ reader: inout ByteReader) throws -> [(certificate: Data, flags: UInt32)] {
        _ = try reader.u32() // version
        var levels: [(certificate: Data, flags: UInt32)] = []
        while reader.remaining >= 4, levels.count < maximumListLength {
            var level = ByteReader(try prefixed(&reader), context: "rotation level")
            var signedData = ByteReader(try prefixed(&level), context: "rotation signed data")
            levels.append((try prefixed(&signedData), try level.u32()))
        }
        return levels
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
