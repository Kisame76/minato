import CryptoKit
import Foundation

/// What APK-Runner needs to know about a signing certificate (ADR-0023). The fingerprint decides:
/// iOS runs converted code only when it is signed with exactly the certificate the app itself is
/// signed with, because amfid checks that the app's provisioning profile lists it (ADR-0008, S1b).
public struct CertificateInfo: Sendable, Equatable, Codable {
    /// SHA-256 over the DER, lowercase hex.
    public var sha256: String
    /// The serial number as uppercase hex, leading zero bytes dropped.
    public var serialNumber: String
    /// The subject's organizational unit: the team ID of an Apple certificate.
    public var team: String
    public var commonName: String
    public var notAfter: Date

    public init(sha256: String, serialNumber: String, team: String, commonName: String, notAfter: Date) {
        self.sha256 = sha256
        self.serialNumber = serialNumber
        self.team = team
        self.commonName = commonName
        self.notAfter = notAfter
    }

    public init(der: Data, serialNumber: String, team: String, commonName: String, notAfter: Date) {
        let digest = SHA256.hash(data: der).map { String(format: "%02x", $0) }.joined()
        self.init(sha256: digest, serialNumber: serialNumber, team: team, commonName: commonName, notAfter: notAfter)
    }
}

/// How the imported certificate relates to the one the app is signed with.
public enum CertificateMatch: Sendable, Equatable {
    /// The app is signed ad hoc (Simulator): converted code is signed ad hoc too.
    case adHoc
    /// Nothing is imported.
    case none
    case same
    /// Another certificate of the same team, e.g. after the store made a new one.
    case renewed
    /// A certificate of another team, e.g. after installing from another Apple account.
    case otherTeam
}

public func compare(imported: CertificateInfo?, own: CertificateInfo?) -> CertificateMatch {
    guard let own else { return .adHoc }
    guard let imported else { return .none }
    if imported.sha256 == own.sha256 { return .same }
    return imported.team == own.team ? .renewed : .otherTeam
}
