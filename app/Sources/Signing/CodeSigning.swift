import APKConvert
import Foundation
import Security

/// The development certificate APK-Runner signs converted native code with, so that iOS runs it
/// (ADR-0008, ADR-0022). It is imported from a .p12 file and kept in the app's keychain, on this
/// device only; the file's password is not kept.
struct SigningCertificate: Equatable {
    let name: String
    let team: String
    let expires: Date
}

enum CodeSigningError: LocalizedError {
    case wrongPassword
    case unreadableFile(OSStatus)
    case noIdentity
    case keychain(OSStatus)
    case converter(String)

    var errorDescription: String? {
        switch self {
        case .wrongPassword: "The password is not correct."
        case .unreadableFile(let status): "The file is not a certificate APK-Runner can read (error \(status))."
        case .noIdentity: "The file contains no certificate with its private key."
        case .keychain(let status): "The certificate could not be saved in the keychain (error \(status))."
        case .converter(let message): message
        }
    }
}

enum CodeSigning {
    /// The imported identity: certificate and private key.
    static func storedIdentity() -> SecIdentity? {
        let query: [CFString: Any] = [kSecClass: kSecClassIdentity, kSecReturnRef: true, kSecMatchLimit: kSecMatchLimitOne]
        var result: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &result) == errSecSuccess, let result,
              CFGetTypeID(result) == SecIdentityGetTypeID()
        else { return nil }
        return (result as! SecIdentity)
    }

    static func storedCertificate() -> SigningCertificate? {
        storedIdentity().flatMap(certificate(of:))
    }

    /// The imported certificate, in DER.
    static func storedCertificateData() -> Data? {
        var certificate: SecCertificate?
        guard let identity = storedIdentity(), SecIdentityCopyCertificate(identity, &certificate) == errSecSuccess,
              let certificate
        else { return nil }
        return SecCertificateCopyData(certificate) as Data
    }

    /// The certificate the app itself is signed with, in DER: on a device the one Xcode or SideStore
    /// installed it with, which iOS accepts for converted code. nil when the app is signed ad hoc,
    /// as on the Simulator.
    static func appCertificate() -> Data? {
        guard let executable = Bundle.main.executablePath else { return nil }
        var buffer = [UInt8](repeating: 0, count: 16384)
        var length = 0
        var error = [CChar](repeating: 0, count: 256)
        guard apkconv_signing_certificate(executable, &buffer, buffer.count, &length, &error, error.count) == 0 else {
            return nil
        }
        return Data(buffer.prefix(length))
    }

    static func certificate(of identity: SecIdentity) -> SigningCertificate? {
        var certificate: SecCertificate?
        guard SecIdentityCopyCertificate(identity, &certificate) == errSecSuccess, let certificate else { return nil }
        let der = SecCertificateCopyData(certificate) as Data
        var description = apkconv_certificate()
        var error = [CChar](repeating: 0, count: 256)
        let described = der.withUnsafeBytes { apkconv_describe_certificate($0.baseAddress, $0.count, &description, &error, error.count) }
        guard described == 0 else { return nil }
        return SigningCertificate(name: text(of: &description.common_name), team: text(of: &description.team),
                                  expires: Date(timeIntervalSince1970: TimeInterval(description.not_after)))
    }

    /// Imports the first identity of a .p12 file. On a device it first checks that iOS accepts code
    /// signed with it; then it replaces the one imported before.
    static func importIdentity(from data: Data, password: String) throws -> SigningCertificate {
        var items: CFArray?
        let status = SecPKCS12Import(data as CFData, [kSecImportExportPassphrase: password] as CFDictionary, &items)
        guard status != errSecAuthFailed else { throw CodeSigningError.wrongPassword }
        guard status == errSecSuccess else { throw CodeSigningError.unreadableFile(status) }
        guard let first = (items as? [[CFString: Any]])?.first, let value = first[kSecImportItemIdentity],
              CFGetTypeID(value as CFTypeRef) == SecIdentityGetTypeID()
        else { throw CodeSigningError.noIdentity }
        let identity = value as! SecIdentity
        guard let certificate = certificate(of: identity) else { throw CodeSigningError.noIdentity }

        #if !targetEnvironment(simulator)
        // The Simulator runs ad-hoc signed code and has no chain to complete; a device checks for real.
        let signer = try ContainerSigner(identity: identity)
        try signer.check(in: URL.temporaryDirectory)
        #endif

        removeIdentity()
        let attributes: [CFString: Any] = [
            kSecValueRef: identity,
            kSecAttrAccessible: kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly,
        ]
        let added = SecItemAdd(attributes as CFDictionary, nil)
        guard added == errSecSuccess else { throw CodeSigningError.keychain(added) }
        return certificate
    }

    /// Removes the imported identity: its certificate and its private key.
    static func removeIdentity() {
        for itemClass in [kSecClassIdentity, kSecClassCertificate, kSecClassKey] {
            SecItemDelete([kSecClass: itemClass] as CFDictionary)
        }
    }

    /// The signer for converted code: on a device with the imported identity, ad hoc on the Simulator.
    static func signer() throws -> ContainerSigner {
        #if targetEnvironment(simulator)
        return try ContainerSigner(identity: nil)
        #else
        guard let identity = storedIdentity() else {
            throw CodeSigningError.converter("No signing certificate is imported.")
        }
        return try ContainerSigner(identity: identity)
        #endif
    }

    private static func text<T>(of field: inout T) -> String {
        withUnsafeBytes(of: &field) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
    }
}

/// The text a C function of APKConvert left in its error buffer.
private func message(in buffer: [CChar]) -> String {
    String(decoding: buffer.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }, as: UTF8.self)
}

/// Converts ELF files into container dylibs and signs them (APKConvert, ADR-0022).
final class ContainerSigner {
    private let signer: OpaquePointer

    /// With `identity` nil the containers are signed ad hoc, which only the Simulator runs.
    init(identity: SecIdentity?) throws {
        var error = [CChar](repeating: 0, count: 1024)
        let identifier = Bundle.main.bundleIdentifier ?? "io.github.kisame76.apkrunner"
        guard let signer = apkconv_signer_create(identity, Bundle.main.executablePath, identifier, &error, error.count) else {
            throw CodeSigningError.converter(message(in: error))
        }
        self.signer = signer
    }

    deinit {
        apkconv_signer_free(signer)
    }

    /// Converts the ELF file `elf` into the container `container` and signs it.
    @discardableResult
    func convert(_ elf: URL, to container: URL) throws -> apkconv_statistics {
        var statistics = apkconv_statistics()
        var error = [CChar](repeating: 0, count: 1024)
        guard apkconv_convert(signer, elf.path, container.path, &statistics, &error, error.count) == 0 else {
            throw CodeSigningError.converter(message(in: error))
        }
        return statistics
    }

    /// Signs a small test container in `directory` and asks the kernel whether it would load it.
    func check(in directory: URL) throws {
        var error = [CChar](repeating: 0, count: 1024)
        guard apkconv_check_signer(signer, directory.path, &error, error.count) == 0 else {
            throw CodeSigningError.converter(message(in: error))
        }
    }
}
