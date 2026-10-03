import Foundation

/// signers.json (ADR-0034 decision 2, ADR-0039 b3.5b1): who signed the APK, written into each version's folder,
/// from which the guest's PackageManager answers (framework/java's SignersFile reads it). It holds exactly what the
/// signature check proved (SignerProof), so the guest parses no part of the APK's signatures:
///
///     {"format":1,"lineage":[{"certificate":"<base64 DER>","flags":N},...],"scheme":3,"signers":["<base64 DER>",...]}
///
/// - scheme: the one decision 2 chose for Android 15, 1 (JAR), 2, 3 or 31 (v3.1);
/// - signers: each of that scheme's signers' own certificate, in the APK's order;
/// - lineage: one v3 or v3.1 signer's verified proof of rotation, oldest first and the signer's certificate last,
///   with each level's capabilities as the int Android reads; empty without a rotation.
///
/// Only a verified APK has the file: an unsigned or unverified one has none, so the guest sees it unsigned. The
/// keys come sorted and nothing is escaped or spaced, the one form the guest's reader accepts.
public enum SignersFile {
    public static let fileName = "signers.json"
    public static let format = 1
    /// What the guest's reader takes at most (SignersFile.MAX_BYTES and MAX_ENTRIES), so that every file written
    /// reads.
    public static let maximumBytes = 1 << 20
    public static let maximumEntries = 64

    struct Content: Encodable {
        struct Node: Encodable {
            var certificate: String
            var flags: Int32
        }

        var format: Int
        var scheme: Int
        var signers: [String]
        var lineage: [Node]
    }

    /// A proof the guest's reader would refuse as a file.
    public struct Refusal: Error, Equatable {
        public var reason: String
    }

    /// The file's bytes for what a check proved.
    public static func content(of proof: SignerProof) throws -> Data {
        if let reason = problem(with: proof) { throw Refusal(reason: reason) }
        return try encode(proof)
    }

    /// Why the guest's reader would refuse the file of this proof; nil when it reads.
    public static func problem(with proof: SignerProof) -> String? {
        guard !proof.certificates.isEmpty else { return "no signer" }
        for (name, count) in [("signers", proof.certificates.count), ("lineage levels", proof.lineage.count)]
        where count > maximumEntries {
            return "\(count) \(name), more than the \(maximumEntries) Minato records"
        }
        guard (proof.certificates + proof.lineage.map(\.certificate)).allSatisfy({ $0.first == 0x30 }) else {
            return "a certificate that is not DER"
        }
        if !proof.lineage.isEmpty {
            guard proof.scheme == .v3 || proof.scheme == .v31, proof.certificates.count == 1 else {
                return "a lineage, which belongs to one signer of v3 or v3.1, not to \(proof.certificates.count) of "
                    + proof.scheme.rawValue
            }
            guard proof.lineage.last?.certificate == proof.certificates[0] else {
                return "a lineage that does not end with the signer's certificate"
            }
            guard Set(proof.lineage.map(\.certificate)).count == proof.lineage.count else {
                return "a certificate twice in the lineage"
            }
        }
        guard let size = try? encode(proof).count, size <= maximumBytes else {
            return "certificates larger than the 1 MiB Minato records"
        }
        return nil
    }

    static func encode(_ proof: SignerProof) throws -> Data {
        let content = Content(
            format: format, scheme: number(of: proof.scheme),
            signers: proof.certificates.map { $0.base64EncodedString() },
            lineage: proof.lineage.map {
                Content.Node(certificate: $0.certificate.base64EncodedString(), flags: Int32(bitPattern: $0.flags))
            })
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
        return try encoder.encode(content)
    }

    private static func number(of scheme: SignatureInfo.Scheme) -> Int {
        switch scheme {
        case .v1: 1
        case .v2: 2
        case .v3: 3
        case .v31: 31
        }
    }
}
