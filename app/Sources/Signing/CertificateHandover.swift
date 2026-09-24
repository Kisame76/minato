#if DEBUG
import APKSigning
import Darwin
import Foundation

/// Takes the signing certificate over from tools/hand-over-certificate.sh, so that a Debug build
/// installed from the Mac signs converted code without a .p12 file imported by hand (ADR-0022).
/// The script asks first and hands the certificate over only when the app lacks the one it is
/// signed with. Both requests end the app before its UI, with one line in the file
/// tmp/signing-certificate/<answer>, a name the script makes new for each request:
///   -signingCertificateStatus <answer>  "current" when the imported certificate is the one the
///       app is signed with, "other" for another one, each with its team and expiry date, or "none".
///   -importSigningCertificate <answer>  imports tmp/signing-certificate/identity.p12, whose
///       one-time password comes in the environment variable APK_CERTIFICATE_PASSWORD, the way the
///       certificate import in the app does it, with the kernel's check: "imported <team> <expiry
///       date>" or "failed <reason>". The file is deleted before the import, whatever happens.
/// Nothing else stays in the folder, above all no identity from an interrupted handover.
enum CertificateHandover {
    private static let folder = URL.temporaryDirectory.appending(path: "signing-certificate", directoryHint: .isDirectory)

    static func runIfRequested() {
        let arguments = ProcessInfo.processInfo.arguments
        if let answer = answerName(after: "-signingCertificateStatus", in: arguments) {
            finish(status(), in: answer)
        }
        if let answer = answerName(after: "-importSigningCertificate", in: arguments) {
            finish(importHandedOverIdentity(), in: answer)
        }
    }

    /// The name of the file to answer in, from the argument after `flag`: letters, digits and dashes.
    private static func answerName(after flag: String, in arguments: [String]) -> String? {
        guard let index = arguments.firstIndex(of: flag) else { return nil }
        let name = arguments.indices.contains(index + 1) ? arguments[index + 1] : ""
        let simple = !name.isEmpty && name.allSatisfy { $0.isASCII && ($0.isLetter || $0.isNumber || $0 == "-") }
        return simple ? name : "answer"
    }

    private static func status() -> String {
        guard let certificate = CodeSigning.storedCertificateInfo() else { return "none" }
        let state = certificate.sha256 == CodeSigning.ownCertificate?.sha256 ? "current" : "other"
        return "\(state) \(certificate.team) \(day(certificate.notAfter))"
    }

    private static func importHandedOverIdentity() -> String {
        let file = folder.appending(path: "identity.p12")
        let password = ProcessInfo.processInfo.environment["APK_CERTIFICATE_PASSWORD"]
        unsetenv("APK_CERTIFICATE_PASSWORD")
        let data = try? Data(contentsOf: file)
        try? FileManager.default.removeItem(at: file)
        guard let data, let password else { return "failed no certificate was handed over" }
        do {
            let certificate = try CertificateImporter.importCertificate(data, passwords: [password], source: .xcodeHandover,
                                                                        own: CodeSigning.ownSignature)
            return "imported \(certificate.team) \(day(certificate.notAfter))"
        } catch {
            return "failed \(error.localizedDescription)"
        }
    }

    private static func day(_ date: Date) -> String {
        date.formatted(.iso8601.year().month().day())
    }

    private static func finish(_ line: String, in name: String) -> Never {
        try? FileManager.default.removeItem(at: folder)
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        try? Data((line + "\n").utf8).write(to: folder.appending(path: name))
        exit(0)
    }
}
#endif
