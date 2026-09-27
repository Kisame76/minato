import APKKit
import CryptoKit
import Foundation
import Security

/// The debug key that signs our test apps (ADR-0031, decision 6): an RSA 2048 key and its
/// self-signed certificate, made once by LibreSSL and kept outside the repo, as Android Studio keeps
/// ~/.android/debug.keystore. `@unchecked` because a SecKey never changes and signs on any thread.
public struct DebugKey: @unchecked Sendable {
    /// The self-signed certificate, DER.
    public let certificate: Data
    /// The certificate's SubjectPublicKeyInfo, exactly as it stands in the certificate.
    public let subjectPublicKeyInfo: Data
    package let privateKey: SecKey

    /// ~/.android/minato-debug-key, next to Android Studio's debug key.
    public static var standardFolder: URL {
        FileManager.default.homeDirectoryForCurrentUser.appending(path: ".android/minato-debug-key", directoryHint: .isDirectory)
    }

    /// The key in `folder` (key.der, PKCS#1, and certificate.der), made first when the folder does
    /// not exist. Refuses a key that does not belong to its certificate.
    public static func load(from folder: URL) throws -> DebugKey {
        if !FileManager.default.fileExists(atPath: folder.path) {
            try make(in: folder)
        }
        let keyFile = folder.appending(path: "key.der")
        let certificateFile = folder.appending(path: "certificate.der")
        let keyData: Data
        let certificate: Data
        do {
            keyData = try Data(contentsOf: keyFile)
            certificate = try Data(contentsOf: certificateFile)
        } catch {
            throw PackError(file: folder.path, message: "cannot read the debug key: \(error.localizedDescription)")
        }

        let attributes = [kSecAttrKeyType: kSecAttrKeyTypeRSA, kSecAttrKeyClass: kSecAttrKeyClassPrivate] as CFDictionary
        guard let privateKey = SecKeyCreateWithData(keyData as CFData, attributes, nil) else {
            throw PackError(file: keyFile.path, message: "not an RSA private key (PKCS#1, DER)")
        }
        guard let parsed = SecCertificateCreateWithData(nil, certificate as CFData), let publicKey = SecCertificateCopyKey(parsed),
              let subjectPublicKeyInfo = try? X509.subjectPublicKeyInfo(of: certificate)
        else {
            throw PackError(file: certificateFile.path, message: "not an X.509 certificate (DER)")
        }
        let key = DebugKey(certificate: certificate, subjectPublicKeyInfo: subjectPublicKeyInfo, privateKey: privateKey)
        // Once per run: a signature made with the key must verify with the certificate's public key.
        let probe = Data("Minato debug key".utf8)
        guard let signature = try? key.sign(probe),
              SecKeyVerifySignature(publicKey, .rsaSignatureMessagePKCS1v15SHA256, probe as CFData, signature as CFData, nil)
        else {
            throw PackError(file: keyFile.path, message: "does not belong to \(certificateFile.lastPathComponent)")
        }
        return key
    }

    /// RSASSA-PKCS1-v1_5 with SHA-256 over `message`: deterministic, so the same APK comes out twice.
    func sign(_ message: Data) throws -> Data {
        var error: Unmanaged<CFError>?
        guard let signature = SecKeyCreateSignature(privateKey, .rsaSignatureMessagePKCS1v15SHA256, message as CFData, &error) else {
            throw PackError(file: "debug key", message: "cannot sign: \(error.map { "\($0.takeRetainedValue())" } ?? "unknown error")")
        }
        return signature as Data
    }

    /// Makes key and certificate with /usr/bin/openssl (LibreSSL, part of macOS) in a temporary
    /// folder next to `folder` and renames it into place, so that two builds at the same time end up
    /// with one key. Android does not check the dates of an APK's certificate; 30 years as Android Studio.
    static func make(in folder: URL) throws {
        #if os(macOS)
        let fileManager = FileManager.default
        let parent = folder.deletingLastPathComponent()
        try fileManager.createDirectory(at: parent, withIntermediateDirectories: true)
        let temporary = parent.appending(path: ".\(folder.lastPathComponent)-\(UUID().uuidString)", directoryHint: .isDirectory)
        try fileManager.createDirectory(at: temporary, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
        defer { try? fileManager.removeItem(at: temporary) }

        let keyPEM = temporary.appending(path: "key.pem").path
        let certificatePEM = temporary.appending(path: "certificate.pem").path
        let keyDER = temporary.appending(path: "key.der").path
        try openssl(["req", "-x509", "-newkey", "rsa:2048", "-nodes", "-sha256", "-days", "10950",
                     "-subj", "/CN=Minato Debug/O=Minato", "-keyout", keyPEM, "-out", certificatePEM])
        try openssl(["rsa", "-in", keyPEM, "-outform", "DER", "-out", keyDER])
        try openssl(["x509", "-in", certificatePEM, "-outform", "DER", "-out", temporary.appending(path: "certificate.der").path])
        try fileManager.removeItem(atPath: keyPEM)
        try fileManager.removeItem(atPath: certificatePEM)
        try fileManager.setAttributes([.posixPermissions: 0o600], ofItemAtPath: keyDER)
        do {
            try fileManager.moveItem(at: temporary, to: folder)
        } catch where fileManager.fileExists(atPath: folder.path) {
            // Another build made the key first; that one counts.
        }
        #else
        throw PackError(file: folder.path, message: "the debug key is made on the Mac")
        #endif
    }

    #if os(macOS)
    private static func openssl(_ arguments: [String]) throws {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/openssl")
        process.arguments = arguments
        process.standardInput = FileHandle.nullDevice
        process.standardOutput = FileHandle.nullDevice
        let errors = Pipe()
        process.standardError = errors
        try process.run()
        let message = errors.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()
        guard process.terminationStatus == 0 else {
            throw PackError(file: "/usr/bin/openssl", message: "openssl \(arguments[0]) failed: \(String(decoding: message, as: UTF8.self))")
        }
    }
    #endif
}

/// APK Signature Scheme v2 (ADR-0031, decision 5): one signer, RSASSA-PKCS1-v1_5 with SHA-256, the
/// signing block between the last entry and the central directory. No v1, v3 or padding block.
public enum APKSigner {
    /// RSASSA-PKCS1-v1_5 with SHA2-256, content digested with SHA2-256 in chunks of 1 MiB.
    static let algorithm: UInt32 = 0x0103
    static let v2BlockID: UInt32 = 0x7109_871A
    static let chunkSize = 1 << 20

    /// The v2 content digest of an unsigned archive: its entries (up to the central directory), the
    /// central directory, and the end record whose central directory offset counts as the signing
    /// block's, all in chunks of 1 MiB: SHA-256 of 0xa5, the chunk's length and the chunk; then
    /// SHA-256 of 0x5a, the number of chunks and their digests.
    public static func contentDigest(of data: Data, centralDirectoryOffset: Int, endRecordOffset: Int) -> Data {
        let data = Data(data)
        var endRecord = data.subdata(in: endRecordOffset..<data.count)
        endRecord.replaceSubrange(16..<20, with: littleEndian(UInt32(centralDirectoryOffset)))
        let sections = [
            data.subdata(in: 0..<centralDirectoryOffset),
            data.subdata(in: centralDirectoryOffset..<endRecordOffset),
            endRecord,
        ]

        var chunkDigests = Data()
        var chunks = 0
        for section in sections {
            var start = 0
            while start < section.count {
                let end = min(start + chunkSize, section.count)
                var hasher = SHA256()
                hasher.update(data: Data([0xA5]) + littleEndian(UInt32(end - start)))
                hasher.update(data: section.subdata(in: start..<end))
                chunkDigests.append(contentsOf: hasher.finalize())
                chunks += 1
                start = end
            }
        }
        var hasher = SHA256()
        hasher.update(data: Data([0x5A]) + littleEndian(UInt32(chunks)))
        hasher.update(data: chunkDigests)
        return Data(hasher.finalize())
    }

    /// The signed APK: `unsigned` with a v2 signing block by `key` before its central directory.
    public static func sign(_ unsigned: Data, with key: DebugKey) throws -> Data {
        let data = Data(unsigned)
        let (centralDirectory, endRecord) = try layout(of: data)
        let digest = contentDigest(of: data, centralDirectoryOffset: centralDirectory, endRecordOffset: endRecord)
        let signed = signedData(digests: [(algorithm, digest)], certificates: [key.certificate])
        let signer = signer(signedData: signed, signatures: [(algorithm, try key.sign(signed))], publicKey: key.subjectPublicKeyInfo)
        return insert(signingBlock(v2Signers: [signer]), into: data)
    }

    // The parts of the signature, each as the specification lays it out; tests build from them what
    // Android must refuse.

    /// Where the central directory and the end record of an unsigned archive lie.
    static func layout(of data: Data) throws -> (centralDirectory: Int, endRecord: Int) {
        guard let offsets = locateEndRecord(in: data) else {
            throw PackError(file: "APK", message: "not a ZIP archive with a central directory, or a ZIP64 one")
        }
        return offsets
    }

    /// Signed data: the digests, the certificates, and the additional attributes as they are given.
    static func signedData(digests: [(algorithm: UInt32, digest: Data)], certificates: [Data],
                           additionalAttributes: Data = Data()) -> Data {
        prefixed(digests.map { prefixed(littleEndian($0.algorithm) + prefixed($0.digest)) }.reduce(Data(), +))
            + prefixed(certificates.map(prefixed).reduce(Data(), +))
            + prefixed(additionalAttributes)
    }

    /// A signer: its signed data, its signatures over them, and its public key.
    static func signer(signedData: Data, signatures: [(algorithm: UInt32, signature: Data)], publicKey: Data) -> Data {
        prefixed(signedData)
            + prefixed(signatures.map { prefixed(littleEndian($0.algorithm) + prefixed($0.signature)) }.reduce(Data(), +))
            + prefixed(publicKey)
    }

    /// The APK Signing Block with a v2 block of these signers.
    static func signingBlock(v2Signers: [Data]) -> Data {
        let value = prefixed(v2Signers.map(prefixed).reduce(Data(), +))
        var pairs = ByteWriter()
        pairs.u64(UInt64(4 + value.count))
        pairs.u32(v2BlockID)
        pairs.bytes(value)
        let size = UInt64(pairs.count + 8 + 16)
        var block = ByteWriter()
        block.u64(size)
        block.bytes(pairs.data)
        block.u64(size)
        block.bytes(Data("APK Sig Block 42".utf8))
        return block.data
    }

    /// `data` with `block` before its central directory and the end record's offset moved by its size.
    static func insert(_ block: Data, into data: Data) -> Data {
        guard let (centralDirectory, endRecord) = locateEndRecord(in: data) else { return data }
        var end = data.subdata(in: endRecord..<data.count)
        end.replaceSubrange(16..<20, with: littleEndian(UInt32(centralDirectory + block.count)))
        return data.subdata(in: 0..<centralDirectory) + block + data.subdata(in: centralDirectory..<endRecord) + end
    }

    /// The offsets of the central directory and of the end record (0x06054b50), searched from the end
    /// over at most a full comment. ZIP64 archives are not written by make-apk and are refused.
    static func locateEndRecord(in data: Data) -> (centralDirectory: Int, endRecord: Int)? {
        guard data.count >= 22 else { return nil }
        func u16(_ at: Int) -> Int { Int(data[at]) | Int(data[at + 1]) << 8 }
        func u32(_ at: Int) -> Int { u16(at) | u16(at + 2) << 16 }
        for offset in stride(from: data.count - 22, through: max(0, data.count - 22 - 0xFFFF), by: -1)
        where u32(offset) == 0x0605_4B50 && offset + 22 + u16(offset + 20) == data.count {
            let centralDirectory = u32(offset + 16)
            guard centralDirectory != 0xFFFF_FFFF, centralDirectory <= offset else { return nil }
            return (centralDirectory, offset)
        }
        return nil
    }

    static func prefixed(_ data: Data) -> Data {
        littleEndian(UInt32(data.count)) + data
    }

    static func littleEndian(_ value: UInt32) -> Data {
        withUnsafeBytes(of: value.littleEndian) { Data($0) }
    }
}
