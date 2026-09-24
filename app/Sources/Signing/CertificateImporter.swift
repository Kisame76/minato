import APKConvert
import APKSigning
import Foundation
import Security
import os

private let log = Logger(subsystem: "io.github.kisame76.apkrunner", category: "signing")

enum CertificateImportError: LocalizedError, Equatable {
    case unreadable(String)
    case wrongPassword
    /// Not the certificate the app is signed with; `store` is where it came from, if a store.
    case notTheAppsCertificate(store: Store?)
    case selfTestFailed(String)
    case keychain(OSStatus)
    /// The app's own certificate cannot be read, so no imported one can be checked against it.
    case ownCertificateUnreadable(String)

    var errorDescription: String? {
        switch self {
        case .unreadable(let reason):
            "The file is not a certificate APK-Runner can read (\(reason))."
        case .wrongPassword:
            "The password is not correct."
        case .notTheAppsCertificate(.some(.sideStore)):
            "SideStore has a newer certificate than the one APK-Runner is installed with. Re-sign APK-Runner in SideStore first."
        case .notTheAppsCertificate(.some(.altStore)):
            "AltStore has a newer certificate than the one APK-Runner is installed with. Refresh APK-Runner in AltStore first."
        case .notTheAppsCertificate(.none):
            "This is not the certificate APK-Runner is installed with, so iOS would not run code signed with it."
        case .selfTestFailed(let reason):
            "iOS does not run code signed with this certificate: \(reason)"
        case .keychain(let status):
            "The certificate could not be saved in the keychain (error \(status))."
        case .ownCertificateUnreadable(let reason):
            "APK-Runner cannot read the certificate it is signed with (\(reason)), so it cannot check this one."
        }
    }
}

/// Runs imports, self-tests and removals one after the other: two at once would interleave their
/// keychain writes and could leave a certificate without its key.
actor CertificateImportLane {
    static let shared = CertificateImportLane()

    func importCertificate(_ p12: Data, passwords: [String], source: CertificateSource,
                           own: OwnSignature) throws -> CertificateInfo {
        try CertificateImporter.importCertificate(p12, passwords: passwords, source: source, own: own)
    }

    func runSelfTest() -> SelfTestRecord? {
        CertificateImporter.runSelfTest()
    }

    func removeCertificate(records: SigningRecordFile) {
        CodeSigning.removeIdentity()
        try? FileManager.default.removeItem(at: records.url)
    }
}

/// Leaves a marker while the self-test loads and runs its test code. A broken signature makes iOS
/// end the process right there; a marker found at the next launch tells the app not to load
/// anything by itself then, instead of ending again at every start.
enum SelfTestGuard {
    static var marker: URL {
        SigningRecordFile.standard.url.deletingLastPathComponent().appending(path: "signing-selftest-running")
    }

    static func run<T>(_ body: () throws -> T) rethrows -> T {
        FileManager.default.createFile(atPath: marker.path, contents: Data())
        defer { try? FileManager.default.removeItem(at: marker) }
        return try body()
    }

    /// Whether the last self-test did not come back; true once, the marker goes with it.
    static func endedTheAppLastTime() -> Bool {
        guard FileManager.default.fileExists(atPath: marker.path) else { return false }
        try? FileManager.default.removeItem(at: marker)
        return true
    }
}

/// The one way a signing certificate gets into the app, whatever brought it (ADR-0023): the file
/// SideStore embeds, a store's export, the deploy hand-over or a picked .p12 file. Callers that may
/// run at the same time go through CertificateImportLane.
enum CertificateImporter {
    /// Reads `p12` with each of `passwords` and then with none; checks that it is the certificate
    /// the app is signed with (`own`; anything goes when the app is signed ad hoc); signs, loads and
    /// calls a test container with it; only then replaces the stored certificate and records where
    /// it came from.
    @discardableResult
    static func importCertificate(_ p12: Data, passwords: [String], source: CertificateSource, own: OwnSignature,
                                  records: SigningRecordFile = .standard, now: Date = .now) throws -> CertificateInfo {
        if let problem = own.problem { throw CertificateImportError.ownCertificateUnreadable(problem) }
        let contents = try read(p12, passwords: passwords)
        guard let info = CodeSigning.info(of: contents.certificateData) else {
            throw CertificateImportError.unreadable("its certificate cannot be read")
        }
        if let own = own.certificate, info.sha256 != own.sha256 {
            throw CertificateImportError.notTheAppsCertificate(store: source.store)
        }
        let result: apkconv_self_test_result
        do {
            result = try SelfTestGuard.run { try testSigner(for: contents).selfTest(in: .temporaryDirectory) }
        } catch {
            throw CertificateImportError.selfTestFailed(error.localizedDescription)
        }

        CodeSigning.removeIdentity()
        if let identity = contents.identity {
            try CodeSigning.store(identity: identity)
        } else {
            try CodeSigning.store(privateKey: contents.privateKey, certificate: contents.certificate)
        }
        let selfTest = SelfTestRecord(sha256: info.sha256, passed: true, message: summary(of: result), date: now)
        try records.update { record in
            record.source = source
            record.importedAt = now
            record.sha256 = info.sha256
            record.selfTest = selfTest
            record.markedStale = false
            record.pendingExport = nil
        }
        let paired = CodeSigning.storedIdentity() != nil ? "keychain identity" : "keychain key and certificate"
        log.info("Imported certificate \(info.sha256.prefix(12), privacy: .public) from \(source.rawValue, privacy: .public) as \(paired, privacy: .public); \(selfTest.message, privacy: .public)")
        return info
    }

    /// Runs the self-test with the stored certificate and records the outcome.
    @discardableResult
    static func runSelfTest(records: SigningRecordFile = .standard, now: Date = .now) -> SelfTestRecord? {
        guard let sha256 = CodeSigning.storedCertificateInfo()?.sha256 ?? records.load().sha256 else { return nil }
        let outcome: SelfTestRecord
        do {
            let result = try SelfTestGuard.run { try CodeSigning.signer().selfTest(in: .temporaryDirectory) }
            outcome = SelfTestRecord(sha256: sha256, passed: true, message: summary(of: result), date: now)
        } catch {
            outcome = SelfTestRecord(sha256: sha256, passed: false, message: error.localizedDescription, date: now)
        }
        try? records.update { $0.selfTest = outcome }
        log.info("Self-test: \(outcome.passed ? "passed" : "failed", privacy: .public), \(outcome.message, privacy: .public)")
        return outcome
    }

    private struct Contents {
        let privateKey: SecKey
        let certificate: SecCertificate
        let certificateData: Data
        /// When SecPKCS12Import read the file.
        let identity: SecIdentity?
    }

    private static func read(_ p12: Data, passwords: [String]) throws -> Contents {
        var candidates: [String] = []
        for password in passwords + [""] where !candidates.contains(password) {
            candidates.append(password)
        }
        var refusedPassword = false
        for password in candidates {
            var items: CFArray?
            let status = SecPKCS12Import(p12 as CFData, [kSecImportExportPassphrase: password] as CFDictionary, &items)
            if status == errSecAuthFailed {
                refusedPassword = true
                continue
            }
            // Any other failure is a format iOS does not read; no other password would help.
            guard status == errSecSuccess else { break }
            guard let first = (items as? [[CFString: Any]])?.first, let value = first[kSecImportItemIdentity],
                  CFGetTypeID(value as CFTypeRef) == SecIdentityGetTypeID()
            else { throw CertificateImportError.unreadable("it holds no certificate with its private key") }
            let identity = value as! SecIdentity
            var key: SecKey?
            var certificate: SecCertificate?
            guard SecIdentityCopyPrivateKey(identity, &key) == errSecSuccess, let key,
                  SecIdentityCopyCertificate(identity, &certificate) == errSecSuccess, let certificate
            else { throw CertificateImportError.unreadable("it holds no certificate with its private key") }
            return Contents(privateKey: key, certificate: certificate,
                            certificateData: SecCertificateCopyData(certificate) as Data, identity: identity)
        }

        // SideStore 0.6.4 and later writes the file without encryption and without a MAC, which
        // SecPKCS12Import rejects (ADR-0023).
        var key = [UInt8](repeating: 0, count: 8192)
        var certificate = [UInt8](repeating: 0, count: 16384)
        var keyLength = 0
        var certificateLength = 0
        var error = [CChar](repeating: 0, count: 512)
        let read = p12.withUnsafeBytes {
            apkconv_read_unencrypted_pkcs12($0.baseAddress, $0.count, &key, key.count, &keyLength, &certificate,
                                            certificate.count, &certificateLength, &error, error.count)
        }
        switch read {
        case 0:
            let keyAttributes: [CFString: Any] = [kSecAttrKeyType: kSecAttrKeyTypeRSA, kSecAttrKeyClass: kSecAttrKeyClassPrivate]
            let certificateData = Data(certificate.prefix(certificateLength))
            guard let privateKey = SecKeyCreateWithData(Data(key.prefix(keyLength)) as CFData, keyAttributes as CFDictionary, nil)
            else { throw CertificateImportError.unreadable("its private key cannot be used") }
            guard let secCertificate = SecCertificateCreateWithData(nil, certificateData as CFData)
            else { throw CertificateImportError.unreadable("its certificate cannot be read") }
            return Contents(privateKey: privateKey, certificate: secCertificate, certificateData: certificateData, identity: nil)
        case 1:
            // Encrypted, and none of the passwords opened it.
            throw CertificateImportError.wrongPassword
        default:
            throw refusedPassword ? CertificateImportError.wrongPassword : CertificateImportError.unreadable(converterMessage(in: error))
        }
    }

    /// The signer for the self-test, before anything is stored.
    private static func testSigner(for contents: Contents) throws -> ContainerSigner {
        #if targetEnvironment(simulator)
        // The Simulator runs ad-hoc signed code and has no chain to complete.
        return try ContainerSigner(identity: nil)
        #else
        if let identity = contents.identity { return try ContainerSigner(identity: identity) }
        return try ContainerSigner(privateKey: contents.privateKey, certificate: contents.certificateData)
        #endif
    }

    private static func summary(of result: apkconv_self_test_result) -> String {
        String(format: "returned %d; signing %.1f ms, loading %.1f ms", result.value, result.sign_milliseconds, result.load_milliseconds)
    }
}
