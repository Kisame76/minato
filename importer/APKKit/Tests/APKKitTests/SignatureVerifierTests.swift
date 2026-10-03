import APKTestSupport
import Foundation
import Testing

@testable import APKKit

/// ADR-0034, test 5: the signature of each scheme that decision 2 uses, verified for real.
@Suite("Signature verification")
struct SignatureVerifierTests {
    static let longName = "res/raw/" + String(repeating: "a-name-longer-than-one-manifest-line-", count: 3) + ".bin"

    static func zip() -> ZipBuilder {
        var zip = ZipBuilder()
        zip.add("AndroidManifest.xml", Data(repeating: 0x03, count: 700))
        zip.add("classes.dex", Data((0..<50_000).map { UInt8(truncatingIfNeeded: $0 &* 7) }))
        zip.add("lib/arm64-v8a/libgame.so", Data(repeating: 0x11, count: 20_000), method: .stored)
        zip.add(longName, Data("wrapped".utf8))
        return zip
    }

    /// What a check found and the signer it proved; the proven certificates themselves are `proof`'s.
    struct Outcome: Equatable {
        var check: SignatureCheck
        var signer: SignerIdentity?
    }

    /// The check of an APK whose manifest targets Android 10 (SDK 29), the last that takes v1 alone.
    static func verify(_ data: Data, targetSdk: Int = 29) throws -> Outcome {
        let verification = SignatureVerifier.verify(try ZipArchive(data: data), targetSdk: targetSdk)
        return Outcome(check: verification.check, signer: verification.signer)
    }

    static func proof(_ data: Data, targetSdk: Int = 29) throws -> SignerProof? {
        SignatureVerifier.verify(try ZipArchive(data: data), targetSdk: targetSdk).proof
    }

    static func failure(_ data: Data, targetSdk: Int = 29) throws -> SignatureFailure? {
        if case .failed(let failure) = try verify(data, targetSdk: targetSdk).check { return failure }
        return nil
    }

    static func verified(_ current: [TestKey], lineage: [LineageNode] = []) -> Outcome {
        Outcome(check: .verified, signer: SignerIdentity(current: current.map(\.sha256), lineage: lineage))
    }

    /// `data` with the byte at `offset` changed.
    static func flipped(_ data: Data, at offset: Int) -> Data {
        var copy = data
        copy[offset] ^= 0x01
        return copy
    }

    static func dataOffset(of name: String, in data: Data) throws -> Int {
        let archive = try ZipArchive(data: data)
        return try Int(archive.dataOffset(of: #require(archive.entry(named: name))))
    }

    // MARK: - Valid signatures pass

    static let schemeKeys: [(TestKey, UInt32)] = [
        (.rsa, TestSigner.rsaPKCS1SHA256), (.rsa, TestSigner.rsaPKCS1SHA512), (.rsa, TestSigner.rsaPSSSHA256),
        (.rsa, TestSigner.rsaPSSSHA512), (.ec, TestSigner.ecdsaSHA256), (.ec, TestSigner.ecdsaSHA512),
    ]

    @Test(arguments: schemeKeys)
    func v2(key: TestKey, algorithm: UInt32) throws {
        #expect(try Self.verify(TestSigner.sign(Self.zip(), v2: [.init(key, algorithms: [algorithm])])) == Self.verified([key]))
    }

    @Test(arguments: schemeKeys)
    func v3(key: TestKey, algorithm: UInt32) throws {
        let signer = TestSigner.Signer(key, algorithms: [algorithm])
        #expect(try Self.verify(TestSigner.sign(Self.zip(), v2: [signer], v3: [signer])) == Self.verified([key]))
    }

    @Test func v31WithTheRotatedKey() throws {
        // v3 keeps the old key for Android 12L and older and says that v3.1 carries the rotated one.
        let old = TestSigner.Signer(.ec, maxSdk: 32, attributes: [(TestSigner.rotationMinSdkID, TestSigner.u32(33))])
        let new = TestSigner.Signer(.rotated, minSdk: 33, lineage: [.init(.ec), .init(.rotated)])
        let apk = TestSigner.sign(Self.zip(), v2: [.init(.ec)], v3: [old], v31: [new])
        #expect(try Self.verify(apk) == Self.verified(
            [.rotated], lineage: [LineageNode(sha256: TestKey.ec.sha256, flags: TestSigner.defaultCapabilities)]))
    }

    @Test(arguments: [TestKey.rsa, .ec], ["SHA-256", "SHA1"])
    func v1(key: TestKey, digest: String) throws {
        for signedAttributes in [false, true] {
            let v1 = TestSigner.V1([key], digest: digest, signedAttributes: signedAttributes)
            #expect(try Self.verify(TestSigner.sign(Self.zip(), v1: v1)) == Self.verified([key]))
        }
    }

    @Test func v1BySectionWithoutTheWholeManifestsDigest() throws {
        let apk = TestSigner.sign(Self.zip(), v1: TestSigner.V1([.ec], wholeManifestDigest: false))
        #expect(try Self.verify(apk) == Self.verified([.ec]))
    }

    @Test func v1WithSeveralSigners() throws {
        #expect(try Self.verify(TestSigner.sign(Self.zip(), v1: TestSigner.V1([.rsa, .ec]))) == Self.verified([.rsa, .ec]))
    }

    @Test func v1IsIgnoredWhenV2IsThere() throws {
        // Android verifies only the newest scheme: a v1 signature it cannot check does not count.
        let apk = TestSigner.sign(Self.zip(), v1: TestSigner.V1([.dsa], androidSigned: "2"), v2: [.init(.ec)])
        #expect(try Self.verify(apk) == Self.verified([.ec]))
    }

    @Test func aVerityAlgorithmNextToAPlainOne() throws {
        let apk = TestSigner.sign(Self.zip(), v2: [.init(.ec, algorithms: [TestSigner.ecdsaSHA256, TestSigner.ecdsaSHA256Verity])])
        #expect(try Self.verify(apk) == Self.verified([.ec]))
    }

    @Test func v2WithSeveralSigners() throws {
        #expect(try Self.verify(TestSigner.sign(Self.zip(), v2: [.init(.rsa), .init(.ec)])) == Self.verified([.rsa, .ec]))
    }

    // MARK: - Unsigned and unverified

    @Test func anUnsignedFile() throws {
        #expect(try Self.verify(Self.zip().build().data) == Outcome(check: .unsigned))
    }

    @Test func algorithmsSecurityCannotCheckLeaveTheFileUnverified() throws {
        for apk in [
            TestSigner.sign(Self.zip(), v1: TestSigner.V1([.dsa])),
            TestSigner.sign(Self.zip(), v2: [.init(.dsa)]),
            TestSigner.sign(Self.zip(), v2: [.init(.ec, algorithms: [TestSigner.ecdsaSHA256Verity])]),
        ] {
            let check = try Self.verify(apk).check
            guard case .unverified = check else {
                Issue.record("got \(check)")
                continue
            }
        }
    }

    // MARK: - Damaged and forged files are refused

    @Test func aChangedByteInAnEntry() throws {
        for apk in [TestSigner.sign(Self.zip(), v2: [.init(.ec)]), TestSigner.sign(Self.zip(), v1: TestSigner.V1([.ec]))] {
            let offset = try Self.dataOffset(of: "lib/arm64-v8a/libgame.so", in: apk) + 100
            #expect(try Self.failure(Self.flipped(apk, at: offset))?.kind == .damaged)
        }
    }

    @Test func aChangedByteInTheCentralDirectory() throws {
        let v2 = TestSigner.sign(Self.zip(), v2: [.init(.ec)])
        // The version that made the first entry: nothing else reads it.
        let centralDirectory = Int(try ZipArchive(data: v2).centralDirectoryOffset)
        #expect(try Self.failure(Self.flipped(v2, at: centralDirectory + 4))?.kind == .damaged)

        // v1 covers the names: a renamed entry is not signed.
        let v1 = TestSigner.sign(Self.zip(), v1: TestSigner.V1([.ec]))
        let name = try #require(v1.range(of: Data("classes.dex".utf8), options: .backwards))
        #expect(try Self.failure(Self.flipped(v1, at: name.lowerBound)) != nil)
    }

    @Test func anyChangedByteInTheSigningBlock() throws {
        let apk = TestSigner.sign(Self.zip(), v2: [.init(.ec)])
        let end = Int(try ZipArchive(data: apk).centralDirectoryOffset)
        let unsigned = Self.zip().build()
        let start = unsigned.centralDirectoryOffset
        // Leaves out the block's size fields, its magic and the v2 pair's ID: without them there is no
        // v2 block, and the file counts as unsigned, as on Android.
        for offset in (start + 20)..<(end - 24) {
            let check = try Self.verify(Self.flipped(apk, at: offset)).check
            guard case .failed = check else {
                Issue.record("byte \(offset - start) of the block: \(check)")
                continue
            }
        }
    }

    @Test func aSignatureMadeWithAnotherKey() throws {
        let apk = TestSigner.sign(Self.zip(), v2: [.init(.rotated, certificate: TestKey.ec.certificate)])
        #expect(try Self.failure(apk)?.kind == .signatureMismatch)
    }

    @Test func aDigestThatDoesNotMatch() throws {
        let apk = TestSigner.sign(Self.zip(), v2: [.init(.ec, digestOf: Data("other bytes".utf8))])
        #expect(try Self.failure(apk)?.kind == .damaged)
    }

    @Test func aChangedSignatureFile() throws {
        var zip = Self.zip()
        var files = TestSigner.jarSignatureFiles(for: zip.entries, TestSigner.V1([.ec]))
        let index = try #require(files.firstIndex { $0.name.hasSuffix(".SF") })
        files[index].content.append(contentsOf: Data("Name: classes.dex\r\nSHA-256-Digest: AAAA\r\n\r\n".utf8))
        zip.entries += files
        #expect(try Self.failure(zip.build().data)?.kind == .signatureMismatch)
    }

    @Test func aSchemeBlockTwiceOrWithoutASigner() throws {
        // Both blocks' certificates would count for the signer, and Android reads only the first.
        let twice = TestSigner.sign(Self.zip(), v2: [.init(.ec)], extra: [(TestSigner.v2ID, SigningBlockBuilder.schemeValue([]))])
        #expect(try Self.failure(twice)?.kind == .damaged)
        let empty = TestSigner.sign(Self.zip(), extra: [(TestSigner.v2ID, SigningBlockBuilder.schemeValue([]))])
        #expect(try Self.failure(empty)?.kind == .signatureMismatch)
    }

    @Test func v3SignersForAndroid15() throws {
        // One at most, as on Android.
        let two = TestSigner.sign(Self.zip(), v2: [.init(.ec)], v3: [.init(.ec), .init(.rotated)])
        #expect(try Self.failure(two)?.kind == .signatureMismatch)
        // None for Android 15 fails, as on Android: otherwise changing the signer's unsigned Android
        // versions would switch v3 off and let v2's older key decide.
        let older = TestSigner.sign(Self.zip(), v2: [.init(.ec)], v3: [.init(.rotated, maxSdk: 30)])
        #expect(try Self.failure(older)?.kind == .signatureMismatch)
        var edited = TestSigner.sign(Self.zip(), v2: [.init(.ec)], v3: [.init(.rotated)])
        let outer = try #require(try Self.outerSdkOffset(in: edited))
        edited.replaceSubrange(outer..<outer + 4, with: TestSigner.u32(36))
        #expect(try Self.failure(edited) != nil)
    }

    /// Where the v3 signer's unsigned minSdk stands: after its length-prefixed signed data.
    static func outerSdkOffset(in apk: Data) throws -> Int? {
        let archive = try ZipArchive(data: apk)
        let pairs = try APKSignatures.signingBlockPairs(archive)
        guard let v3 = pairs.first(where: { $0.0 == TestSigner.v3ID })?.1,
              let start = apk.range(of: v3)?.lowerBound else { return nil }
        // value: signers length, signer length, signed data length, signed data, then minSdk.
        let signedDataLength = Int(v3[8]) | Int(v3[9]) << 8 | Int(v3[10]) << 16 | Int(v3[11]) << 24
        return start + 12 + signedDataLength
    }

    @Test func v31SkippedWhileV3SaysItHasARotatedKey() throws {
        let v3 = TestSigner.Signer(.ec, maxSdk: 32, attributes: [(TestSigner.rotationMinSdkID, TestSigner.u32(33))])
        // A v3.1 signer only for Android 16 on: v3 decides for Android 15, but says v3.1 should have.
        let skipped = TestSigner.sign(Self.zip(), v2: [.init(.ec)], v3: [TestSigner.Signer(.ec, attributes: v3.attributes)],
                                      v31: [.init(.rotated, minSdk: 36, lineage: [.init(.ec), .init(.rotated)])])
        #expect(try Self.failure(skipped)?.kind == .signatureMismatch)
        // Rotated only from a later Android on: v3 decides and is right to, when v3.1 is there for what it names.
        let later = TestSigner.sign(Self.zip(), v2: [.init(.ec)], v3: [.init(.ec, attributes: [(TestSigner.rotationMinSdkID, TestSigner.u32(36))])],
                                    v31: [.init(.rotated, minSdk: 36, lineage: [.init(.ec), .init(.rotated)])])
        #expect(try Self.verify(later) == Self.verified([.ec]))
    }

    @Test func v3NamesTheV31SignatureItLeftOut() throws {
        // As ApkSignatureSchemeV3Verifier.java:479-500: the attribute needs v3.1's block, whatever version it names,
        // and the lowest minSdk among v3.1's signers for other Android versions.
        for named: UInt32 in [33, 35, 36] {
            let stripped = TestSigner.sign(Self.zip(), v2: [.init(.ec)],
                                           v3: [.init(.ec, attributes: [(TestSigner.rotationMinSdkID, TestSigner.u32(named))])])
            #expect(try Self.failure(stripped)?.kind == .signatureMismatch, "SDK \(named)")
        }
        // v3.1 only for Android 13 and 14: v3 decides for 15, naming 33.
        let earlier = TestSigner.sign(Self.zip(), v2: [.init(.ec)],
                                      v3: [.init(.ec, attributes: [(TestSigner.rotationMinSdkID, TestSigner.u32(33))])],
                                      v31: [.init(.rotated, minSdk: 33, maxSdk: 34, lineage: [.init(.ec), .init(.rotated)])])
        #expect(try Self.verify(earlier) == Self.verified([.ec]))
        let lowest = TestSigner.sign(Self.zip(), v2: [.init(.ec)],
                                     v3: [.init(.ec, attributes: [(TestSigner.rotationMinSdkID, TestSigner.u32(37))])],
                                     v31: [.init(.rotated, minSdk: 37, lineage: [.init(.ec), .init(.rotated)]),
                                           .init(.rotated, minSdk: 36, lineage: [.init(.ec), .init(.rotated)])])
        #expect(try Self.failure(lowest)?.kind == .signatureMismatch)
    }

    @Test func aV31SignerForTheDevelopmentPreviewDoesNotCount() throws {
        // Android 15 is a release: a v3.1 signer for SDK 35's preview is skipped and v3 decides
        // (ApkSignatureSchemeV3Verifier.java:502-519).
        let preview = TestSigner.Signer(.rotated, minSdk: 35, lineage: [.init(.ec), .init(.rotated)],
                                        attributes: [(TestSigner.rotationOnDevReleaseID, Data())])
        let apk = TestSigner.sign(Self.zip(), v2: [.init(.ec)],
                                  v3: [.init(.ec, attributes: [(TestSigner.rotationMinSdkID, TestSigner.u32(35))])], v31: [preview])
        #expect(try Self.verify(apk) == Self.verified([.ec]))
        #expect(try Self.proof(apk)?.scheme == .v3)
        // For the preview of another SDK version, or in a v3 block, the attribute changes nothing.
        var earlier = preview
        earlier.minSdk = 33
        let counted = TestSigner.sign(Self.zip(), v2: [.init(.ec)],
                                      v3: [.init(.ec, maxSdk: 32, attributes: [(TestSigner.rotationMinSdkID, TestSigner.u32(33))])], v31: [earlier])
        #expect(try Self.verify(counted) == Self.verified([.rotated], lineage: [LineageNode(sha256: TestKey.ec.sha256, flags: TestSigner.defaultCapabilities)]))
        let inV3 = TestSigner.sign(Self.zip(), v2: [.init(.ec)], v3: [.init(.ec, attributes: [(TestSigner.rotationOnDevReleaseID, Data())])])
        #expect(try Self.verify(inV3) == Self.verified([.ec]))
        // Skipped only after its signature is checked, as on Android.
        var forged = preview
        forged.certificate = TestKey.rsa.certificate
        forged.publicKey = DERWriter.subjectPublicKeyInfo(of: TestKey.rsa.certificate)
        let broken = TestSigner.sign(Self.zip(), v2: [.init(.ec)],
                                     v3: [.init(.ec, attributes: [(TestSigner.rotationMinSdkID, TestSigner.u32(35))])], v31: [forged])
        #expect(try Self.failure(broken)?.kind == .signatureMismatch)
    }

    @Test func v3ReadsItsAndroidVersionsAsInts() throws {
        // ApkSignatureSchemeV3Verifier.java:274-276: maxSdk 0xFFFFFFFF is -1, below Android 15; minSdk 0x80000000 is
        // the lowest int.
        let none = TestSigner.sign(Self.zip(), v2: [.init(.ec)], v3: [.init(.rotated, maxSdk: 0xFFFF_FFFF)])
        #expect(try Self.failure(none)?.kind == .signatureMismatch)
        let all = TestSigner.sign(Self.zip(), v2: [.init(.ec)], v3: [.init(.rotated, minSdk: 0x8000_0000)])
        #expect(try Self.verify(all) == Self.verified([.rotated]))
        // A v3.1 signer for no version falls back to v3.
        let fallback = TestSigner.sign(Self.zip(), v2: [.init(.ec)], v3: [.init(.ec)],
                                       v31: [.init(.rotated, maxSdk: 0xFFFF_FFFF, lineage: [.init(.ec), .init(.rotated)])])
        #expect(try Self.verify(fallback) == Self.verified([.ec]))
    }

    @Test func theSameCertificateTwiceIsOneSigner() throws {
        #expect(try Self.verify(TestSigner.sign(Self.zip(), v2: [.init(.ec), .init(.ec)])) == Self.verified([.ec]))
    }

    @Test func theManifestAsAndroidReadsIt() throws {
        let text = "Manifest-Version: 1.0\r\n\r\n\r\nName: x\r\nSHA-256-Digest: aa\r\n\r\n\n\r\nName: y\nSHA-256-Digest: b\r\n b\r\n\r\nName: z\r\nLast: dropped"
        let manifest = try JarManifest(Data(text.utf8), name: "MANIFEST.MF")
        let x = try #require(manifest.sections["x"])
        let y = try #require(manifest.sections["y"])
        // The blank lines after a section belong to it, and after the main section to that.
        #expect(manifest.mainEnd == "Manifest-Version: 1.0\r\n\r\n\r\n".utf8.count)
        #expect(x.range == 27..<(27 + "Name: x\r\nSHA-256-Digest: aa\r\n\r\n\n\r\n".utf8.count))
        #expect(y.range.lowerBound == x.range.upperBound)
        // Continuation lines.
        #expect(y.attributes["sha-256-digest"] == "bb")
        // A last line without its end does not count.
        #expect(manifest.sections["z"]?.attributes["last"] == nil)
        #expect(manifest.main == ["manifest-version": "1.0"])
    }

    @Test func v1NamesItsSignerByValue() throws {
        var v1 = TestSigner.V1([.ec])
        v1.reencodedIssuer = true
        #expect(try Self.verify(TestSigner.sign(Self.zip(), v1: v1)) == Self.verified([.ec]))
    }

    @Test func aKeyOnACurveSecurityLacksLeavesTheFileUnverified() throws {
        // An EC key named as brainpoolP256r1, with a point Security.framework would take for P-256.
        let spki = DERWriter.subjectPublicKeyInfo(of: TestKey.ec.certificate)
        let parts = DERWriter.children(of: spki)
        let algorithm = DERWriter.sequence([DERWriter.oid("1.2.840.10045.2.1"), DERWriter.oid("1.3.36.3.3.2.8.1.1.7")])
        #expect(throws: Unverifiable.self) { try SignatureVerifier.publicKey(subjectPublicKeyInfo: DERWriter.sequence([algorithm, parts[1]])) }
        #expect(throws: Never.self) { try SignatureVerifier.publicKey(subjectPublicKeyInfo: spki) }
    }

    @Test func theSignersKeyMayBeEncodedOtherwiseThanItsCertificates() throws {
        // The certificate writes its RSA modulus as a negative INTEGER, as old Java runtimes did; Android
        // compares the keys, not their bytes.
        let certificate = Self.certificate(TestKey.rsa.certificate, withKey: Self.negativeModulus(DERWriter.subjectPublicKeyInfo(of: TestKey.rsa.certificate)))
        let apk = TestSigner.sign(Self.zip(), v2: [.init(.rsa, certificate: certificate,
                                                         publicKey: DERWriter.subjectPublicKeyInfo(of: TestKey.rsa.certificate))])
        let result = try Self.verify(apk)
        #expect(result == Outcome(check: .verified, signer: SignerIdentity(current: [SigningCertificate(der: certificate).sha256])))
    }

    @Test func aKeySecurityCannotReadLeavesTheFileUnverified() throws {
        // A P-256 key whose point is too short: nothing Security.framework can check, so not proven either way.
        let spki = DERWriter.subjectPublicKeyInfo(of: TestKey.ec.certificate)
        let parts = DERWriter.children(of: spki)
        let short = DERWriter.sequence([parts[0], DERWriter.tagged(0x03, Data([0, 0x04]) + Data(repeating: 0x11, count: 10))])
        let certificate = Self.certificate(TestKey.ec.certificate, withKey: short)
        let apk = TestSigner.sign(Self.zip(), v2: [.init(.ec, certificate: certificate)])
        let check = try Self.verify(apk).check
        guard case .unverified = check else { Issue.record("got \(check)"); return }
    }

    /// `certificate` with another SubjectPublicKeyInfo; its own signature no longer matches, which Android
    /// does not check either.
    static func certificate(_ certificate: Data, withKey spki: Data) -> Data {
        let parts = DERWriter.children(of: certificate)
        var tbs = DERWriter.children(of: parts[0])
        tbs[tbs.firstIndex(of: DERWriter.subjectPublicKeyInfo(of: certificate))!] = spki
        return DERWriter.sequence([DERWriter.sequence(tbs), parts[1], parts[2]])
    }

    /// An RSA SubjectPublicKeyInfo with the modulus's leading zero byte left out.
    static func negativeModulus(_ spki: Data) -> Data {
        let parts = DERWriter.children(of: spki)
        let key = DERWriter.content(of: parts[1]).dropFirst()
        let numbers = DERWriter.children(of: key)
        let modulus = DERWriter.content(of: numbers[0])
        precondition(modulus.first == 0 && modulus[modulus.startIndex + 1] & 0x80 != 0)
        let encoded = DERWriter.sequence([DERWriter.tagged(0x02, modulus.dropFirst()), numbers[1]])
        return DERWriter.sequence([parts[0], DERWriter.tagged(0x03, Data([0]) + encoded)])
    }

    @Test func tooManyV2Signers() throws {
        // As Android: no more than 10.
        let apk = TestSigner.sign(Self.zip(), v2: Array(repeating: TestSigner.Signer(.ec), count: 11))
        #expect(try Self.failure(apk)?.kind == .signatureMismatch)
    }

    @Test func aSignerWithThousandsOfCertificates() throws {
        // Real chains hold one to three; each one here would be read, and the report would parse them all.
        let certificates = (0..<5_000).map { TestSigner.prefixed(TestSigner.u32(UInt32($0))) }.reduce(Data(), +)
        let signedData = TestSigner.prefixed(Data()) + TestSigner.prefixed(certificates) + TestSigner.prefixed(Data())
        let signer = TestSigner.prefixed(signedData) + TestSigner.prefixed(Data()) + TestSigner.prefixed(Data())
        let apk = TestSigner.sign(Self.zip(), extra: [(TestSigner.v2ID, SigningBlockBuilder.schemeValue([signer]))])
        #expect(try Self.failure(apk)?.kind == .damaged)
        #expect(APKSignatures.read(try ZipArchive(data: apk)).certificates.count <= APKSignatures.maximumListLength)
    }

    @Test func noListBeyondItsLimit() throws {
        let limit = APKSignatures.maximumListLength
        // Pairs in the signing block.
        let pairs = TestSigner.sign(Self.zip(), v2: [.init(.ec)], extra: Array(repeating: (0x4272_6577, Data()), count: limit))
        #expect(try Self.failure(pairs)?.kind == .damaged)
        // A signer's attributes.
        let attributes = TestSigner.sign(Self.zip(), v2: [.init(.ec, attributes: Array(repeating: (0x1234, Data()), count: limit + 1))])
        #expect(try Self.failure(attributes)?.kind == .damaged)
        // Certificates in a v1 signature block.
        let certificates = Self.v1Changed { _, files in files[2].content = Self.withCertificates(files[2].content, count: limit + 1) }
        #expect(try Self.failure(certificates)?.kind == .damaged)
    }

    /// A PKCS#7 signature block whose certificate stands `count` times in it.
    static func withCertificates(_ block: Data, count: Int) -> Data {
        let outer = DERWriter.children(of: block)
        var fields = DERWriter.children(of: DERWriter.children(of: outer[1])[0])
        let index = fields.firstIndex { $0.first == 0xA0 }!
        fields[index] = DERWriter.tagged(0xA0, Array(repeating: DERWriter.children(of: fields[index])[0], count: count).reduce(Data(), +))
        return DERWriter.sequence([outer[0], DERWriter.tagged(0xA0, DERWriter.sequence(fields))])
    }

    @Test func theReportReadsNoMoreSignersAndLevelsThanTheCheck() throws {
        let limit = APKSignatures.maximumListLength
        // Signers, each with a certificate of its own.
        let signers = (0..<20).map { index in
            let signedData = TestSigner.prefixed(Data()) + TestSigner.prefixed(TestSigner.prefixed(TestSigner.u32(UInt32(index))))
                + TestSigner.prefixed(Data())
            return TestSigner.prefixed(signedData) + TestSigner.prefixed(Data()) + TestSigner.prefixed(Data())
        }
        let many = TestSigner.sign(Self.zip(), extra: [(TestSigner.v2ID, SigningBlockBuilder.schemeValue(signers))])
        #expect(APKSignatures.read(try ZipArchive(data: many)).certificates.count == JarVerifier.maximumSigners)
        // Lineage levels.
        let levels = (0..<limit + 10).map { index in
            TestSigner.prefixed(TestSigner.prefixed(TestSigner.prefixed(TestSigner.u32(UInt32(index))) + TestSigner.u32(0)) + TestSigner.u32(1))
        }.reduce(Data(), +)
        let long = TestSigner.sign(Self.zip(), v2: [.init(.ec)],
                                   v3: [.init(.ec, attributes: [(TestSigner.proofOfRotationID, TestSigner.u32(1) + levels)])])
        #expect(try #require(APKSignatures.read(try ZipArchive(data: long)).signer).lineage.count <= limit)
        // Attributes before the lineage.
        let late = TestSigner.sign(Self.zip(), v2: [.init(.ec)], v3: [.init(.rotated, lineage: [.init(.ec), .init(.rotated)],
                                                                          attributes: Array(repeating: (0x1234, Data()), count: limit))])
        #expect(try #require(APKSignatures.read(try ZipArchive(data: late)).signer).lineage.isEmpty)
    }

    @Test func theReportReadsNoMoreV1BlocksThanTheCheck() throws {
        // Ten blocks before the real one: the check refuses the file, and the report stops there too.
        var zip = Self.zip()
        for index in 0..<JarVerifier.maximumSigners { zip.add("META-INF/A\(index).RSA", Data([0x30, 0x00])) }
        let apk = TestSigner.sign(zip, v1: TestSigner.V1([.ec]))
        #expect(APKSignatures.read(try ZipArchive(data: apk)).certificates.isEmpty)
    }

    @Test func tooManyV1Signers() throws {
        let apk = TestSigner.sign(Self.zip(), v1: TestSigner.V1(Array(repeating: TestKey.ec, count: 11)))
        #expect(try Self.failure(apk)?.kind == .signatureMismatch)
    }

    @Test func everySignersDigestCounts() throws {
        let apk = TestSigner.sign(Self.zip(), v2: [.init(.rsa, digestOf: Data("other bytes".utf8)), .init(.ec)])
        #expect(try Self.failure(apk)?.kind == .damaged)
    }

    @Test func digestsAndSignaturesNameTheSameAlgorithms() throws {
        let apk = TestSigner.sign(Self.zip(), v2: [.init(.ec, digestAlgorithms: [TestSigner.ecdsaSHA512])])
        #expect(try Self.failure(apk)?.kind == .signatureMismatch)
    }

    @Test func v3SignedAndroidVersionsMatchTheSigners() throws {
        let apk = TestSigner.sign(Self.zip(), v2: [.init(.ec)], v3: [.init(.ec, signedSdk: (24, 34))])
        #expect(try Self.failure(apk)?.kind == .signatureMismatch)
    }

    @Test func aLineageLevelNamesTheAlgorithmItWasSignedWith() throws {
        let apk = TestSigner.sign(Self.zip(), v2: [.init(.ec)], v3: [.init(.rotated, lineage: [
            .init(.ec), .init(.rotated, signedAlgorithm: TestSigner.ecdsaSHA512),
        ])])
        #expect(try Self.failure(apk)?.kind == .signatureMismatch)
    }

    @Test func theAnalyzerReportsTheSignerTheCheckProved() throws {
        // A second signature block without its .SF signs nothing: its certificate is no signer.
        var zip = Self.zip()
        zip.entries += TestSigner.jarSignatureFiles(for: zip.entries, TestSigner.V1([.ec]))
        zip.add("META-INF/ORPHAN.EC", TestSigner.pkcs7(signing: Data(), with: .rotated, TestSigner.V1([.rotated])))
        let archive = try ZipArchive(data: zip.build().data)
        #expect(APKSignatures.read(archive).signer == SignerIdentity(current: [TestKey.ec.sha256, TestKey.rotated.sha256]))
        let signature = APKAnalyzer.signature(of: archive, targetSdk: 29)
        #expect(signature.check == .verified)
        #expect(signature.signer == SignerIdentity(current: [TestKey.ec.sha256]))
        #expect(signature.proof == SignerProof(scheme: .v1, certificates: [TestKey.ec.certificate]))
    }

    // MARK: - v1's entries, as Android's verifier sees them

    static func v1Signed(_ change: (inout ZipBuilder) -> Void) -> Data {
        var zip = Self.zip()
        let files = TestSigner.jarSignatureFiles(for: zip.entries, TestSigner.V1([.ec]))
        change(&zip)
        zip.entries += files
        return zip.build().data
    }

    @Test(arguments: ["assets/extra.txt", "META-INF.txt", "META-INFO/extra.txt"])
    func v1AnEntryAddedAfterSigning(name: String) throws {
        // Only META-INF/ itself goes unsigned.
        let apk = Self.v1Signed { $0.add(name, Data("extra".utf8)) }
        #expect(try Self.failure(apk)?.kind == .signatureMismatch)
    }

    @Test func v1ASignatureBlockWithoutItsSignatureFile() throws {
        let apk = Self.v1Changed { _, files in files.removeAll { $0.name.hasSuffix(".SF") } }
        #expect(try Self.failure(apk)?.kind == .signatureMismatch)
    }

    @Test func v1AFileInMetaInfNeedsNoSignature() throws {
        #expect(try Self.verify(Self.v1Signed { $0.add("META-INF/extra.txt", Data("extra".utf8)) }) == Self.verified([.ec]))
    }

    @Test(arguments: ["classes.dex", "lib/arm64-v8a/libgame.so", longName])
    func v1AListedEntryMissingFromTheZip(name: String) throws {
        // As StrictJarFile: "File … in manifest does not exist".
        #expect(try Self.failure(Self.v1Signed { $0.entries.removeAll { $0.name == name } })?.kind == .damaged)
    }

    @Test func v1ComparesNamesByTheirCodePoints() throws {
        // Signed as "é", zipped as "e" with a combining accent: equal Swift Strings, other names on Android.
        var zip = Self.zip()
        zip.add("assets/caf\u{E9}.txt", Data("menu".utf8))
        let files = TestSigner.jarSignatureFiles(for: zip.entries, TestSigner.V1([.ec]))
        zip.entries[zip.entries.count - 1].name = "assets/cafe\u{301}.txt"
        zip.entries += files
        #expect(try Self.failure(zip.build().data) != nil)
    }

    @Test func v1TwoNamesThatOnlyLookAlikeAreNoDuplicates() throws {
        // Android's ZIP reader compares names exactly; the one not in the manifest is unsigned.
        let apk = Self.v1Signed { $0.add("classes.de\u{301}x", Data("x".utf8)); $0.add("classes.d\u{E9}x", Data("x".utf8)) }
        #expect(try Self.failure(apk)?.kind == .signatureMismatch)
    }

    @Test func v1ADuplicateEntry() throws {
        // Even with the same content: Android's ZIP reader refuses both.
        for content in [Data("another".utf8), Self.zip().entries[1].content] {
            let apk = Self.v1Signed { $0.add("classes.dex", content) }
            #expect(try Self.failure(apk)?.kind == .damaged)
        }
    }

    /// v1 files for `zip`, then `change` on the entries and the signature files before they are zipped.
    static func v1Changed(_ v1: TestSigner.V1 = TestSigner.V1([.ec]),
                          _ change: (inout [ZipBuilder.Entry], inout [ZipBuilder.Entry]) -> Void) -> Data {
        var zip = Self.zip()
        var files = TestSigner.jarSignatureFiles(for: zip.entries, v1)
        change(&zip.entries, &files)
        zip.entries += files
        return zip.build().data
    }

    static func replace(_ old: String, with new: String, in entry: inout ZipBuilder.Entry) {
        entry.content = Data(String(decoding: entry.content, as: UTF8.self).replacingOccurrences(of: old, with: new).utf8)
    }

    @Test(arguments: [true, false])
    func v1AnEntryAndItsManifestDigestChangedAfterSigning(wholeManifestDigest: Bool) throws {
        let apk = Self.v1Changed(TestSigner.V1([.ec], wholeManifestDigest: wholeManifestDigest)) { entries, files in
            let index = entries.firstIndex { $0.name == "classes.dex" }!
            let old = TestSigner.digest(entries[index].content, "SHA-256").base64EncodedString()
            entries[index].content = Data("changed".utf8)
            Self.replace(old, with: TestSigner.digest(entries[index].content, "SHA-256").base64EncodedString(), in: &files[0])
        }
        #expect(try Self.failure(apk)?.kind == .damaged)
    }

    @Test func v1AChangedMainSection() throws {
        let apk = Self.v1Changed { _, files in
            Self.replace("Created-By: Minato tests\r\n", with: "Created-By: Minato tests\r\nX-Extra: 1\r\n", in: &files[0])
        }
        #expect(try Self.failure(apk)?.kind == .damaged)
    }

    @Test func v1AChangedSignatureFileWithSignedAttributes() throws {
        let apk = Self.v1Changed(TestSigner.V1([.rsa], signedAttributes: true)) { _, files in
            files[1].content.append(contentsOf: Data("Name: extra\r\nSHA-256-Digest: AAAA\r\n\r\n".utf8))
        }
        #expect(try Self.failure(apk)?.kind == .signatureMismatch)
    }

    @Test func v1AnEntryOnlyOneSignerSigned() throws {
        var zip = Self.zip()
        let first = TestSigner.jarSignatureFiles(for: zip.entries, TestSigner.V1([.ec]))
        zip.add("assets/later.txt", Data("later".utf8))
        let second = TestSigner.jarSignatureFiles(for: zip.entries, TestSigner.V1([.rsa]))
        // The second manifest lists every entry; the first signer's .SF lacks the one added later.
        zip.entries += [second[0]] + first.dropFirst().map {
            ZipBuilder.Entry(name: $0.name.replacingOccurrences(of: "SIGNER0", with: "FIRST"), content: $0.content)
        } + second.dropFirst()
        let failure = try Self.failure(zip.build().data)
        #expect(failure?.kind == .signatureMismatch)
        #expect(failure?.detail.contains("every signer") == true)
    }

    // MARK: - Stripped signatures

    @Test func v1ThatNamesARemovedV2() throws {
        for header in ["2", "2, 3"] {
            let apk = TestSigner.sign(Self.zip(), v1: TestSigner.V1([.ec], androidSigned: header))
            #expect(try Self.failure(apk)?.kind == .signatureMismatch)
        }
        // Scheme IDs Android does not know are ignored.
        #expect(try Self.verify(TestSigner.sign(Self.zip(), v1: TestSigner.V1([.ec], androidSigned: "7"))) == Self.verified([.ec]))
    }

    @Test func v2ThatNamesARemovedV3() throws {
        let apk = TestSigner.sign(Self.zip(), v2: [.init(.ec, attributes: [(TestSigner.strippingProtectionID, TestSigner.u32(3))])])
        #expect(try Self.failure(apk)?.kind == .signatureMismatch)
        let kept = TestSigner.sign(Self.zip(), v2: [.init(.ec, attributes: [(TestSigner.strippingProtectionID, TestSigner.u32(3))])],
                                   v3: [.init(.ec)])
        #expect(try Self.verify(kept) == Self.verified([.ec]))
    }

    @Test func v3ThatNamesARemovedV31() throws {
        let apk = TestSigner.sign(Self.zip(), v2: [.init(.ec)],
                                  v3: [.init(.ec, attributes: [(TestSigner.rotationMinSdkID, TestSigner.u32(33))])])
        #expect(try Self.failure(apk)?.kind == .signatureMismatch)
    }

    // MARK: - The lineage

    @Test func aLineageWithItsCapabilities() throws {
        let flags = TestSigner.installedData | TestSigner.rollback
        let apk = TestSigner.sign(Self.zip(), v2: [.init(.ec)], v3: [.init(.rotated, lineage: [.init(.ec, flags: flags), .init(.rotated)])])
        #expect(try Self.verify(apk) == Self.verified([.rotated], lineage: [LineageNode(sha256: TestKey.ec.sha256, flags: flags)]))
    }

    @Test func aProofOfRotationOfAnotherVersion() throws {
        // Android reads past the version (ApkSigningBlockUtils.java:845-846).
        var value = TestSigner.proofOfRotation([.init(.ec), .init(.rotated)])
        value[0] = 2
        let apk = TestSigner.sign(Self.zip(), v2: [.init(.ec)], v3: [.init(.rotated, attributes: [(TestSigner.proofOfRotationID, value)])])
        #expect(try Self.verify(apk) == Self.verified([.rotated], lineage: [LineageNode(sha256: TestKey.ec.sha256, flags: TestSigner.defaultCapabilities)]))
    }

    @Test func aProofOfRotationWithoutLevelsIsNoLineage() throws {
        let apk = TestSigner.sign(Self.zip(), v2: [.init(.ec)], v3: [.init(.rotated, attributes: [(TestSigner.proofOfRotationID, TestSigner.u32(1))])])
        #expect(try Self.verify(apk) == Self.verified([.rotated]))
        #expect(try Self.proof(apk)?.lineage == [])
    }

    @Test func twoProofsOfRotation() throws {
        // ApkSignatureSchemeV3Verifier.java:459-461.
        let second = TestSigner.proofOfRotation([.init(.rsa), .init(.rotated)])
        let apk = TestSigner.sign(Self.zip(), v2: [.init(.ec)], v3: [.init(.rotated, lineage: [.init(.ec), .init(.rotated)],
                                                                          attributes: [(TestSigner.proofOfRotationID, second)])])
        #expect(try Self.failure(apk)?.kind == .signatureMismatch)
    }

    @Test func aLineageAcrossKeyTypes() throws {
        let apk = TestSigner.sign(Self.zip(), v2: [.init(.rsa)], v3: [.init(.ec, lineage: [.init(.rsa), .init(.ec)])])
        #expect(try Self.verify(apk).check == .verified)
    }

    @Test func aBrokenLineage() throws {
        let broken = TestSigner.sign(Self.zip(), v2: [.init(.ec)],
                                     v3: [.init(.rotated, lineage: [.init(.ec), .init(.rotated, signedBy: .rsa)])])
        #expect(try Self.failure(broken)?.kind == .signatureMismatch)
        // The lineage must end with the signer's own certificate.
        let elsewhere = TestSigner.sign(Self.zip(), v2: [.init(.ec)], v3: [.init(.rotated, lineage: [.init(.ec), .init(.rsa)])])
        #expect(try Self.failure(elsewhere)?.kind == .signatureMismatch)
        // Every certificate once.
        let twice = TestSigner.sign(Self.zip(), v2: [.init(.ec)],
                                    v3: [.init(.rotated, lineage: [.init(.rotated), .init(.ec), .init(.rotated)])])
        #expect(try Self.failure(twice)?.kind == .signatureMismatch)
    }

    // MARK: - Malformed input

    @Test(arguments: ["Manifest-Version: 1.0\n \n", "Manifest-Version: 1.0\r\n \r\n \r\n", "A: x\n \n y\n\n"])
    func aContinuationLineOfOnlyASpace(text: String) throws {
        let error = #expect(throws: SignatureFailure.self) { try JarManifest(Data(text.utf8), name: "MANIFEST.MF") }
        #expect(error?.kind == .damaged)
    }

    @Test func aV1FileWithSuchAManifest() throws {
        let apk = Self.v1Changed { _, files in files[0].content = Data("Manifest-Version: 1.0\n \n".utf8) }
        #expect(try Self.failure(apk)?.kind == .damaged)
    }

    @Test func noManifestTraps() {
        let pieces = ["Name: x", "Name: y", "a: b", "SHA-256-Digest: AA", ":", " ", "  ", "\r", "\n", "\r\n", "\0", "\u{E9}"]
        var random = SplitMix64(seed: 34)
        for _ in 0..<50_000 {
            let text = (0..<Int.random(in: 0...12, using: &random)).map { _ in pieces.randomElement(using: &random)! }.joined()
            _ = try? JarManifest(Data(text.utf8), name: "MANIFEST.MF")
        }
    }

    @Test func noSignatureBlockTraps() throws {
        let files = TestSigner.jarSignatureFiles(for: Self.zip().entries, TestSigner.V1([.rsa], signedAttributes: true))
        let block = files[2].content
        for offset in 0..<block.count {
            _ = try? PKCS7Signature.verify(Self.flipped(block, at: offset), content: files[1].content, name: "X.RSA")
            _ = try? PKCS7Signature.verify(block.prefix(offset), content: files[1].content, name: "X.RSA")
        }
    }

    // MARK: - v1 signers Android leaves out

    /// A v1 APK signed by .ec and .rsa whose second .SF is `change`d and signed again.
    static func v1WithASecondSignatureFile(_ change: (inout String) -> Void) -> Data {
        v1Changed(TestSigner.V1([.ec, .rsa])) { _, files in
            var text = String(decoding: files[3].content, as: UTF8.self)
            change(&text)
            files[3].content = Data(text.utf8)
            files[4].content = TestSigner.pkcs7(signing: files[3].content, with: .rsa, TestSigner.V1([.rsa]))
        }
    }

    @Test func v1ASignatureFileWithoutItsVersionSignsNothing() throws {
        let apk = Self.v1WithASecondSignatureFile { $0 = $0.replacingOccurrences(of: "Signature-Version: 1.0\r\n", with: "") }
        #expect(try Self.verify(apk) == Self.verified([.ec]))
        #expect(try Self.verify(Self.v1WithASecondSignatureFile { $0 = "" }) == Self.verified([.ec]))
    }

    @Test func v1ASignerOfAnEntryTheManifestLacksIsLeftOut() throws {
        // Without the whole manifest's digest, each section counts; one the manifest lacks drops the signer.
        let apk = Self.v1WithASecondSignatureFile {
            $0 = $0.split(separator: "\r\n", omittingEmptySubsequences: false).filter { !$0.contains("-Digest-Manifest:") }
                .joined(separator: "\r\n") + "Name: ghost\r\nSHA-256-Digest: AAAA\r\n\r\n"
        }
        #expect(try Self.verify(apk) == Self.verified([.ec]))
    }

    @Test func v1SignatureBlocksAsAndroidFindsThem() throws {
        // Case matters, and META-INF/'s folders count.
        let lower = Self.v1Changed(TestSigner.V1([.ec, .rsa])) { _, files in
            files[4].name = "META-INF/SIGNER1.rsa"
        }
        #expect(try Self.verify(lower) == Self.verified([.ec]))
        let nested = Self.v1Changed(TestSigner.V1([.ec, .rsa])) { _, files in
            files[3].name = "META-INF/more/SIGNER1.SF"
            files[4].name = "META-INF/more/SIGNER1.RSA"
        }
        #expect(try Self.verify(nested) == Self.verified([.ec, .rsa]))
    }

    @Test func v1TheStrippingCheckComesBeforeTheVersion() throws {
        // StrictJarVerifier.java:399-452: a .SF that names v2, which the APK lacks, fails it even without its version.
        let apk = Self.v1WithASecondSignatureFile {
            $0 = $0.replacingOccurrences(of: "Signature-Version: 1.0\r\n", with: "X-Android-APK-Signed: 2\r\n")
        }
        #expect(try Self.failure(apk)?.kind == .signatureMismatch)
    }

    /// Lines that make Android's reader throw an IOException, so verifyCertificate gives up on the .SF (StrictJarVerifier
    /// .java:392-397): a name with another character or longer than 70 (Attributes.java:480-503), a colon without a
    /// space, and a NUL (StrictJarManifestReader.java:124-137, 150-151).
    @Test(arguments: ["Bad Name: x\r\n", String(repeating: "X", count: 71) + ": x\r\n", "X-Colon:x\r\n", "X-Nul: a\0b\r\n"])
    func v1ASignatureFileAndroidCannotReadIsLeftOut(line: String) throws {
        let apk = Self.v1WithASecondSignatureFile {
            $0 = $0.replacingOccurrences(of: "Signature-Version: 1.0\r\n", with: "Signature-Version: 1.0\r\n" + line)
        }
        #expect(try Self.verify(apk) == Self.verified([.ec]))
        // So is one with a section that has no name (StrictJarManifestReader.java:61-63).
        #expect(try Self.verify(Self.v1WithASecondSignatureFile { $0 += "Unnamed: x\r\n\r\n" }) == Self.verified([.ec]))
    }

    @Test(arguments: ["X:", "X: a\r\n \r\n"])
    func v1ASignatureFileThatBreaksAndroidsReaderFailsTheAPK(end: String) throws {
        // Reading past a colon at the end, or a continuation line of only a space, throws a runtime exception, which
        // refuses the APK (ApkSignatureVerifier.java:502-504).
        #expect(try Self.failure(Self.v1WithASecondSignatureFile { $0 += end })?.kind == .damaged)
    }

    @Test func v1OnlyASignatureFileMayNameAnEntryTwice() throws {
        // readEntries keeps one set of attributes per name: a .SF's second section adds to it, a manifest's throws
        // (StrictJarManifestReader.java:66-89).
        let section = "Name: classes.dex\r\nX-Extra: 1\r\n\r\n"
        #expect(try Self.verify(Self.v1WithASecondSignatureFile { $0 += section }) == Self.verified([.ec, .rsa]))
        let manifest = Self.v1Changed { _, files in files[0].content += Data(section.utf8) }
        #expect(try Self.failure(manifest)?.kind == .damaged)
        // Android refuses a manifest its reader cannot read.
        #expect(throws: (any Error).self) { try JarManifest(Data("Manifest-Version: 1.0\r\nBad Name: x\r\n\r\n".utf8), name: "MANIFEST.MF") }
    }

    @Test func v1ASignatureFileChecksItsSectionsInTheOrderOfItsHashMap() throws {
        // Without the whole manifest's digest, verifyCertificate goes through the .SF's sections as its HashMap gives
        // them (StrictJarVerifier.java:474-485): a name the manifest lacks gives up on the .SF, a section that does not
        // match fails the APK. AndroidManifest.xml comes before ghost, and ghost before classes.dex (JDK 17's HashMap).
        func apk(wrong entry: String) -> Data {
            Self.v1WithASecondSignatureFile { text in
                var lines = text.components(separatedBy: "\r\n").filter { !$0.contains("-Digest-Manifest:") }
                lines[lines.firstIndex(of: "Name: \(entry)")! + 1] = "SHA-256-Digest: " + Data(count: 32).base64EncodedString()
                text = lines.joined(separator: "\r\n") + "Name: ghost\r\nSHA-256-Digest: AAAA\r\n\r\n"
            }
        }
        #expect(try Self.failure(apk(wrong: "AndroidManifest.xml"))?.kind == .damaged)
        #expect(try Self.verify(apk(wrong: "classes.dex")) == Self.verified([.ec]))
    }

    // MARK: - Streaming

    @Test(arguments: [true, false])
    func noMoreThanTheEntrysSizeComesOut(stored: Bool) throws {
        // The central directory says 1,000 bytes; the data holds 3 MiB.
        var zip = ZipBuilder()
        zip.add("big.bin", Data(repeating: 0x61, count: 3 << 20), method: stored ? .stored : .deflated)
        let output = zip.build()
        var data = output.data
        let sizeField = output.centralDirectoryOffset + 24
        data.replaceSubrange(sizeField..<sizeField + 4, with: TestSigner.u32(1_000))
        let archive = try ZipArchive(data: data)
        var total = 0
        #expect(throws: APKError.self) {
            try archive.forEachChunk(of: #require(archive.entry(named: "big.bin")), chunkSize: 1 << 16) { total += $0.count }
        }
        #expect(total <= 1_000)
    }

    @Test(arguments: [true, false])
    func entriesComeInBoundedPieces(stored: Bool) throws {
        let content = Data((0..<(3 << 20) + 12_345).map { UInt8(truncatingIfNeeded: $0 &* 31 &+ $0 >> 9) })
        var zip = ZipBuilder()
        zip.add("big.bin", content, method: stored ? .stored : .deflated)
        let archive = try ZipArchive(data: zip.build().data)
        var pieces: [Data] = []
        try archive.forEachChunk(of: #require(archive.entry(named: "big.bin")), chunkSize: 1 << 20) { pieces.append($0) }
        #expect(pieces.allSatisfy { $0.count <= 1 << 20 })
        #expect(pieces.reduce(Data(), +) == content)
    }

    // MARK: - What the check proves (signers.json, ADR-0034 decision 2)

    @Test func v1ProvesTheCertificateItsSignerInfoNames() throws {
        // Re-signed with another key, the developer's certificate first in the block: Android takes the one the
        // SignerInfo names and whose key verifies (StrictJarVerifier.java:330-353).
        var v1 = TestSigner.V1([.rotated])
        v1.certificatesBefore = [TestKey.ec.certificate]
        let apk = TestSigner.sign(Self.zip(), v1: v1)
        #expect(try Self.proof(apk) == SignerProof(scheme: .v1, certificates: [TestKey.rotated.certificate]))
        #expect(try Self.verify(apk) == Self.verified([.rotated]))
    }

    @Test func v1ABlockWithoutItsSignatureFileProvesNothing() throws {
        // StrictJarVerifier.java:365-371: a block without its .SF is skipped, whatever certificate it carries.
        var zip = Self.zip()
        zip.add("META-INF/AAA.RSA", TestSigner.pkcs7(signing: Data(), with: .rsa, TestSigner.V1([.rsa])))
        zip.entries += TestSigner.jarSignatureFiles(for: Self.zip().entries, TestSigner.V1([.rotated]))
        let apk = zip.build().data
        #expect(try Self.proof(apk) == SignerProof(scheme: .v1, certificates: [TestKey.rotated.certificate]))
    }

    @Test func v1SignersInTheArchivesOrder() throws {
        let apk = TestSigner.sign(Self.zip(), v1: TestSigner.V1([.rsa, .ec]))
        #expect(try Self.proof(apk) == SignerProof(scheme: .v1, certificates: [TestKey.rsa.certificate, TestKey.ec.certificate]))
    }

    /// An APK whose MANIFEST.MF, `name`.SF and two blocks for it, `name`.RSA and `name`.EC, come in this order, then
    /// `extra` empty files in META-INF/; `ec` replaces the .EC block.
    static func twoBlocks(_ name: String, ec: Data? = nil, extra: [String] = []) -> Data {
        // MANIFEST.MF, SIGNER0.SF, SIGNER0.RSA, SIGNER1.SF and SIGNER1.EC: both .SF hold the same bytes.
        var files = TestSigner.jarSignatureFiles(for: Self.zip().entries, TestSigner.V1([.rsa, .ec]))
        files.removeAll { $0.name == "META-INF/SIGNER1.SF" }
        for index in files.indices where files[index].name.hasPrefix("META-INF/SIGNER") {
            files[index].name = "META-INF/\(name)." + files[index].name.split(separator: ".").last!
        }
        if let ec { files[files.count - 1].content = ec }
        var zip = Self.zip()
        zip.entries += files + extra.map { ZipBuilder.Entry(name: "META-INF/" + $0, content: Data()) }
        return zip.build().data
    }

    /// StrictJarVerifier keys each chain by its .SF (365-383) and drops the .SF's bytes once a block of it signs (487), so
    /// two blocks for one .SF are one signer: the block it verifies first. It verifies them in the order of
    /// StrictJarFile's metaEntries, a java.util.HashMap over the META-INF names (readCertificates, 296-316). For SIGNER0
    /// that is the .RSA, for SIGNER1 the .EC, though the .RSA comes first in both archives (the orders are the JDK 17
    /// HashMap's).
    @Test func v1TwoBlocksForOneSignatureFileAreOneSigner() throws {
        for (name, winner) in [("SIGNER0", TestKey.rsa.certificate), ("SIGNER1", TestKey.ec.certificate)] {
            #expect(try Self.proof(Self.twoBlocks(name)) == SignerProof(scheme: .v1, certificates: [winner]), "\(name)")
        }
        // A bucket's 9th name grows a table below 64 (HashMap.java:648-649, 760-763), and K4.EC then comes first.
        let grown = Self.twoBlocks("K4", extra: ["F2", "F50", "F61", "F72", "F83", "F94", "F100", "F111"])
        #expect(try Self.proof(grown) == SignerProof(scheme: .v1, certificates: [TestKey.ec.certificate]))
    }

    @Test func v1ABlockAfterTheOneThatSignedIsNotRead() throws {
        // SIGNER0.RSA comes first and signs, so Android never reads SIGNER0.EC (StrictJarVerifier.java:368-371, 487).
        let apk = Self.twoBlocks("SIGNER0", ec: Data("not a signature block".utf8))
        #expect(try Self.verify(apk) == Self.verified([.rsa]))
        #expect(try Self.proof(apk) == SignerProof(scheme: .v1, certificates: [TestKey.rsa.certificate]))
    }

    @Test func v1EveryBlockOfASignatureFileThatGivesUpIsRead() throws {
        // A .SF that gives up keeps its bytes, so each of its blocks is verified (StrictJarVerifier.java:368-371, 449-452):
        // SIGNER1.EC comes first, then SIGNER0.EC and SIGNER1.RSA (JDK 17's HashMap).
        func apk(_ block: Data?) -> Data {
            Self.v1Changed(TestSigner.V1([.ec, .rsa])) { _, files in
                Self.replace("Signature-Version: 1.0\r\n", with: "", in: &files[3])
                files[4].content = block ?? TestSigner.pkcs7(signing: files[3].content, with: .rsa, TestSigner.V1([.rsa]))
                files.append(ZipBuilder.Entry(name: "META-INF/SIGNER1.EC",
                                              content: TestSigner.pkcs7(signing: files[3].content, with: .ec, TestSigner.V1([.ec]))))
            }
        }
        #expect(try Self.verify(apk(nil)) == Self.verified([.ec]))
        #expect(try Self.failure(apk(Data("not a signature block".utf8)))?.kind == .damaged)
    }

    @Test func v1AnUncheckableBlockOfASignatureFileThatGivesUp() throws {
        // Android verifies that block too, and Minato cannot check DSA, so it cannot tell Android's answer.
        let apk = Self.v1Changed(TestSigner.V1([.ec, .dsa])) { _, files in
            Self.replace("Signature-Version: 1.0\r\n", with: "", in: &files[3])
            files[4].content = TestSigner.pkcs7(signing: files[3].content, with: .dsa, TestSigner.V1([.dsa]))
        }
        let check = try Self.verify(apk).check
        guard case .unverified = check else { Issue.record("got \(check)"); return }
    }

    /// java.util.HashMap's iteration order, as the JDK 17's gives it for these names put in this order: 4 names in a
    /// table of 16, 14 in a table of 32 after its resize, 12 whose 9th name in one bucket grows the table to 32, 11
    /// with only 8 in one bucket, 21 whose bucket of the grown table gets its 9th name later, and a Kelvin sign that
    /// Swift's String equates with a K.
    @Test func v1BlocksComeInTheOrderOfJavasHashMap() {
        func order(_ names: [String]) -> [String] {
            let rank = JarVerifier.hashMapOrder(names.map { "META-INF/" + $0 })
            return names.sorted { rank[.init("META-INF/" + $0)]! < rank[.init("META-INF/" + $1)]! }
        }
        #expect(order(["MANIFEST.MF", "K4.SF", "K4.RSA", "K4.EC", "F2", "F50", "F61", "F72", "F83", "F94", "F100", "F111"])
            == ["K4.EC", "K4.SF", "F2", "F100", "F111", "K4.RSA", "F50", "F61", "F72", "F83", "F94", "MANIFEST.MF"])
        #expect(order(["MANIFEST.MF", "A.SF", "A.RSA", "A.EC", "N30", "N41", "N52", "N63", "N4", "N102", "N113"])
            == ["A.SF", "A.RSA", "A.EC", "N30", "N41", "N52", "N63", "N4", "N102", "N113", "MANIFEST.MF"])
        let later = ["MANIFEST.MF", "N3"] + (0...11).filter { $0 != 3 }.map { "N\($0)" }
            + ["N202", "N224", "N246", "N268", "N213", "N235", "N257", "N279"]
        #expect(order(later) == [
            "N10", "N11", "N3", "N213", "N235", "N257", "N279", "N2", "N1", "N0", "N7", "N6", "N5", "N4", "MANIFEST.MF", "N9", "N8",
            "N202", "N224", "N246", "N268",
        ])
        #expect(order(["MANIFEST.MF", "K.SF", "K.RSA", "\u{212A}.RSA"]).map { ExactName($0) }
            == ["K.SF", "\u{212A}.RSA", "K.RSA", "MANIFEST.MF"].map { ExactName($0) })
        #expect(order(["MANIFEST.MF", "CERT.SF", "CERT.RSA", "CERT.EC"])
            == ["CERT.SF", "CERT.EC", "CERT.RSA", "MANIFEST.MF"])
        let names = [
            "MANIFEST.MF", "CERT.SF", "CERT.RSA", "CERT.EC", "services/a", "services/b", "kotlin.kotlin_module",
            "androidx.core_core.version", "LICENSE", "NOTICE", "proguard/x.pro", "com/android/build/gradle/app-metadata.properties",
            "version-control-info.textproto", "A.SF",
        ]
        #expect(order(names) == [
            "NOTICE", "LICENSE", "CERT.EC", "version-control-info.textproto", "proguard/x.pro", "CERT.SF", "services/a", "A.SF",
            "services/b", "CERT.RSA", "androidx.core_core.version", "com/android/build/gradle/app-metadata.properties",
            "MANIFEST.MF", "kotlin.kotlin_module",
        ])
    }

    @Test func v2ProvesEachSignersOwnCertificate() throws {
        let apk = TestSigner.sign(Self.zip(), v2: [.init(.rotated, chain: [TestKey.rsa.certificate]), .init(.ec)])
        #expect(try Self.proof(apk) == SignerProof(scheme: .v2, certificates: [TestKey.rotated.certificate, TestKey.ec.certificate]))
    }

    @Test func v3ProvesItsLineageWithTheFlags() throws {
        let apk = TestSigner.sign(Self.zip(), v2: [.init(.rsa)],
                                  v3: [.init(.rotated, lineage: [.init(.rsa, flags: 0x8000_0001), .init(.ec, flags: 3), .init(.rotated)])])
        #expect(try Self.proof(apk) == SignerProof(scheme: .v3, certificates: [TestKey.rotated.certificate], lineage: [
            .init(certificate: TestKey.rsa.certificate, flags: 0x8000_0001), .init(certificate: TestKey.ec.certificate, flags: 3),
            .init(certificate: TestKey.rotated.certificate, flags: TestSigner.defaultCapabilities),
        ]))
    }

    @Test func v31ProvesItsOwnScheme() throws {
        let apk = TestSigner.sign(Self.zip(), v2: [.init(.ec)],
                                  v3: [.init(.ec, maxSdk: 32, attributes: [(TestSigner.rotationMinSdkID, TestSigner.u32(33))])],
                                  v31: [.init(.rotated, minSdk: 33, lineage: [.init(.ec), .init(.rotated)])])
        let proof = try #require(try Self.proof(apk))
        #expect(proof.scheme == .v31)
        #expect(proof.certificates == [TestKey.rotated.certificate])
        #expect(proof.lineage.map(\.certificate) == [TestKey.ec.certificate, TestKey.rotated.certificate])
    }

    @Test func nothingProvenWithoutAVerifiedCheck() throws {
        for apk in [
            Self.zip().build().data,
            TestSigner.sign(Self.zip(), v1: TestSigner.V1([.dsa])),
            TestSigner.sign(Self.zip(), v2: [.init(.ec, algorithms: [TestSigner.ecdsaSHA256Verity])]),
            TestSigner.sign(Self.zip(), v2: [.init(.ec)], v3: [.init(.ec), .init(.rotated)]),
        ] {
            #expect(try Self.proof(apk) == nil)
        }
    }

    @Test func certificatesLargerThanSignersJsonHoldsAreNotProven() throws {
        // A real certificate with a 1.1 MB subject: verified, but more than signers.json holds, so unverified.
        let certificate = Self.certificate(TestKey.ec.certificate, withSubjectOf: 1_100_000)
        let apk = TestSigner.sign(Self.zip(), v2: [.init(.ec, certificate: certificate)])
        #expect(try Self.verify(apk) == Outcome(check: .unverified("certificates larger than the 1 MiB Minato records")))
        #expect(try Self.proof(apk) == nil)
    }

    /// `certificate` with a subject of about `size` bytes.
    static func certificate(_ certificate: Data, withSubjectOf size: Int) -> Data {
        let parts = DERWriter.children(of: certificate)
        var tbs = DERWriter.children(of: parts[0])
        let subject = (tbs.first?.first == 0xA0 ? 1 : 0) + 4
        tbs[subject] = DERWriter.sequence([DERWriter.set([DERWriter.sequence([
            DERWriter.oid("2.5.4.3"), DERWriter.tagged(0x0C, Data(repeating: 0x61, count: size)),
        ])])])
        return DERWriter.sequence([DERWriter.sequence(tbs), parts[1], parts[2]])
    }

    // MARK: - targetSdkVersion 30 and later

    @Test func v1AloneFromTargetSdk30() throws {
        // ApkSignatureVerifier.java:218-226 with getMinimumSignatureSchemeVersionForTargetSdk (572-577).
        let v1 = TestSigner.sign(Self.zip(), v1: TestSigner.V1([.ec]))
        #expect(try Self.verify(v1, targetSdk: 29) == Self.verified([.ec]))
        for targetSdk in [30, 35] {
            let failure = try #require(try Self.failure(v1, targetSdk: targetSdk))
            #expect(failure.kind == .noV2Signature)
            #expect(failure.detail.hasPrefix("No APK Signature Scheme v2 signature"))
        }
        // Not looked at, so a v1 signature Security.framework cannot check fails the same way.
        #expect(try Self.failure(TestSigner.sign(Self.zip(), v1: TestSigner.V1([.dsa])), targetSdk: 30)?.kind == .noV2Signature)
        // v2 and newer pass; an unsigned file stays unsigned (Minato installs it, ADR-0034).
        #expect(try Self.verify(TestSigner.sign(Self.zip(), v1: TestSigner.V1([.ec]), v2: [.init(.ec)]), targetSdk: 30) == Self.verified([.ec]))
        #expect(try Self.verify(Self.zip().build().data, targetSdk: 30) == Outcome(check: .unsigned))
    }
}
