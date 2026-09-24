import APKSigning
import Foundation
import UIKit

/// What the signing automation sees of the world (ADR-0023): the certificate the app is signed
/// with, who installed it, whether SideStore embedded its certificate, and which stores are there.
/// Debug builds on the Simulator can replace parts for tools/certificate-check.sh, with files from
/// tmp/certificate-check: -embeddedCertificate <file>, -simulatedInstaller sideStore|altStore,
/// -simulatedReachableStores sideStore,altStore (and -simulatedOwnCertificate, CodeSigning).
struct SigningEnvironment {
    var ownSignature: OwnSignature
    var own: CertificateInfo? { ownSignature.certificate }
    var installer: Installer
    /// `ALTCertificate.p12` in the app bundle, which SideStore 0.6.4 and later puts there.
    var embeddedCertificate: URL?
    var reachableStores: @MainActor () -> Set<Store>

    @MainActor
    static func current() -> SigningEnvironment {
        var environment = SigningEnvironment(
            ownSignature: CodeSigning.ownSignature,
            installer: Installer.detect(infoDictionary: Bundle.main.infoDictionary ?? [:]),
            embeddedCertificate: Bundle.main.url(forResource: "ALTCertificate", withExtension: "p12"),
            reachableStores: {
                Set(Store.allCases.filter { store in
                    URL(string: "\(store.exportScheme)://").map(UIApplication.shared.canOpenURL) ?? false
                })
            })
        #if DEBUG && targetEnvironment(simulator)
        let arguments = ProcessInfo.processInfo.arguments
        func value(after flag: String) -> String? {
            guard let index = arguments.firstIndex(of: flag), arguments.indices.contains(index + 1) else { return nil }
            return arguments[index + 1]
        }
        let folder = URL.temporaryDirectory.appending(path: "certificate-check")
        if let file = value(after: "-embeddedCertificate") {
            environment.embeddedCertificate = folder.appending(path: file)
        }
        if let installer = value(after: "-simulatedInstaller").flatMap(Store.init(rawValue:)) {
            environment.installer = .store(installer)
        }
        if let stores = value(after: "-simulatedReachableStores") {
            let reachable = Set(stores.split(separator: ",").compactMap { Store(rawValue: String($0)) })
            environment.reachableStores = { reachable }
        }
        #endif
        return environment
    }
}
