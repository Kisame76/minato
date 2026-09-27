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
    }

    public var kind: Kind
    /// What exactly, in English, for the details and the tests.
    public var detail: String

    public init(_ kind: Kind, _ detail: String) {
        self.kind = kind
        self.detail = detail
    }
}

/// The result of `SignatureVerifier.verify`: the check, and the signer it proved.
public struct SignatureVerification: Sendable, Equatable {
    public var check: SignatureCheck
    /// Who signed the APK, as the verified scheme says; nil unless `check` is `.verified`.
    public var signer: SignerIdentity?

    public init(check: SignatureCheck, signer: SignerIdentity? = nil) {
        self.check = check
        self.signer = signer
    }
}
