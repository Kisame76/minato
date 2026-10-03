import CryptoKit
import Foundation

/// Signs test APKs for real, with v1 (JAR signing), v2, v3 and v3.1, as apksigner lays them out
/// (ADR-0034, step d). Written from the specifications, independently of APKKit's verifier.
public enum TestSigner {
    // Signature algorithm IDs of APK Signature Scheme v2 and v3.
    public static let rsaPSSSHA256: UInt32 = 0x0101
    public static let rsaPSSSHA512: UInt32 = 0x0102
    public static let rsaPKCS1SHA256: UInt32 = 0x0103
    public static let rsaPKCS1SHA512: UInt32 = 0x0104
    public static let ecdsaSHA256: UInt32 = 0x0201
    public static let ecdsaSHA512: UInt32 = 0x0202
    public static let dsaSHA256: UInt32 = 0x0301
    public static let rsaPKCS1SHA256Verity: UInt32 = 0x0421
    public static let ecdsaSHA256Verity: UInt32 = 0x0423
    public static let dsaSHA256Verity: UInt32 = 0x0425

    public static let v2ID: UInt32 = 0x7109_871A
    public static let v3ID: UInt32 = 0xF053_68C0
    public static let v31ID: UInt32 = 0x1B93_AD61
    /// v2 signed data's attribute that names the newer scheme the APK is also signed with.
    public static let strippingProtectionID: UInt32 = 0xBEEF_F00D
    /// v3 signed data's attribute that says a v3.1 block carries the rotated key.
    public static let rotationMinSdkID: UInt32 = 0x559F_8B02
    public static let proofOfRotationID: UInt32 = 0x3BA0_6F8C
    /// v3.1 signed data's attribute that says the signer is for the development preview of its minSdk.
    public static let rotationOnDevReleaseID: UInt32 = 0xC2A6_B3BA

    // Capabilities of a lineage level (SigningCertificateLineage).
    public static let installedData: UInt32 = 1
    public static let sharedUserID: UInt32 = 2
    public static let permission: UInt32 = 4
    public static let rollback: UInt32 = 8
    public static let auth: UInt32 = 16
    /// What apksigner grants an older key by default: everything but rollback.
    public static let defaultCapabilities: UInt32 = installedData | sharedUserID | permission | auth

    /// One level of a proof of rotation: a key, what it may still do, and the algorithm it signs the
    /// next level with.
    public struct Level {
        public var key: TestKey
        public var flags: UInt32
        public var algorithm: UInt32?
        /// Signs this level instead of the level before, for a broken lineage.
        public var signedBy: TestKey?
        /// The algorithm this level's signed data names, when it is not the level before's.
        public var signedAlgorithm: UInt32?

        public init(_ key: TestKey, flags: UInt32 = TestSigner.defaultCapabilities, algorithm: UInt32? = nil,
                    signedBy: TestKey? = nil, signedAlgorithm: UInt32? = nil) {
            self.key = key
            self.flags = flags
            self.algorithm = algorithm
            self.signedBy = signedBy
            self.signedAlgorithm = signedAlgorithm
        }
    }

    /// A v2, v3 or v3.1 signer.
    public struct Signer {
        public var key: TestKey
        public var algorithms: [UInt32]
        public var minSdk: UInt32
        public var maxSdk: UInt32
        /// Oldest first; the last level is `key`'s own.
        public var lineage: [Level]
        public var attributes: [(id: UInt32, value: Data)]
        /// The certificate the signer names, when it is not `key`'s: a signature made with another key.
        public var certificate: Data?
        /// More certificates the signer lists after its own, as a chain does.
        public var chain: [Data]
        /// The SubjectPublicKeyInfo the signer names, when it is not its certificate's bytes.
        public var publicKey: Data?
        /// Signed content digests of other bytes, when given: a digest that does not match.
        public var digestOf: Data?
        /// The algorithms the digests name, when they are not the signatures'.
        public var digestAlgorithms: [UInt32]?
        /// The Android versions in the signed data, when they are not the signer's (v3).
        public var signedSdk: (min: UInt32, max: UInt32)?

        public init(_ key: TestKey, algorithms: [UInt32]? = nil, minSdk: UInt32 = 24, maxSdk: UInt32 = 0x7FFF_FFFF,
                    lineage: [Level] = [], attributes: [(id: UInt32, value: Data)] = [], certificate: Data? = nil,
                    publicKey: Data? = nil, digestOf: Data? = nil, digestAlgorithms: [UInt32]? = nil,
                    signedSdk: (min: UInt32, max: UInt32)? = nil, chain: [Data] = []) {
            self.certificate = certificate
            self.chain = chain
            self.publicKey = publicKey
            self.digestOf = digestOf
            self.digestAlgorithms = digestAlgorithms
            self.signedSdk = signedSdk
            self.key = key
            self.algorithms = algorithms ?? [key.defaultAlgorithm]
            self.minSdk = minSdk
            self.maxSdk = maxSdk
            self.lineage = lineage
            self.attributes = attributes
        }
    }

    /// JAR signing (v1).
    public struct V1 {
        public var keys: [TestKey]
        /// "SHA1", "SHA-256", "SHA-384" or "SHA-512", as the manifest names them.
        public var digest: String
        /// PKCS#7 signed attributes, as jarsigner writes them; apksigner writes none.
        public var signedAttributes: Bool
        /// The .SF's X-Android-APK-Signed header, e.g. "2, 3"; nil for none.
        public var androidSigned: String?
        /// The .SF's digest of the whole manifest; without it Android checks each section.
        public var wholeManifestDigest: Bool
        /// Names the signer's issuer with PrintableStrings where its certificate has UTF8Strings: the
        /// same name by value, other bytes.
        public var reencodedIssuer = false
        /// Certificates the signature block carries before the signer's own, which sign nothing.
        public var certificatesBefore: [Data] = []

        public init(_ keys: [TestKey], digest: String = "SHA-256", signedAttributes: Bool = false,
                    androidSigned: String? = nil, wholeManifestDigest: Bool = true) {
            self.keys = keys
            self.digest = digest
            self.signedAttributes = signedAttributes
            self.androidSigned = androidSigned
            self.wholeManifestDigest = wholeManifestDigest
        }
    }

    /// The signed APK: `zip` with v1's files, then a signing block with the signers of each scheme and
    /// the `extra` pairs; without any signer, only v1.
    public static func sign(_ zip: ZipBuilder, v1: V1? = nil, v2: [Signer] = [], v3: [Signer] = [], v31: [Signer] = [],
                            extra: [(id: UInt32, value: Data)] = []) -> Data {
        var zip = zip
        if let v1 { zip.entries += jarSignatureFiles(for: zip.entries, v1) }
        zip.beforeCentralDirectory = Data()
        let unsigned = zip.build()
        var pairs: [(id: UInt32, value: Data)] = []
        for (id, signers) in [(v2ID, v2), (v3ID, v3), (v31ID, v31)] where !signers.isEmpty {
            let encoded = signers.map { schemeSigner($0, isV3: id != v2ID, apk: unsigned) }
            pairs.append((id, SigningBlockBuilder.schemeValue(encoded)))
        }
        pairs += extra
        guard !pairs.isEmpty else { return unsigned.data }
        zip.beforeCentralDirectory = SigningBlockBuilder.block(pairs)
        return zip.build().data
    }

    // MARK: - v2, v3, v3.1

    static func schemeSigner(_ signer: Signer, isV3: Bool, apk: ZipBuilder.Output) -> Data {
        let digests = zip(signer.algorithms, signer.digestAlgorithms ?? signer.algorithms).map { algorithm, named in
            let digest = signer.digestOf.map { TestSigner.digest($0, "SHA-256") } ?? contentDigest(apk, algorithm: algorithm)
            return prefixed(u32(named) + prefixed(digest))
        }.reduce(Data(), +)
        let certificate = signer.certificate ?? signer.key.certificate
        var attributes = signer.attributes
        if !signer.lineage.isEmpty { attributes.append((proofOfRotationID, proofOfRotation(signer.lineage))) }
        let encodedAttributes = attributes.map { prefixed(u32($0.id) + $0.value) }.reduce(Data(), +)
        var signedData = prefixed(digests) + prefixed(([certificate] + signer.chain).map(prefixed).reduce(Data(), +))
        if isV3 { signedData += u32(signer.signedSdk?.min ?? signer.minSdk) + u32(signer.signedSdk?.max ?? signer.maxSdk) }
        signedData += prefixed(encodedAttributes)

        let signatures = signer.algorithms.map { algorithm in
            prefixed(u32(algorithm) + prefixed(signer.key.sign(signedData, algorithm: algorithm)))
        }.reduce(Data(), +)
        var encoded = prefixed(signedData)
        if isV3 { encoded += u32(signer.minSdk) + u32(signer.maxSdk) }
        return encoded + prefixed(signatures) + prefixed(signer.publicKey ?? DERWriter.subjectPublicKeyInfo(of: certificate))
    }

    /// The proof of rotation: a version, then each level with its certificate and the algorithm the
    /// level before signed it with, its flags, the algorithm it signs the next level with, and the
    /// level before's signature.
    public static func proofOfRotation(_ levels: [Level]) -> Data {
        var value = u32(1)
        var previous: (key: TestKey, algorithm: UInt32)?
        for level in levels {
            let algorithm = level.algorithm ?? level.key.defaultAlgorithm
            let signedData = prefixed(level.key.certificate) + u32(level.signedAlgorithm ?? previous?.algorithm ?? 0)
            let signature = previous.map { (level.signedBy ?? $0.key).sign(signedData, algorithm: $0.algorithm) } ?? Data()
            value += prefixed(prefixed(signedData) + u32(level.flags) + u32(algorithm) + prefixed(signature))
            previous = (level.key, algorithm)
        }
        return value
    }

    /// The content digest of `apk` (unsigned, the signing block not yet in it) for `algorithm`: the
    /// entries, the central directory and the end record in chunks of 1 MiB. Verity algorithms get
    /// random bytes: nothing here computes their Merkle tree.
    public static func contentDigest(_ apk: ZipBuilder.Output, algorithm: UInt32) -> Data {
        let data = apk.data
        let endRecord = data.count - 22 - Int(UInt16(data[data.count - 2]) | UInt16(data[data.count - 1]) << 8)
        let sections = [data.subdata(in: 0..<apk.centralDirectoryOffset),
                        data.subdata(in: apk.centralDirectoryOffset..<endRecord),
                        data.subdata(in: endRecord..<data.count)]
        switch algorithm {
        case rsaPSSSHA256, rsaPKCS1SHA256, ecdsaSHA256, dsaSHA256:
            return chunked(sections, SHA256.self)
        case rsaPSSSHA512, rsaPKCS1SHA512, ecdsaSHA512:
            return chunked(sections, SHA512.self)
        default:
            return Data((0..<40).map { _ in UInt8.random(in: 0...255) })
        }
    }

    static func chunked<H: HashFunction>(_ sections: [Data], _: H.Type) -> Data {
        var digests = Data()
        var count: UInt32 = 0
        for section in sections {
            var start = 0
            while start < section.count {
                let end = min(start + (1 << 20), section.count)
                var hasher = H()
                hasher.update(data: Data([0xA5]) + u32(UInt32(end - start)))
                hasher.update(data: section.subdata(in: start..<end))
                digests += Data(hasher.finalize())
                count += 1
                start = end
            }
        }
        var hasher = H()
        hasher.update(data: Data([0x5A]) + u32(count))
        hasher.update(data: digests)
        return Data(hasher.finalize())
    }

    // MARK: - v1

    /// META-INF/MANIFEST.MF, and for each key its .SF and its signature block, for these entries.
    public static func jarSignatureFiles(for entries: [ZipBuilder.Entry], _ v1: V1) -> [ZipBuilder.Entry] {
        let main = "Manifest-Version: 1.0\r\nCreated-By: Minato tests\r\n\r\n"
        var sections: [(name: String, text: String)] = []
        for entry in entries where !entry.name.hasPrefix("META-INF/") && !entry.name.hasSuffix("/") {
            let text = header("Name", entry.name) + header("\(v1.digest)-Digest", digest(entry.content, v1.digest).base64EncodedString()) + "\r\n"
            sections.append((entry.name, text))
        }
        let manifest = Data((main + sections.map(\.text).joined()).utf8)
        var files = [ZipBuilder.Entry(name: "META-INF/MANIFEST.MF", content: manifest)]

        var signatureFile = "Signature-Version: 1.0\r\nCreated-By: 1.0 (Android)\r\n"
        if v1.wholeManifestDigest {
            signatureFile += header("\(v1.digest)-Digest-Manifest", digest(manifest, v1.digest).base64EncodedString())
        }
        signatureFile += header("\(v1.digest)-Digest-Manifest-Main-Attributes", digest(Data(main.utf8), v1.digest).base64EncodedString())
        if let androidSigned = v1.androidSigned { signatureFile += header("X-Android-APK-Signed", androidSigned) }
        signatureFile += "\r\n"
        for section in sections {
            signatureFile += header("Name", section.name)
                + header("\(v1.digest)-Digest", digest(Data(section.text.utf8), v1.digest).base64EncodedString()) + "\r\n"
        }
        let sf = Data(signatureFile.utf8)
        for (index, key) in v1.keys.enumerated() {
            let name = "META-INF/SIGNER\(index)"
            let suffix = switch key.kind {
            case .rsa: "RSA"
            case .ec: "EC"
            case .dsa: "DSA"
            }
            files.append(ZipBuilder.Entry(name: "\(name).SF", content: sf))
            files.append(ZipBuilder.Entry(name: "\(name).\(suffix)", content: pkcs7(signing: sf, with: key, v1)))
        }
        return files
    }

    /// A manifest header, wrapped at 72 bytes with continuation lines that start with a space.
    static func header(_ name: String, _ value: String) -> String {
        let bytes = Array("\(name): \(value)".utf8)
        var lines = [Data(bytes.prefix(72))]
        var rest = bytes.dropFirst(72)
        while !rest.isEmpty {
            lines.append(Data([0x20] + rest.prefix(71)))
            rest = rest.dropFirst(71)
        }
        return lines.map { String(decoding: $0, as: UTF8.self) }.joined(separator: "\r\n") + "\r\n"
    }

    static let digestOIDs = ["SHA1": "1.3.14.3.2.26", "SHA-256": "2.16.840.1.101.3.4.2.1",
                             "SHA-384": "2.16.840.1.101.3.4.2.2", "SHA-512": "2.16.840.1.101.3.4.2.3"]

    public static func digest(_ data: Data, _ name: String) -> Data {
        switch name {
        case "SHA1": Data(Insecure.SHA1.hash(data: data))
        case "SHA-384": Data(SHA384.hash(data: data))
        case "SHA-512": Data(SHA512.hash(data: data))
        default: Data(SHA256.hash(data: data))
        }
    }

    /// PKCS#7 SignedData over the .SF, detached, with the key's certificate and one SignerInfo.
    public static func pkcs7(signing sf: Data, with key: TestKey, _ v1: V1) -> Data {
        let digestAlgorithm = DERWriter.algorithm(digestOIDs[v1.digest]!)
        let encryption = switch key.kind {
        case .rsa: DERWriter.algorithm("1.2.840.113549.1.1.1")
        case .ec: DERWriter.algorithm("1.2.840.10045.2.1", parameters: false)
        case .dsa: DERWriter.algorithm("1.2.840.10040.4.1", parameters: false)
        }
        var issuerAndSerial = DERWriter.issuerAndSerialNumber(of: key.certificate)
        if v1.reencodedIssuer {
            let parts = DERWriter.children(of: issuerAndSerial)
            let issuer = DERWriter.sequence(DERWriter.children(of: parts[0]).map { set in
                DERWriter.set(DERWriter.children(of: set).map { attribute in
                    DERWriter.sequence(DERWriter.children(of: attribute).map { $0.first == 0x0C ? Data([0x13]) + $0.dropFirst() : $0 })
                })
            })
            issuerAndSerial = DERWriter.sequence([issuer, parts[1]])
        }
        var signerInfo = [DERWriter.integer(1), issuerAndSerial, digestAlgorithm]
        var signed = sf
        if v1.signedAttributes {
            let attributes = [
                DERWriter.sequence([DERWriter.oid("1.2.840.113549.1.9.3"), DERWriter.set([DERWriter.oid("1.2.840.113549.1.7.1")])]),
                DERWriter.sequence([DERWriter.oid("1.2.840.113549.1.9.4"), DERWriter.set([DERWriter.octetString(digest(sf, v1.digest))])]),
            ].reduce(Data(), +)
            signed = DERWriter.tagged(0x31, attributes)
            signerInfo.append(DERWriter.tagged(0xA0, attributes))
        }
        signerInfo += [encryption, DERWriter.octetString(key.sign(signed, digest: v1.digest))]
        let signedData = DERWriter.sequence([
            DERWriter.integer(1),
            DERWriter.set([digestAlgorithm]),
            DERWriter.sequence([DERWriter.oid("1.2.840.113549.1.7.1")]),
            DERWriter.tagged(0xA0, v1.certificatesBefore.reduce(Data(), +) + key.certificate),
            DERWriter.set([DERWriter.sequence(signerInfo)]),
        ])
        return DERWriter.sequence([DERWriter.oid("1.2.840.113549.1.7.2"), DERWriter.tagged(0xA0, signedData)])
    }

    public static func prefixed(_ data: Data) -> Data { u32(UInt32(data.count)) + data }

    public static func u32(_ value: UInt32) -> Data { withUnsafeBytes(of: value.littleEndian) { Data($0) } }
}
