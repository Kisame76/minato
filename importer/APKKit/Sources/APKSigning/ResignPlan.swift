/// A library entry as far as re-signing cares: how much converted code it has and which
/// certificate signed it (a SHA-256 fingerprint, as in `CertificateInfo`).
public struct ResignCandidate: Sendable, Equatable {
    public var id: String
    public var signedWith: String?
    public var containerCount: Int

    public init(id: String, signedWith: String?, containerCount: Int) {
        self.id = id
        self.signedWith = signedWith
        self.containerCount = containerCount
    }
}

/// The entries whose converted code iOS would no longer load: signed with another certificate than
/// `current`, or not recorded at all (ADR-0023). Entries without converted code have nothing to sign.
public func entriesToResign(_ candidates: [ResignCandidate], current sha256: String) -> [String] {
    candidates.filter { $0.containerCount > 0 && $0.signedWith != sha256 }.map(\.id)
}
