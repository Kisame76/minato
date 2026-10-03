import Foundation

/// What checking an APK's signatures found (ADR-0034, step d).
public enum SignatureCheck: Sendable, Equatable, Hashable, Codable {
    /// The scheme that names the signer (decision 2) verifies, and so does the content it covers.
    case verified
    /// No signature at all.
    case unsigned
    /// Signed with an algorithm Security.framework cannot check, such as DSA or a verity digest only:
    /// neither damaged nor proven.
    case unverified(String)
    /// Damaged or forged: the file is not installed.
    case failed(SignatureFailure)

    public var isFailure: Bool {
        if case .failed = self { true } else { false }
    }
}

/// Why a signature check failed.
public struct SignatureFailure: Error, Sendable, Equatable, Hashable, Codable {
    public enum Kind: String, Sendable, Codable {
        /// The content does not match its digests, or the file's structure is broken.
        case damaged
        /// A signature does not verify with its key, or a newer signature was removed.
        case signatureMismatch
        /// Only v1, which Android refuses from targetSdkVersion 30 (Android 11) on.
        case noV2Signature
    }

    public var kind: Kind
    /// What exactly, in English, for the details and the tests.
    public var detail: String

    public init(_ kind: Kind, _ detail: String) {
        self.kind = kind
        self.detail = detail
    }
}

/// The result of `SignatureVerifier.verify`: the check, and what it proved.
public struct SignatureVerification: Sendable, Equatable {
    public var check: SignatureCheck
    /// What the verified scheme proved; nil unless `check` is `.verified`.
    public var proof: SignerProof?

    public init(check: SignatureCheck, proof: SignerProof? = nil) {
        self.check = check
        self.proof = proof
    }

    /// Who signed the APK, as the proof says; nil unless `check` is `.verified`.
    public var signer: SignerIdentity? { proof?.identity }
}

/// What a verified check proved (ADR-0034 decision 2): the scheme that decided, the certificate of each of
/// its signers, and one v3 or v3.1 signer's proof of rotation. signers.json holds exactly this.
public struct SignerProof: Sendable, Equatable, Hashable, Codable {
    /// A level of the proof of rotation: its certificate and the capabilities the rotation left it.
    public struct Level: Sendable, Equatable, Hashable, Codable {
        public var certificate: Data
        public var flags: UInt32

        public init(certificate: Data, flags: UInt32) {
            self.certificate = certificate
            self.flags = flags
        }
    }

    public var scheme: SignatureInfo.Scheme
    /// Each signer's own certificate, DER, in the APK's order: for v1 the one its signature block's
    /// SignerInfo names, for v2, v3 and v3.1 the first one the signer lists.
    public var certificates: [Data]
    /// Oldest first, the signer's own certificate last; empty without a rotation.
    public var lineage: [Level]

    public init(scheme: SignatureInfo.Scheme, certificates: [Data], lineage: [Level] = []) {
        self.scheme = scheme
        self.certificates = certificates
        self.lineage = lineage
    }

    /// The signer as the update policy compares it: each certificate's SHA-256, and the lineage's older ones.
    public var identity: SignerIdentity {
        SignerIdentity(current: certificates.map { SigningCertificate.sha256(of: $0) },
                       lineage: lineage.dropLast().map {
                           LineageNode(sha256: SigningCertificate.sha256(of: $0.certificate), flags: $0.flags)
                       })
    }
}
