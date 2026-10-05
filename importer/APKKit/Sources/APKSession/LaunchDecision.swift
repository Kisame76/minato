import APKKit
import APKLibrary

/// What signs converted code now (ADR-0023): the imported certificate once it passed its self-test, or
/// ad hoc in the Simulator; nothing yet otherwise.
public enum CurrentSigner: Equatable, Sendable {
    case certificate(sha256: String)
    case adHoc
    case none
    /// Nothing yet, and the first look at the certificate after launch has not ended: it may still
    /// bring a signer without a tap, e.g. from the certificate SideStore embeds.
    case pending

    /// What `signedWith` records for code this signer signed.
    public var fingerprint: String? {
        switch self {
        case .certificate(let sha256): sha256
        case .adHoc: "ad hoc"
        case .none, .pending: nil
        }
    }
}

/// What one tap on an app in the library does (ADR-0032, decision 3).
public enum LaunchDecision: Equatable, Sendable {
    /// Why the app does not start.
    public enum Reason: Equatable, Sendable {
        case updating
        case needsSplits
        case incompatible
        case invalid
    }

    case start
    /// Sign the app's native code with the current signer, then start without another tap.
    case signFirst
    case openSigning
    /// Wait until the first look at the certificate ends, then decide again.
    case waitForSigner
    case explain(Reason)

    /// An app that cannot start says why before anything else, so it needs no certificate; a ready app, a
    /// NativeActivity app or one with Java code (ADR-0039 c5), starts once the current signer signed its native code.
    public init(entry: LibraryEntry, isUpdating: Bool, signer: CurrentSigner) {
        switch entry.state {
        case .invalid:
            self = .explain(.invalid)
        case .incompatible:
            self = .explain(.incompatible)
        case .needsSplits:
            self = .explain(.needsSplits)
        case .ready:
            if isUpdating {
                self = .explain(.updating)
            } else if let fingerprint = signer.fingerprint {
                self = entry.signedWith == fingerprint ? .start : .signFirst
            } else if signer == .pending {
                self = .waitForSigner
            } else {
                self = .openSigning
            }
        }
    }
}
