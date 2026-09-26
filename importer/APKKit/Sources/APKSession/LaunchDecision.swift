import APKKit
import APKLibrary

/// What signs converted code now (ADR-0023): the imported certificate once it passed its self-test, or
/// ad hoc in the Simulator; nothing yet otherwise.
public enum CurrentSigner: Equatable, Sendable {
    case certificate(sha256: String)
    case adHoc
    case none

    /// What `signedWith` records for code this signer signed.
    public var fingerprint: String? {
        switch self {
        case .certificate(let sha256): sha256
        case .adHoc: "ad hoc"
        case .none: nil
        }
    }
}

/// What one tap on an app in the library does (ADR-0032, decision 3).
public enum LaunchDecision: Equatable, Sendable {
    /// Why the app does not start.
    public enum Reason: Equatable, Sendable {
        case needsART
        case updating
        case needsSplits
        case incompatible
        case invalid
    }

    case start
    /// Sign the app's native code with the current signer, then start without another tap.
    case signFirst
    case openSigning
    case explain(Reason)

    /// An app that cannot start says why before anything else, so it needs no certificate; a ready
    /// NativeActivity app starts once the current signer signed its code.
    public init(entry: LibraryEntry, isUpdating: Bool, signer: CurrentSigner) {
        switch entry.state {
        case .invalid:
            self = .explain(.invalid)
        case .incompatible:
            self = .explain(.incompatible)
        case .needsSplits:
            self = .explain(.needsSplits)
        case .ready:
            guard case .nativeActivity = entry.report?.runtime else {
                self = .explain(.needsART)
                return
            }
            if isUpdating {
                self = .explain(.updating)
            } else if let fingerprint = signer.fingerprint {
                self = entry.signedWith == fingerprint ? .start : .signFirst
            } else {
                self = .openSigning
            }
        }
    }
}
