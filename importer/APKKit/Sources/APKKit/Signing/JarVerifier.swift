import CryptoKit
import Foundation
import Security

/// Verifies JAR signing (v1) as Android's platform verifier does, StrictJarVerifier with the checks of
/// ApkSignatureVerifier (ADR-0034, step d): each signature block's PKCS#7 signature over its .SF, the
/// .SF against MANIFEST.MF, and every entry outside META-INF/ against its digest in the manifest and
/// in every signer's .SF. Entries stream through their digest, one piece at a time.
enum JarVerifier {
    static let manifestName = "META-INF/MANIFEST.MF"
    /// The digests Android reads, strongest first; a manifest names them "<name>-Digest".
    static let digestNames = ["SHA-512", "SHA-384", "SHA-256", "SHA1"]
    /// Far above a real manifest (about 100 bytes per entry), and bounded: both are read whole.
    static let maximumSignatureFileSize: UInt64 = 16 << 20
    /// As apksig: more signers than this is no real APK.
    static let maximumSigners = 10

    static func verify(_ archive: ZipArchive, present: Set<UInt32>) throws -> SignatureVerification {
        let blocks = archive.entries.filter { APKSignatures.isV1SignatureFile($0.name) }
        guard !blocks.isEmpty else { return SignatureVerification(check: .unsigned) }
        guard blocks.count <= maximumSigners else {
            throw SignatureFailure(.signatureMismatch, "\(blocks.count) v1 signature blocks, more than \(maximumSigners)")
        }
        var entries: [ExactName: ZipEntry] = [:]
        for entry in archive.entries {
            guard entries.updateValue(entry, forKey: ExactName(entry.name)) == nil else {
                throw SignatureFailure(.damaged, "\(entry.name) appears twice")
            }
        }
        guard let manifestEntry = entries[ExactName(manifestName)] else {
            throw SignatureFailure(.damaged, "\(manifestName) is missing")
        }
        let manifestBytes = try read(manifestEntry, from: archive)
        let manifest: JarManifest
        do {
            manifest = try JarManifest(manifestBytes, name: manifestName)
        } catch let error as JarManifest.Unreadable {
            throw SignatureFailure(.damaged, error.reason)
        }
        // As StrictJarFile, before any signature: every section names an entry ("in manifest does not
        // exist"), so no signed entry can be left out.
        if let missing = manifest.sections.keys.first(where: { entries[$0] == nil }) {
            throw SignatureFailure(.damaged, "the manifest lists \(missing), which the file lacks")
        }

        // Android verifies the blocks in the order of StrictJarFile's metaEntries, a java.util.HashMap over the META-INF
        // names (readCertificates, StrictJarVerifier.java:296-316), and keys each chain by its .SF (383). Once a block's
        // .SF signs, it drops the .SF's bytes (487), so the .SF's later blocks go unread (368-371): its first block that
        // signs is the signer. A .SF that gives up keeps its bytes, so each of its blocks is verified.
        let rank = hashMapOrder(archive.entries.map(\.name).filter { $0.utf8.starts(with: "META-INF/".utf8) })
        var signers: [ExactName: (certificate: Data?, names: Set<ExactName>)] = [:]
        var unverifiable: String?
        for block in blocks.sorted(by: { rank[ExactName($0.name)]! < rank[ExactName($1.name)]! }) {
            let signatureFileName = Self.signatureFileName(of: block)
            // A signature block without its .SF signs nothing, as on Android.
            guard signers[ExactName(signatureFileName)] == nil, let signatureFileEntry = entries[ExactName(signatureFileName)] else {
                continue
            }
            let signatureFileBytes = try read(signatureFileEntry, from: archive)
            // The signature first, as Android: a changed .SF is a signature that does not match.
            var certificate: Data?
            do {
                certificate = try PKCS7Signature.verify(try read(block, from: archive, maximum: 1 << 20), content: signatureFileBytes,
                                                        name: block.name)
            } catch let error as Unverifiable {
                unverifiable = error.reason
            }
            if let signatureFile = try checkedSignatureFile(signatureFileBytes, name: signatureFileName, against: manifest,
                                                            bytes: manifestBytes, present: present) {
                signers[ExactName(signatureFileName)] = (certificate, Set(signatureFile.sections.keys))
            }
        }
        // Several signers keep the archive's order (a logged gap: Android's comes from a Hashtable).
        var seen: Set<ExactName> = []
        let ordered = blocks.compactMap { block in
            let name = ExactName(signatureFileName(of: block))
            return seen.insert(name).inserted ? signers[name] : nil
        }
        guard !ordered.isEmpty else { throw SignatureFailure(.signatureMismatch, "no signature block signs the file") }

        for entry in archive.entries where !entry.isDirectory && !entry.name.utf8.starts(with: "META-INF/".utf8) {
            let name = ExactName(entry.name)
            guard let section = manifest.sections[name] else {
                throw SignatureFailure(.signatureMismatch, "\(entry.name) is not signed")
            }
            guard ordered.allSatisfy({ $0.names.contains(name) }) else {
                throw SignatureFailure(.signatureMismatch, "\(entry.name) is not signed by every signer")
            }
            guard let (digestName, expected) = digest(in: section.attributes, suffix: "-Digest") else {
                throw SignatureFailure(.signatureMismatch, "\(entry.name) has no digest in the manifest")
            }
            guard try streamedDigest(of: entry, in: archive, digestName) == expected else {
                throw SignatureFailure(.damaged, "\(entry.name) does not match its digest")
            }
        }
        if let unverifiable { return SignatureVerification(check: .unverified(unverifiable)) }
        // Each signer is the certificate its block's SignerInfo names and whose key verified its .SF
        // (StrictJarVerifier.java:330-353), not whichever certificate the block lists first.
        return SignatureVerification(check: .verified, proof: SignerProof(scheme: .v1, certificates: ordered.compactMap(\.certificate)))
    }

    /// Where each name comes as Android iterates a java.util.HashMap of these names put in this order, such as
    /// StrictJarFile's metaEntries (StrictJarVerifier.readCertificates, 296-316): by bucket of the spread
    /// String.hashCode, and within a bucket in the order put. The table of 16 doubles when the names fill more than
    /// three quarters of it, and while it is below 64, also for a bucket's 9th name (HashMap.java:648-649, 667-668,
    /// 760-763). From 64 on, such a bucket becomes a tree, whose order Minato does not follow (a logged gap). Android
    /// puts metaEntries in libziparchive's order and Minato in the archive's (another).
    static func hashMapOrder(_ names: [String]) -> [ExactName: Int] {
        let hashes = names.map { name in
            var hash: Int32 = 0
            for unit in name.utf16 { hash = hash &* 31 &+ Int32(unit) }
            return Int(hash ^ Int32(bitPattern: UInt32(bitPattern: hash) >> 16))
        }
        var capacity = 16
        var counts: [Int: Int] = [:]
        for (index, hash) in hashes.enumerated() {
            let chain = counts[hash & (capacity - 1), default: 0]
            counts[hash & (capacity - 1)] = chain + 1
            let before = capacity
            if chain >= 8, capacity < 64 { capacity *= 2 }
            if index + 1 > capacity / 4 * 3 { capacity *= 2 }
            if capacity != before {
                counts = Dictionary(hashes[...index].map { ($0 & (capacity - 1), 1) }, uniquingKeysWith: +)
            }
        }
        let ordered = hashes.indices.sorted { (hashes[$0] & (capacity - 1), $0) < (hashes[$1] & (capacity - 1), $1) }
        return Dictionary(ordered.enumerated().map { (ExactName(names[$1]), $0) }, uniquingKeysWith: { first, _ in first })
    }

    /// The .SF of a signature block: its name up to the last dot, then ".SF" (StrictJarVerifier.java:367).
    static func signatureFileName(of block: ZipEntry) -> String {
        String(block.name[..<block.name.lastIndex(of: ".")!]) + ".SF"
    }

    /// The .SF after its block, as verifyCertificate reads it (StrictJarVerifier.java:389-488): nil when Android gives
    /// up on it, which leaves it out but keeps its bytes for its next block. Android gives up on a .SF its reader
    /// cannot read, on one without its version, an empty one too, and on one that signs an entry the manifest lacks.
    static func checkedSignatureFile(_ bytes: Data, name: String, against manifest: JarManifest, bytes manifestBytes: Data,
                                     present: Set<UInt32>) throws -> JarManifest? {
        let signatureFile: JarManifest
        do {
            signatureFile = try JarManifest(bytes, name: name, signatureFile: true)
        } catch is JarManifest.Unreadable {
            return nil
        }
        // The stripped signatures before the version (399-452).
        try checkStripping(signatureFile, name: name, present: present)
        guard signatureFile.main["signature-version"] != nil else { return nil }
        return try verify(signatureFile, name: name, against: manifest, bytes: manifestBytes) ? signatureFile : nil
    }

    static func read(_ entry: ZipEntry, from archive: ZipArchive, maximum: UInt64 = maximumSignatureFileSize) throws -> Data {
        do {
            return try archive.data(for: entry, maxSize: maximum)
        } catch {
            throw SignatureFailure(.damaged, "\(entry.name): \(error)")
        }
    }

    /// An .SF whose X-Android-APK-Signed names v2 or v3, when that block is gone: someone removed it
    /// to fall back to v1 (Android's "signature stripped?").
    static func checkStripping(_ signatureFile: JarManifest, name: String, present: Set<UInt32>) throws {
        guard let header = signatureFile.main["x-android-apk-signed"] else { return }
        for id in header.split(separator: ",").compactMap({ Int($0.trimmingCharacters(in: .whitespaces)) }) {
            if id == 2, !present.contains(SignatureVerifier.v2ID) {
                throw SignatureFailure(.signatureMismatch, "\(name) says the APK has a v2 signature, which was removed")
            }
            if id == 3, !present.contains(SignatureVerifier.v3ID) {
                throw SignatureFailure(.signatureMismatch, "\(name) says the APK has a v3 signature, which was removed")
            }
        }
    }

    /// The .SF against the manifest: its main section's digest when the .SF has one, then the whole
    /// manifest's digest, or else each section's, in the order of the .SF's HashMap (StrictJarVerifier.java:
    /// 474-485). False when Android gives up on the .SF: a section names an entry the manifest lacks before
    /// any section fails.
    static func verify(_ signatureFile: JarManifest, name: String, against manifest: JarManifest, bytes: Data) throws -> Bool {
        let bySigntool = signatureFile.main["created-by"]?.contains("signtool") == true
        if manifest.mainEnd > 0, !bySigntool,
           let (digestName, expected) = digest(in: signatureFile.main, suffix: "-Digest-Manifest-Main-Attributes"),
           Self.digest(bytes.subdata(in: 0..<manifest.mainEnd), digestName) != expected {
            throw SignatureFailure(.damaged, "the manifest's main section does not match \(name)")
        }
        if let (digestName, expected) = digest(in: signatureFile.main, suffix: bySigntool ? "-Digest" : "-Digest-Manifest"),
           Self.digest(bytes, digestName) == expected {
            return true
        }
        let rank = hashMapOrder(signatureFile.order.map(\.description))
        for entryName in signatureFile.order.sorted(by: { rank[$0]! < rank[$1]! }) {
            guard let chunk = manifest.sections[entryName] else { return false }
            guard let (digestName, expected) = digest(in: signatureFile.sections[entryName]!.attributes, suffix: "-Digest") else {
                throw SignatureFailure(.signatureMismatch, "\(name) has no digest for \(entryName)")
            }
            var range = chunk.range
            // signtool leaves out the section's second line end.
            if bySigntool, range.count >= 2, bytes[range.upperBound - 1] == 0x0A, bytes[range.upperBound - 2] == 0x0A {
                range = range.lowerBound..<range.upperBound - 1
            }
            guard Self.digest(bytes.subdata(in: range), digestName) == expected else {
                throw SignatureFailure(.damaged, "the manifest's section of \(entryName) does not match \(name)")
            }
        }
        return true
    }

    /// The strongest "<digest><suffix>" attribute Android reads, decoded; nil without one.
    static func digest(in attributes: [String: String], suffix: String) -> (name: String, value: Data)? {
        for name in digestNames {
            if let text = attributes[(name + suffix).lowercased()] {
                return (name, Data(base64Encoded: text) ?? Data())
            }
        }
        return nil
    }

    static func digest(_ data: Data, _ name: String) -> Data {
        switch name {
        case "SHA-512": Data(SHA512.hash(data: data))
        case "SHA-384": Data(SHA384.hash(data: data))
        case "SHA-256": Data(SHA256.hash(data: data))
        default: Data(Insecure.SHA1.hash(data: data))
        }
    }

    static func streamedDigest(of entry: ZipEntry, in archive: ZipArchive, _ name: String) throws -> Data {
        func stream<H: HashFunction>(_ hasher: inout H) throws {
            do {
                try archive.forEachChunk(of: entry, chunkSize: SignatureVerifier.chunkSize) { hasher.update(data: $0) }
            } catch {
                throw SignatureFailure(.damaged, "\(entry.name): \(error)")
            }
        }
        switch name {
        case "SHA-512":
            var hasher = SHA512()
            try stream(&hasher)
            return Data(hasher.finalize())
        case "SHA-384":
            var hasher = SHA384()
            try stream(&hasher)
            return Data(hasher.finalize())
        case "SHA-256":
            var hasher = SHA256()
            try stream(&hasher)
            return Data(hasher.finalize())
        default:
            var hasher = Insecure.SHA1()
            try stream(&hasher)
            return Data(hasher.finalize())
        }
    }
}

/// An entry name as Android compares it, code point by code point. Swift's String == and hashing treat
/// "é" and "e" with a combining accent as one name; Android's lookups do not.
struct ExactName: Hashable, CustomStringConvertible, ExpressibleByStringLiteral {
    let utf8: [UInt8]

    init(_ name: String) { utf8 = Array(name.utf8) }
    init(stringLiteral name: String) { self.init(name) }

    var description: String { String(decoding: utf8, as: UTF8.self) }
}

/// A JAR manifest or signature file, read as Android's StrictJarManifestReader reads it: the main
/// section's attributes, and each named section with its attributes and its bytes, from its first line
/// to the next section (the blank lines that end it included). Lines end with CR LF, LF or CR; a line
/// that starts with a space continues the one before; a last line without its end does not count.
/// Attribute names ignore case, and have 1 to 70 ASCII letters, digits, "_" and "-" (Attributes.java:480-503).
struct JarManifest {
    /// What Android's reader throws as an IOException: the APK is refused for its manifest, and a .SF left out
    /// (StrictJarVerifier.java:392-397). Its other faults are runtime exceptions, which refuse the APK either way
    /// (ApkSignatureVerifier.java:502-504) and throw a SignatureFailure here.
    struct Unreadable: Error {
        let reason: String
    }

    struct Section {
        var attributes: [String: String]
        var range: Range<Int>
    }

    var main: [String: String] = [:]
    /// Where the main section ends, after the blank lines that end it.
    var mainEnd = 0
    var sections: [ExactName: Section] = [:]
    /// The sections' names in the order they first come.
    var order: [ExactName] = []

    /// In a .SF (`signatureFile`), a second section of one name adds to the first; in a manifest it is unreadable
    /// (StrictJarManifestReader.java:66-89).
    init(_ data: Data, name file: String, signatureFile: Bool = false) throws {
        var reader = Reader(bytes: [UInt8](data), file: file)
        while let (name, value) = try reader.header() {
            main[name] = value
        }
        mainEnd = reader.position
        var start = reader.position
        while let (name, value) = try reader.header() {
            guard name == "name" else { throw Unreadable(reason: "\(file) has a section without a name") }
            var attributes = ["name": value]
            while let (name, value) = try reader.header() {
                attributes[name] = value
            }
            let key = ExactName(value)
            if sections[key] == nil {
                sections[key] = Section(attributes: attributes, range: start..<reader.position)
                order.append(key)
            } else if signatureFile {
                sections[key]!.attributes.merge(attributes) { $1 }
            } else {
                throw Unreadable(reason: "\(file) lists \(value) twice")
            }
            start = reader.position
        }
    }

    /// StrictJarManifestReader's readHeader, readName and readValue.
    struct Reader {
        let bytes: [UInt8]
        let file: String
        var position = 0
        var lineBreaks = 0

        init(bytes: [UInt8], file: String) {
            self.bytes = bytes
            self.file = file
        }

        /// The next attribute, its name lowercased; nil at the end of a section or of the file.
        mutating func header() throws -> (String, String)? {
            if lineBreaks > 1 {
                lineBreaks = 0
                return nil
            }
            let name = try readName()
            lineBreaks = 0
            let value = try readValue()
            guard lineBreaks > 0, let name else { return nil }
            return (name, value)
        }

        mutating func readName() throws -> String? {
            let mark = position
            while position < bytes.count {
                position += 1
                guard bytes[position - 1] == 0x3A else { continue }
                let name = bytes[mark..<position - 1]
                let text = String(decoding: name, as: UTF8.self)
                // A colon at the very end makes Android read past it.
                guard position < bytes.count else { throw SignatureFailure(.damaged, "\(file) ends with a colon") }
                guard bytes[position] == 0x20 else {
                    throw Unreadable(reason: "\(file) has an attribute \(text) without a space after its colon")
                }
                position += 1
                guard (1...70).contains(name.count), name.allSatisfy(Self.isNameByte) else {
                    throw Unreadable(reason: "\(file) has an attribute named \(text)")
                }
                return text.lowercased()
            }
            return nil
        }

        static func isNameByte(_ byte: UInt8) -> Bool {
            (0x30...0x39).contains(byte) || (0x41...0x5A).contains(byte) || (0x61...0x7A).contains(byte) || byte == 0x5F || byte == 0x2D
        }

        mutating func readValue() throws -> String {
            var lastCR = false
            var mark = position
            var last = position
            var value: [UInt8] = []
            while position < bytes.count {
                let next = bytes[position]
                position += 1
                switch next {
                case 0:
                    throw Unreadable(reason: "\(file) has a NUL character")
                case 0x0A:
                    if lastCR { lastCR = false } else { lineBreaks += 1 }
                    continue
                case 0x0D:
                    lastCR = true
                    lineBreaks += 1
                    continue
                case 0x20 where lineBreaks == 1:
                    // A continuation line.
                    value += try piece(mark, last)
                    mark = position
                    lineBreaks = 0
                    continue
                default:
                    break
                }
                if lineBreaks >= 1 {
                    position -= 1
                    break
                }
                last = position
            }
            value += try piece(mark, last)
            return String(decoding: value, as: UTF8.self)
        }

        /// The value's bytes from `mark` to `last`. After a continuation line of only a space, `mark` is
        /// past `last`, and Android's write of a negative length throws.
        func piece(_ mark: Int, _ last: Int) throws -> ArraySlice<UInt8> {
            guard mark <= last else { throw SignatureFailure(.damaged, "\(file) has a continuation line without a value") }
            return bytes[mark..<last]
        }
    }
}

/// The PKCS#7 SignedData of a v1 signature block: one SignerInfo over the .SF, directly or through
/// signed attributes, made with the key of a certificate the block carries.
enum PKCS7Signature {
    static let digestOIDs = ["1.3.14.3.2.26": "SHA1", "2.16.840.1.101.3.4.2.1": "SHA-256",
                             "2.16.840.1.101.3.4.2.2": "SHA-384", "2.16.840.1.101.3.4.2.3": "SHA-512"]
    static let rsaOIDs: Set<String> = ["1.2.840.113549.1.1.1", "1.2.840.113549.1.1.5", "1.2.840.113549.1.1.11",
                                       "1.2.840.113549.1.1.12", "1.2.840.113549.1.1.13"]
    static let ecOIDs: Set<String> = ["1.2.840.10045.2.1", "1.2.840.10045.4.1", "1.2.840.10045.4.3.2",
                                      "1.2.840.10045.4.3.3", "1.2.840.10045.4.3.4"]
    static let dsaOIDs: Set<String> = ["1.2.840.10040.4.1", "1.2.840.10040.4.3", "2.16.840.1.101.3.4.3.1", "2.16.840.1.101.3.4.3.2"]
    static let messageDigestOID = "1.2.840.113549.1.9.4"

    /// The signer's certificate once its signature over `content` verifies.
    static func verify(_ block: Data, content: Data, name: String) throws -> Data {
        let parsed: (certificates: [Data], signerInfo: [DERNode], data: Data)
        do {
            parsed = try parse(block)
        } catch let failure as SignatureFailure {
            throw failure
        } catch {
            throw SignatureFailure(.damaged, "\(name): \(error)")
        }
        let data = parsed.data
        let info = parsed.signerInfo
        // SignerInfo: version, issuerAndSerialNumber, digestAlgorithm, [0] signed attributes (optional),
        // digestEncryptionAlgorithm, encryptedDigest.
        let hasAttributes = info.count > 3 && info[3].tag == 0xA0
        let next = hasAttributes ? 4 : 3
        guard info.count > next + 1, info[1].tag == 0x30, info[2].tag == 0x30, info[next].tag == 0x30, info[next + 1].tag == 0x04,
              let issuerAndSerial = try? DER.children(of: info[1], in: data), issuerAndSerial.count == 2
        else { throw SignatureFailure(.damaged, "\(name): not a SignerInfo") }

        let issuer = DER.bytes(of: issuerAndSerial[0], in: data)
        let serial = DER.bytes(of: issuerAndSerial[1], in: data)
        // Android compares the issuer by value, which another encoding of the same name passes; the
        // serial number alone then finds the certificate, whose key must still verify the signature.
        let names = parsed.certificates.map { try? X509.issuerAndSerialNumber(of: $0) }
        guard let index = names.firstIndex(where: { $0?.issuer == issuer && $0?.serial == serial })
            ?? names.firstIndex(where: { $0?.serial == serial })
        else {
            throw SignatureFailure(.signatureMismatch, "\(name) lacks its signer's certificate")
        }
        let certificate = parsed.certificates[index]

        let digestOID = try algorithmOID(info[2], in: data, name: name)
        guard let digestName = digestOIDs[digestOID] else {
            throw SignatureFailure(.signatureMismatch, "\(name) uses the digest \(digestOID)")
        }
        let encryptionOID = try algorithmOID(info[next], in: data, name: name)
        if dsaOIDs.contains(encryptionOID) { throw Unverifiable(reason: "signed with DSA") }
        guard let algorithm = secAlgorithm(encryption: encryptionOID, digest: digestName) else {
            throw SignatureFailure(.signatureMismatch, "\(name) uses the signature algorithm \(encryptionOID)")
        }

        var signed = content
        if hasAttributes {
            // The signature covers the attributes, DER as a SET, and they carry the .SF's digest.
            let messageDigest = try self.messageDigest(in: info[3], data: data, name: name)
            guard messageDigest == JarVerifier.digest(content, digestName) else {
                throw SignatureFailure(.signatureMismatch, "\(name) signs another signature file")
            }
            signed = Data([0x31]) + data.subdata(in: info[3].start + 1..<info[3].end)
        }
        let signature = DER.content(of: info[next + 1], in: data)
        guard SignatureVerifier.verifies(try SignatureVerifier.publicKey(of: certificate), algorithm, signed, signature) else {
            throw SignatureFailure(.signatureMismatch, "the signature in \(name) does not verify")
        }
        return certificate
    }

    static func parse(_ block: Data) throws -> (certificates: [Data], signerInfo: [DERNode], data: Data) {
        let data = Data(block)
        let contentInfo = try DER.node(in: data, at: 0)
        let parts = try DER.children(of: contentInfo, in: data)
        guard contentInfo.tag == 0x30, parts.count == 2, try DER.objectIdentifier(parts[0], in: data) == "1.2.840.113549.1.7.2",
              parts[1].tag == 0xA0, let signedData = try DER.children(of: parts[1], in: data).first, signedData.tag == 0x30
        else { throw SignatureFailure(.damaged, "not a PKCS#7 SignedData") }
        let fields = try DER.children(of: signedData, in: data)
        let certificates = try fields.first(where: { $0.tag == 0xA0 }).map {
            try DER.children(of: $0, in: data).filter { $0.tag == 0x30 }.map { DER.bytes(of: $0, in: data) }
        } ?? []
        guard certificates.count <= APKSignatures.maximumListLength else {
            throw SignatureFailure(.damaged, "\(certificates.count) certificates in one signature block")
        }
        guard let infos = fields.last, infos.tag == 0x31 else { throw SignatureFailure(.damaged, "no SignerInfos") }
        let signerInfos = try DER.children(of: infos, in: data)
        guard signerInfos.count == 1, signerInfos[0].tag == 0x30 else {
            throw SignatureFailure(.signatureMismatch, "\(signerInfos.count) signers in one signature block")
        }
        return (certificates, try DER.children(of: signerInfos[0], in: data), data)
    }

    static func algorithmOID(_ node: DERNode, in data: Data, name: String) throws -> String {
        guard let first = try DER.children(of: node, in: data).first else {
            throw SignatureFailure(.damaged, "\(name): an empty AlgorithmIdentifier")
        }
        return try DER.objectIdentifier(first, in: data)
    }

    static func secAlgorithm(encryption: String, digest: String) -> SecKeyAlgorithm? {
        let rsa: [String: SecKeyAlgorithm] = ["SHA1": .rsaSignatureMessagePKCS1v15SHA1, "SHA-256": .rsaSignatureMessagePKCS1v15SHA256,
                                              "SHA-384": .rsaSignatureMessagePKCS1v15SHA384, "SHA-512": .rsaSignatureMessagePKCS1v15SHA512]
        let ec: [String: SecKeyAlgorithm] = ["SHA1": .ecdsaSignatureMessageX962SHA1, "SHA-256": .ecdsaSignatureMessageX962SHA256,
                                             "SHA-384": .ecdsaSignatureMessageX962SHA384, "SHA-512": .ecdsaSignatureMessageX962SHA512]
        if rsaOIDs.contains(encryption) { return rsa[digest] }
        if ecOIDs.contains(encryption) { return ec[digest] }
        return nil
    }

    /// The messageDigest attribute among the signed attributes.
    static func messageDigest(in attributes: DERNode, data: Data, name: String) throws -> Data {
        for attribute in try DER.children(of: attributes, in: data) {
            let parts = try DER.children(of: attribute, in: data)
            guard parts.count == 2, try DER.objectIdentifier(parts[0], in: data) == messageDigestOID else { continue }
            guard let value = try DER.children(of: parts[1], in: data).first, value.tag == 0x04 else { break }
            return DER.content(of: value, in: data)
        }
        throw SignatureFailure(.signatureMismatch, "\(name) signs attributes without the signature file's digest")
    }
}
