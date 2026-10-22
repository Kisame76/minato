import APKConvert
import APKLibrary
import APKSigning
import Foundation
import Security

enum CodeSigningError: LocalizedError {
    case converter(String)

    var errorDescription: String? {
        switch self {
        case .converter(let message): message
        }
    }
}

/// How the app itself is signed, which decides the certificate iOS accepts for converted code.
enum OwnSignature: Equatable {
    /// Signed ad hoc, as on the Simulator: converted code is signed ad hoc too.
    case adHoc
    case certificate(CertificateInfo)
    /// Signed with a certificate Minato cannot read; the reason.
    case unreadable(String)

    var certificate: CertificateInfo? {
        if case .certificate(let certificate) = self { certificate } else { nil }
    }

    var problem: String? {
        if case .unreadable(let reason) = self { reason } else { nil }
    }
}

/// The development certificate Minato signs converted native code with, so that iOS runs it
/// (ADR-0008, ADR-0022, ADR-0023). It is kept in the app's keychain, on this device only: as one
/// identity when SecPKCS12Import read it, or as its private key and certificate when it came as an
/// unencrypted file from SideStore. No password is kept.
enum CodeSigning {
    // MARK: - The certificates

    /// What the app shows and compares about the certificate in `der`.
    static func info(of der: Data) -> CertificateInfo? {
        var description = apkconv_certificate()
        var error = converterErrorBuffer()
        let described = der.withUnsafeBytes { apkconv_describe_certificate($0.baseAddress, $0.count, &description, &error, error.count) }
        guard described == 0 else { return nil }
        return CertificateInfo(der: der, serialNumber: text(of: &description.serial_number), team: text(of: &description.team),
                               commonName: text(of: &description.common_name),
                               notAfter: Date(timeIntervalSince1970: TimeInterval(description.not_after)))
    }

    /// How the app itself is signed, read once per process (ADR-0023). Its certificate is the only
    /// one iOS accepts for converted code. Debug builds on the Simulator, which are signed ad hoc,
    /// can take one with -simulatedOwnCertificate <file in tmp/certificate-check>.
    static let ownSignature: OwnSignature = {
        #if DEBUG && targetEnvironment(simulator)
        let arguments = ProcessInfo.processInfo.arguments
        if let index = arguments.firstIndex(of: "-simulatedOwnCertificate"), arguments.indices.contains(index + 1) {
            let file = URL.temporaryDirectory.appending(path: "certificate-check").appending(path: arguments[index + 1])
            return (try? Data(contentsOf: file)).flatMap(info(of:)).map(OwnSignature.certificate) ?? .unreadable("no such file")
        }
        #endif
        return readOwnSignature()
    }()

    /// The certificate the app itself is signed with; nil when it is signed ad hoc or that cannot be read.
    static var ownCertificate: CertificateInfo? { ownSignature.certificate }

    private static func readOwnSignature() -> OwnSignature {
        guard let executable = Bundle.main.executablePath else { return .unreadable("the app has no executable path") }
        var team = [CChar](repeating: 0, count: 64)
        var error = converterErrorBuffer()
        guard apkconv_signing_team(executable, &team, team.count, &error, error.count) == 0 else {
            return .unreadable(converterMessage(in: error))
        }
        // No team in the code directory: an ad-hoc signature, as on the Simulator.
        guard team.first != 0 else { return .adHoc }
        var buffer = [UInt8](repeating: 0, count: 16384)
        var length = 0
        guard apkconv_signing_certificate(executable, &buffer, buffer.count, &length, &error, error.count) == 0 else {
            return .unreadable(converterMessage(in: error))
        }
        guard let certificate = info(of: Data(buffer.prefix(length))) else {
            return .unreadable("its certificate cannot be read")
        }
        return .certificate(certificate)
    }

    // MARK: - The keychain

    /// The imported identity, when the keychain pairs the stored key and certificate into one.
    static func storedIdentity() -> SecIdentity? {
        let query: [CFString: Any] = [kSecClass: kSecClassIdentity, kSecReturnRef: true, kSecMatchLimit: kSecMatchLimitOne]
        var result: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &result) == errSecSuccess, let result,
              CFGetTypeID(result) == SecIdentityGetTypeID()
        else { return nil }
        return (result as! SecIdentity)
    }

    static func storedPrivateKey() -> SecKey? {
        let query: [CFString: Any] = [kSecClass: kSecClassKey, kSecAttrKeyClass: kSecAttrKeyClassPrivate,
                                      kSecReturnRef: true, kSecMatchLimit: kSecMatchLimitOne]
        var result: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &result) == errSecSuccess, let result,
              CFGetTypeID(result) == SecKeyGetTypeID()
        else { return nil }
        return (result as! SecKey)
    }

    /// The imported certificate, in DER, however it is stored.
    static func storedCertificateData() -> Data? {
        let query: [CFString: Any] = [kSecClass: kSecClassCertificate, kSecReturnData: true, kSecMatchLimit: kSecMatchLimitOne]
        var result: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &result) == errSecSuccess else { return nil }
        return result as? Data
    }

    static func storedCertificateInfo() -> CertificateInfo? {
        storedCertificateData().flatMap(info(of:))
    }

    /// The common names of the certificates in the app's own signature, which complete an imported
    /// certificate's chain (ADR-0022); for the diagnostics.
    static func ownChain() -> String {
        guard let executable = Bundle.main.executablePath else { return "no executable" }
        var names = [CChar](repeating: 0, count: 2048)
        var error = converterErrorBuffer()
        guard apkconv_signing_chain(executable, &names, names.count, &error, error.count) == 0 else {
            return "unreadable: \(converterMessage(in: error))"
        }
        let text = converterMessage(in: names)
        return text.isEmpty ? "none (signed ad hoc)" : text.replacingOccurrences(of: "\n", with: " · ")
    }

    static func store(identity: SecIdentity) throws {
        try add(identity)
    }

    /// Stores a key and its certificate as two items; the keychain can then pair them into an
    /// identity, and `signer()` also works when it does not. When the certificate cannot be added,
    /// the key goes again, and nothing else is touched.
    static func store(privateKey: SecKey, certificate: SecCertificate) throws {
        try add(privateKey)
        do {
            try add(certificate)
        } catch {
            SecItemDelete([kSecValueRef: privateKey] as CFDictionary)
            throw error
        }
    }

    /// Removes the imported certificate and its private key.
    static func removeIdentity() {
        for itemClass in [kSecClassIdentity, kSecClassCertificate, kSecClassKey] {
            SecItemDelete([kSecClass: itemClass] as CFDictionary)
        }
    }

    private static func add(_ item: CFTypeRef) throws {
        let attributes: [CFString: Any] = [kSecValueRef: item, kSecAttrAccessible: kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly]
        let status = SecItemAdd(attributes as CFDictionary, nil)
        guard status == errSecSuccess else { throw CertificateImportError.keychain(status) }
    }

    // MARK: - Signing

    /// The signer for converted code: on a device with the imported certificate, ad hoc on the Simulator.
    static func signer() throws -> ContainerSigner {
        #if targetEnvironment(simulator)
        return try ContainerSigner(identity: nil)
        #else
        if let identity = storedIdentity() { return try ContainerSigner(identity: identity) }
        guard let key = storedPrivateKey(), let certificate = storedCertificateData() else {
            throw CodeSigningError.converter("No signing certificate is imported.")
        }
        return try ContainerSigner(privateKey: key, certificate: certificate)
        #endif
    }

    private static func text<T>(of field: inout T) -> String {
        withUnsafeBytes(of: &field) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
    }
}

/// The signer as the library's converter, at import (ADR-0032) and for signing converted code again
/// (ADR-0023). The library calls it from its actor, one file after the other, never at the same time.
struct SignerConverter: ContainerConverting, @unchecked Sendable {
    let signer: ContainerSigner
    var containerVersion: UInt32 { ContainerSigner.containerVersion }

    func convert(_ elf: URL, to container: URL) throws {
        do {
            try signer.convert(elf, to: container)
        } catch CodeSigningError.converter(let message) {
            // Code that cannot run on iOS as it is, which APKConvert marks (apkconv.h).
            if let refusal = ConversionRefused(apkConvertMessage: message) { throw refusal }
            throw CodeSigningError.converter(message)
        }
    }
}

/// Converts ELF files into container dylibs and signs them (APKConvert, ADR-0022).
final class ContainerSigner {
    private let signer: OpaquePointer

    private static var identifier: String { Bundle.main.bundleIdentifier ?? "io.github.kisame76.minato" }

    /// The format version of the containers this app's converter writes (APK_CONTAINER_VERSION).
    static let containerVersion = apkconv_container_version()

    /// With `identity` nil the containers are signed ad hoc, which only the Simulator runs.
    init(identity: SecIdentity?) throws {
        var error = converterErrorBuffer()
        guard let signer = apkconv_signer_create(identity, Bundle.main.executablePath, Self.identifier, &error, error.count) else {
            throw CodeSigningError.converter(converterMessage(in: error))
        }
        self.signer = signer
    }

    /// For a private key and its certificate (DER) kept as two keychain items.
    init(privateKey: SecKey, certificate: Data) throws {
        var error = converterErrorBuffer()
        let created = certificate.withUnsafeBytes {
            apkconv_signer_create_with_key(privateKey, $0.baseAddress, $0.count, Bundle.main.executablePath, Self.identifier,
                                           &error, error.count)
        }
        guard let created else { throw CodeSigningError.converter(converterMessage(in: error)) }
        signer = created
    }

    deinit {
        apkconv_signer_free(signer)
    }

    /// Converts the ELF file `elf` into the container `container` and signs it.
    @discardableResult
    func convert(_ elf: URL, to container: URL) throws -> apkconv_statistics {
        var statistics = apkconv_statistics()
        var error = converterErrorBuffer()
        guard apkconv_convert(signer, elf.path, container.path, &statistics, &error, error.count) == 0 else {
            throw CodeSigningError.converter(converterMessage(in: error))
        }
        return statistics
    }

    /// Signs a test container in `directory`, loads it with dlopen and calls it (ADR-0023).
    func selfTest(in directory: URL) throws -> apkconv_self_test_result {
        var result = apkconv_self_test_result()
        var error = converterErrorBuffer()
        guard apkconv_self_test(signer, directory.path, &result, &error, error.count) == 0 else {
            throw CodeSigningError.converter(converterMessage(in: error))
        }
        return result
    }
}
