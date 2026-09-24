import Foundation

/// Where the imported certificate came from.
public enum CertificateSource: String, Sendable, Codable {
    /// `ALTCertificate.p12` that SideStore 0.6.4 and later puts into every app it signs.
    case sideStoreEmbedded
    case sideStoreExport
    case altStoreExport
    /// tools/hand-over-certificate.sh, for builds from Xcode (ADR-0022).
    case xcodeHandover
    /// A .p12 file the user picked.
    case file

    /// The store the certificate came from, if any.
    public var store: Store? {
        switch self {
        case .sideStoreEmbedded, .sideStoreExport: .sideStore
        case .altStoreExport: .altStore
        case .xcodeHandover, .file: nil
        }
    }
}

/// The outcome of the self-test for one certificate: sign a test container, load it, call it.
public struct SelfTestRecord: Sendable, Equatable, Codable {
    /// The certificate it was run with.
    public var sha256: String
    public var passed: Bool
    /// The value the test function returned, or why the test failed.
    public var message: String
    public var date: Date

    public init(sha256: String, passed: Bool, message: String, date: Date) {
        self.sha256 = sha256
        self.passed = passed
        self.message = message
        self.date = date
    }
}

public enum SigningState: Sendable, Equatable {
    /// The app is signed ad hoc (Simulator); converted code is signed ad hoc too.
    case notNeeded
    /// The app is signed with a certificate that APK-Runner cannot read, so it cannot tell which
    /// certificate iOS accepts. Nothing is imported until that is fixed.
    case ownCertificateUnreadable(String)
    case missing
    case valid(CertificateInfo)
    /// The app is signed with another certificate than the imported one, e.g. after the store made
    /// a new one; also when the imported one is marked stale.
    case changed(old: CertificateInfo, new: CertificateInfo)
    case expired(CertificateInfo)
    case selfTestFailed(CertificateInfo, reason: String)
}

public enum NextStep: Sendable, Equatable {
    case none
    /// Import `ALTCertificate.p12` from the app bundle: no tap.
    case importEmbedded
    /// Open the store's export link, by itself only once per launch.
    case requestExport(Store, automatically: Bool)
    /// No store can give a certificate that fits; a picked .p12 file still can.
    case offerFileImport
    case runSelfTest
    /// Converted code signed with another certificate waits to be signed again.
    case resign(count: Int)
}

/// Everything the decision depends on, gathered by the app.
public struct SigningInputs: Sendable, Equatable {
    /// The leaf of the app's own code signature; nil when the app is signed ad hoc or it cannot be read.
    public var own: CertificateInfo?
    /// Why the app's own certificate cannot be read, when it is signed with one.
    public var ownCertificateProblem: String?
    public var imported: CertificateInfo?
    /// Set by "Simulate Certificate Change" in the diagnostics.
    public var markedStale = false
    public var selfTest: SelfTestRecord?
    public var hasEmbeddedCertificate = false
    /// The embedded file was tried in this process and did not import.
    public var embeddedCertificateFailed = false
    public var installer: Installer = .other
    /// Stores whose export link can be opened.
    public var reachableStores: Set<Store> = []
    public var openedStoreThisLaunch = false
    public var pendingResign = 0
    public var now: Date

    public init(own: CertificateInfo?, imported: CertificateInfo?, now: Date) {
        self.own = own
        self.imported = imported
        self.now = now
    }
}

/// Decides the state and the next step (ADR-0023). Only a certificate equal to the app's own is
/// usable, so a missing and a changed one are fetched the same way, and only from the store that
/// installed the app: another store's certificate is not in the app's provisioning profile.
public func assess(_ inputs: SigningInputs) -> (state: SigningState, next: NextStep) {
    guard let own = inputs.own else {
        if let problem = inputs.ownCertificateProblem { return (.ownCertificateUnreadable(problem), .none) }
        return (.notNeeded, .none)
    }
    guard let imported = inputs.imported else { return (.missing, fetchStep(inputs)) }
    if imported.sha256 != own.sha256 || inputs.markedStale {
        return (.changed(old: imported, new: own), fetchStep(inputs))
    }
    if own.notAfter <= inputs.now { return (.expired(imported), .none) }
    guard let selfTest = inputs.selfTest, selfTest.sha256 == imported.sha256 else { return (.valid(imported), .runSelfTest) }
    guard selfTest.passed else { return (.selfTestFailed(imported, reason: selfTest.message), .none) }
    if inputs.pendingResign > 0 { return (.valid(imported), .resign(count: inputs.pendingResign)) }
    return (.valid(imported), .none)
}

private func fetchStep(_ inputs: SigningInputs) -> NextStep {
    if inputs.hasEmbeddedCertificate && !inputs.embeddedCertificateFailed { return .importEmbedded }
    if case .store(let store) = inputs.installer, inputs.reachableStores.contains(store) {
        return .requestExport(store, automatically: !inputs.openedStoreThisLaunch)
    }
    return .offerFileImport
}
