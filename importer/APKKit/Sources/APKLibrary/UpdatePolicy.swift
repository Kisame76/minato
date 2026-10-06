import APKFormats
import APKKit
import Foundation

/// What the update policy compares of a version, installed or about to be imported (ADR-0034).
public struct AppVersion: Sendable, Equatable {
    public var state: LibraryEntry.State
    public var versionCode: Int64
    /// SHA-256 of the APK; nil for an installed version from before it was recorded that keeps no copy.
    public var sha256: String?
    public var signer: SignerIdentity
    /// Whether the signature check proved `signer` (ADR-0034, step d), for the new file and for the
    /// installed version alike: an unproven one on either side asks, since a forged file names anyone.
    public var signerProven: Bool
    /// Whether the signature check found the file damaged or forged.
    public var signatureFailed: Bool
    /// The APKs of a set whose content Minato cannot check: a set's unverifiable APKs beside verified ones
    /// (ADR-0048 b3).
    public var uncheckedFiles: [String]

    public init(state: LibraryEntry.State, versionCode: Int64, sha256: String?, signer: SignerIdentity,
                signerProven: Bool = true, signatureFailed: Bool = false, uncheckedFiles: [String] = []) {
        self.state = state
        self.versionCode = versionCode
        self.sha256 = sha256
        self.signer = signer
        self.signerProven = signerProven
        self.signatureFailed = signatureFailed
        self.uncheckedFiles = uncheckedFiles
    }

    /// The version a checked set is about to become (ADR-0048 b3), its signer from how the set is signed, never from
    /// its base's own report. A set of APKs Minato cannot check, or of unsigned APKs, is one such APK; one that mixes
    /// verified and unverifiable APKs has the verified APKs' signer, proven, and names the others.
    public init(set: CheckedSet, state: LibraryEntry.State, sha256: String?) {
        let first = set.base ?? set.splits.first
        let versionCode = first?.versionCode ?? 0
        switch set.signing {
        case .verified(let signer):
            self.init(state: state, versionCode: versionCode, sha256: sha256, signer: signer)
        case .partlyVerified(let signer, let unverifiable):
            self.init(state: state, versionCode: versionCode, sha256: sha256, signer: signer,
                      uncheckedFiles: unverifiable.map(\.fileName))
        case .unverified:
            // The certificates every APK names, the same in each (SetChecks).
            self.init(state: state, versionCode: versionCode, sha256: sha256,
                      signer: SignerIdentity(current: first?.certificates ?? []), signerProven: false)
        case .unsigned:
            self.init(state: state, versionCode: versionCode, sha256: sha256, signer: SignerIdentity(current: []),
                      signerProven: false)
        }
    }

    /// The version an entry holds; nil for a file that was not a readable APK. A report from before the
    /// signer was recorded counts every certificate it lists as current, and one from before the check
    /// counts as unproven; the library reads both from the version's base.apk instead where it can.
    public init?(entry: LibraryEntry) {
        guard let report = entry.report else { return nil }
        self.init(
            state: entry.state, versionCode: report.versionCode, sha256: entry.source.sha256 ?? entry.base?.sha256,
            signer: report.signature.signer ?? SignerIdentity(current: report.signature.certificates.map(\.sha256)),
            signerProven: report.signature.check == .verified, signatureFailed: report.signature.check?.isFailure == true)
    }
}

/// Why the library asks before an import (ADR-0034, decision 1).
public enum UpdateWarning: Sendable, Hashable {
    /// The file is an older version, which may not be able to read the newer one's data.
    case olderVersion
    /// Someone else signed the file, one of the two is unsigned, or the check could not prove the file's
    /// signer: it would get the installed app's data.
    case otherSigner
    /// APKs of the set are signed in a way Minato cannot check, so it cannot check their content; the question names
    /// them. Asked also where nothing is installed (ADR-0048 b3, the lead's decision).
    case uncheckedContent
    /// The container brings an OBB under an installed OBB's name with other bytes, which would change the app's
    /// content; the question names them (`ImportResult.otherOBBs`, ADR-0049 c2, the lead's rule).
    case otherOBB
}

/// What importing a file does to the installed app of the same package (ADR-0034, decision 1).
public enum UpdateDecision: Sendable, Equatable {
    /// No app of this package is installed yet.
    case install
    /// A newer version replaces the installed one, which keeps its data and its place.
    case update
    /// The same version with other bytes is installed again, as Android does.
    case reinstall
    /// The same bytes: nothing changes, and the file is discarded.
    case unchanged
    /// The file failed its signature check, or the installed version works and the file, in this state,
    /// would not: nothing changes.
    case reject(LibraryEntry.State)
    /// Nothing changes until the user answers.
    case ask(Set<UpdateWarning>)
}

public enum UpdatePolicy {
    public static func decide(installed: AppVersion?, new: AppVersion) -> UpdateDecision {
        guard let installed else { return new.uncheckedFiles.isEmpty ? .install : .ask([.uncheckedContent]) }
        // A damaged or forged file never replaces anything, and an unusable one never replaces a working
        // version, whatever its version or signer. A version that needs splits is a base waiting for its split
        // APKs: a file that cannot run, a lone split APK among them, never replaces it either.
        if new.signatureFailed || installed.state == .ready && (new.state == .incompatible || new.state == .needsSplits)
            || installed.state == .needsSplits && new.state == .incompatible {
            return .reject(new.state)
        }
        // The same bytes carry the same signature, so no answer could change anything (ADR-0048 b5b2).
        if let bytes = installed.sha256, bytes == new.sha256 { return .unchanged }
        var warnings: Set<UpdateWarning> = []
        if !new.uncheckedFiles.isEmpty { warnings.insert(.uncheckedContent) }
        if !new.signerProven || !installed.signerProven || !new.signer.canUpdate(installed.signer) {
            warnings.insert(.otherSigner)
        }
        if new.versionCode < installed.versionCode { warnings.insert(.olderVersion) }
        if !warnings.isEmpty { return .ask(warnings) }
        if new.versionCode > installed.versionCode { return .update }
        return .reinstall
    }
}

/// What `Library.importFile` did.
public struct ImportResult: Sendable, Equatable {
    /// The package's entry after the import: the new one, or the installed one that stays.
    public var entry: LibraryEntry
    public var decision: UpdateDecision
    /// The imported file's report; nil for a file that is not a readable APK.
    public var report: APKReport?
    /// The APKs of an asking set whose signature Minato cannot verify, which the question about `uncheckedContent`
    /// names (ADR-0048 b7a).
    public var uncheckedFiles: [String] = []
    /// The OBBs this import moved into the app's OBB folder (ADR-0049 c2), also where the version stayed `.unchanged`.
    public var installedOBBs: [PackageFile] = []
    /// The installed OBBs an asking import would replace with other bytes, by name, which the question about
    /// `otherOBB` names (ADR-0049 c2).
    public var otherOBBs: [String] = []

    /// The version a question lists for this import, with what it warns about; nil where it does not ask.
    public var asking: AskingVersion? {
        guard case .ask(let warnings) = decision, let report else { return nil }
        return AskingVersion(package: report.packageName, versionCode: report.versionCode, warnings: warnings)
    }
}

/// A version that asks, as its question lists it: "Replace" answers it and no other, and only while it asks the same,
/// so an answer after the installed app changed meanwhile asks again (ADR-0048, F7).
public struct AskingVersion: Sendable, Hashable {
    public var package: String
    public var versionCode: Int64
    public var warnings: Set<UpdateWarning>

    public init(package: String, versionCode: Int64, warnings: Set<UpdateWarning>) {
        self.package = package
        self.versionCode = versionCode
        self.warnings = warnings
    }
}
