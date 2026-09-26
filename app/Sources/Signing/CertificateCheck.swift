#if DEBUG
import APKSigning
import Darwin
import Foundation

/// Answers tools/certificate-check.sh in the Simulator (ADR-0023). The script copies test files into
/// tmp/certificate-check/ and starts the app with requests; the app answers with one line in
/// tmp/certificate-check/<answer>, a name the script makes new each time, and ends:
///   -certificateCheckImport <file> <password> <answer>  imports tmp/certificate-check/<file> the
///       way every source does (CertificateImporter), before the UI: "imported <sha256> selftest
///       passed" or "failed <reason>".
///   -certificateCheckStatus <answer>  once the SigningCoordinator has settled: "<state> <source> <sha256>"
///       for a valid certificate, "<state> <next step> ..." otherwise (see `line`).
///   -certificateCheckStatusAtLaunch <answer>  the same for the state the coordinator starts with, before
///       its first look, which a tap in the library may meet (ADR-0032, decision 3).
///   -certificateCheckAwaitCallback  with -certificateCheckStatus: the coordinator does not open the
///       store by itself and answers only after a certificate callback, which the script sends.
///   -certificateCheckOpenCallback <file>  with -certificateCheckAwaitCallback: the app opens the URL
///       in tmp/certificate-check/<file> itself, with UIApplication.open, the call a store makes to
///       answer. (xcrun simctl openurl cuts URLs to 2047 characters, too short for a certificate.)
///   -certificateCheckDiagnostics <answer>  once the coordinator has settled: the diagnostics, one
///       "key=value" line per row (SigningDiagnostics).
///   -certificateCheckReset  removes the imported certificate and signing.json first.
///   -certificateCheckSimulateChange  the coordinator's first look marks the certificate and everything
///       signed with it old, as "Simulate Certificate Change" in the diagnostics does.
/// With -simulatedOwnCertificate <file> the ad-hoc signed Simulator app takes that certificate as
/// the one it is signed with (CodeSigning.ownCertificate); see SigningEnvironment for the others.
enum CertificateCheck {
    static let folder = URL.temporaryDirectory.appending(path: "certificate-check", directoryHint: .isDirectory)
    private static let arguments = ProcessInfo.processInfo.arguments

    static var awaitsCallback: Bool { arguments.contains("-certificateCheckAwaitCallback") }

    /// The coordinator's first look marks everything old, as "Simulate Certificate Change" does.
    static var simulatesChange: Bool { arguments.contains("-certificateCheckSimulateChange") }

    /// The callback URL the app is to open itself, as a store would.
    static var callbackToOpen: URL? {
        guard let index = arguments.firstIndex(of: "-certificateCheckOpenCallback"), arguments.indices.contains(index + 1),
              let text = try? String(contentsOf: folder.appending(path: arguments[index + 1]), encoding: .utf8)
        else { return nil }
        return URL(string: text.trimmingCharacters(in: .whitespacesAndNewlines))
    }

    static func runIfRequested() {
        if let index = arguments.firstIndex(of: "-certificateCheckImport"), arguments.indices.contains(index + 3) {
            finish(importFile(arguments[index + 1], password: arguments[index + 2]), in: arguments[index + 3])
        }
        if arguments.contains("-certificateCheckReset") {
            CodeSigning.removeIdentity()
            try? FileManager.default.removeItem(at: SigningRecordFile.standard.url)
        }
    }

    /// Called by the coordinator when it is made, before its first look; answers when asked to.
    @MainActor
    static func reportAtLaunchIfRequested(_ coordinator: SigningCoordinator) {
        guard let index = arguments.firstIndex(of: "-certificateCheckStatusAtLaunch"), arguments.indices.contains(index + 1)
        else { return }
        finish(line(state: coordinator.state, next: coordinator.next, record: coordinator.records.load()), in: arguments[index + 1])
    }

    /// Called by the coordinator whenever it has settled; answers once, when asked to.
    @MainActor
    static func reportIfRequested(_ coordinator: SigningCoordinator) {
        if let index = arguments.firstIndex(of: "-certificateCheckStatus"), arguments.indices.contains(index + 1) {
            finish(line(state: coordinator.state, next: coordinator.next, record: coordinator.records.load()),
                   in: arguments[index + 1])
        }
        if let index = arguments.firstIndex(of: "-certificateCheckDiagnostics"), arguments.indices.contains(index + 1) {
            let rows = SigningDiagnostics.current(coordinator).rows.map { "\($0.key)=\($0.value)" }
            finish(rows.joined(separator: "\n"), in: arguments[index + 1])
        }
    }

    private static func line(state: SigningState, next: NextStep, record: SigningRecord) -> String {
        switch state {
        case .valid(let certificate):
            return "valid \(record.source?.rawValue ?? "unknown") \(certificate.sha256)"
        case .notNeeded:
            return "notNeeded"
        case .ownCertificateUnreadable(let reason):
            return "ownCertificateUnreadable \(reason)"
        case .missing:
            return "missing \(describe(next, record: record))"
        case .changed:
            return "changed \(describe(next, record: record))"
        case .expired(let certificate):
            return "expired \(certificate.sha256)"
        case .selfTestFailed(_, let reason):
            return "selfTestFailed \(reason)"
        }
    }

    private static func describe(_ step: NextStep, record: SigningRecord) -> String {
        switch step {
        case .requestExport(let store, _): "requestExport \(store.rawValue) \(record.pendingExport?.token ?? "none")"
        case .importEmbedded: "importEmbedded"
        case .offerFileImport: "offerFileImport"
        case .runSelfTest: "runSelfTest"
        case .resign(let count): "resign \(count)"
        case .none: "none"
        }
    }

    private static func importFile(_ name: String, password: String) -> String {
        guard let data = try? Data(contentsOf: folder.appending(path: name)) else { return "failed no such file" }
        do {
            let info = try CertificateImporter.importCertificate(data, passwords: [password], source: .file,
                                                                 own: CodeSigning.ownSignature)
            let passed = SigningRecordFile.standard.load().selfTest?.passed == true
            return "imported \(info.sha256) selftest \(passed ? "passed" : "failed")"
        } catch let error as CertificateImportError {
            return "failed \(error.checkName)"
        } catch {
            return "failed \(error.localizedDescription)"
        }
    }

    /// Writes the answer; the name comes from the script and may only hold letters, digits and dashes.
    private static func finish(_ line: String, in name: String) -> Never {
        let simple = !name.isEmpty && name.allSatisfy { $0.isASCII && ($0.isLetter || $0.isNumber || $0 == "-") }
        try? Data((line + "\n").utf8).write(to: folder.appending(path: simple ? name : "answer"))
        exit(0)
    }
}

private extension CertificateImportError {
    var checkName: String {
        switch self {
        case .unreadable: "unreadable"
        case .wrongPassword: "wrongPassword"
        case .notTheAppsCertificate: "notTheAppsCertificate"
        case .selfTestFailed(let reason): "selfTestFailed \(reason)"
        case .keychain(let status): "keychain \(status)"
        case .ownCertificateUnreadable: "ownCertificateUnreadable"
        }
    }
}
#endif
