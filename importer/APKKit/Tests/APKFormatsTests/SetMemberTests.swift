import APKFormats
import APKKit
import APKTestSupport
import Foundation
import Testing

/// What one APK of a set says about itself, read as Android 15's installer reads it (ADR-0048, decision 2).
@Suite("Set members")
struct SetMemberTests {
    static func member(_ data: Data, named fileName: String = "x.apk") throws -> SetMember {
        try SetMember.read(ZipArchive(data: data), fileName: fileName, sha256: "00")
    }

    @Test func readsWhatAndroidsInstallerReads() throws {
        let member = try Self.member(SplitAPK.make(
            versionCode: 7, versionCodeMajor: 1, split: "feature.config.xxhdpi", configForSplit: "feature",
            manifestSplitRequired: true, requiredSplitTypes: " base__abi, base__density ", splitTypes: "feature__density",
            usesSplit: "other", targetSdk: 34, abis: ["arm64-v8a"]))

        #expect(member.packageName == SplitAPK.package)
        #expect(member.versionCode == 1 << 32 | 7)
        #expect(member.splitName == "feature.config.xxhdpi")
        #expect(member.configForSplit == "feature")
        #expect(member.splitRequired)
        #expect(!member.stricterSplitSignal)
        #expect(member.requiredSplitTypes == ["base__abi", "base__density"])
        #expect(member.splitTypes == ["feature__density"])
        #expect(member.usesSplit == "other")
        #expect(member.targetSdk == 34)
        #expect(member.abis == ["arm64-v8a"])
        #expect(member.signature.check == .verified)
        #expect(member.fileName == "x.apk")
    }

    @Test(arguments: [(false, true), (true, false)])
    func readsBundletoolsSignalsAsStricter(application: Bool, metaData: Bool) throws {
        // Android 15 ignores both; Minato reads them as "Needs split APKs".
        let member = try Self.member(SplitAPK.make(applicationSplitRequired: application, splitsRequiredMetaData: metaData))
        #expect(!member.splitRequired)
        #expect(member.stricterSplitSignal)
    }

    /// `getAttributeBooleanValue` (`ApkLiteParseUtils.java:431-432`, android-15.0.0_r1): any integer type's data, and
    /// false for a string.
    @Test func readsIsolatedSplitsAsAndroidsInstallerDoes() throws {
        let isolated = { (value: AXMLBuilder.Value) in
            try Self.member(SplitAPK.make(attributes: [.android("isolatedSplits", 0x0101_054B, value)])).isolatedSplits
        }

        #expect(try isolated(.bool(true)))
        #expect(try isolated(.typed(type: 0x10, data: 2)))
        #expect(try !isolated(.bool(false)))
        #expect(try !isolated(.string("true")))
        #expect(try !Self.member(SplitAPK.make()).isolatedSplits)
    }

    @Test func rolesComeFromTheManifestNotTheFileName() throws {
        #expect(try Self.member(SplitAPK.make(), named: "split_config.en.apk").role == .base)
        #expect(try Self.member(SplitAPK.make(split: "config.arm64_v8a"), named: "base.apk").role
            == .config(module: nil, suffix: "arm64_v8a"))
        #expect(try Self.member(SplitAPK.make(split: "dynamic", isFeatureSplit: true)).role == .feature("dynamic"))
        #expect(try Self.member(SplitAPK.make(split: "dynamic.config.de")).role == .config(module: "dynamic", suffix: "de"))
        #expect(try Self.member(SplitAPK.make(split: "dynamic.config.de", configForSplit: "dynamic")).role
            == .config(module: "dynamic", suffix: "de"))
        #expect(try Self.member(SplitAPK.make(split: "other")).role == .other("other"))
    }

    @Test(arguments: [("../x", "bad character '/'"), ("a/b", "bad character '/'"), ("config.1x", "bad character '1'"),
                      ("x-y", "bad character '-'")])
    func refusesASplitNameAndroidRefuses(name: String, reason: String) {
        #expect(throws: SetMemberError(fileName: "x.apk", reason: "Invalid manifest split: \(reason)")) {
            try Self.member(SplitAPK.make(split: name))
        }
    }

    @Test(arguments: [("base__abi,,x", "Invalid filename"), ("a/b", "bad character '/'"), ("1abc", "bad character '1'"),
                      (".", "Invalid filename"), ("a,..", "Invalid filename"), (" ", "Invalid filename"), ("base__abi\u{A0}", "bad character '\u{A0}'"),
                      (String(repeating: "a", count: 224), "the length of the name is greater than 223"),
                      (String(repeating: "a", count: 255), "the length of the name is greater than 223"),
                      (String(repeating: "a", count: 256), "Invalid filename")])
    func refusesSplitTypesAndroidRefuses(types: String, reason: String) {
        #expect(throws: SetMemberError(fileName: "x.apk", reason: "Invalid manifest split types: \(reason)")) {
            try Self.member(SplitAPK.make(requiredSplitTypes: types))
        }
    }

    @Test(arguments: [("base__abi,", ["base__abi"]), (",", []), ("base__abi\n", ["base__abi"]), (" a ,\tb ", ["a", "b"]),
                      (String(repeating: "a", count: 223), [String(repeating: "a", count: 223)])])
    func splitsTypesAsJavaDoes(types: String, expected: [String]) throws {
        // String.trim drops every character up to U+0020, and split(",") drops trailing empty parts (:760-761).
        #expect(try Self.member(SplitAPK.make(requiredSplitTypes: types)).requiredSplitTypes == Set(expected))
    }

    @Test(arguments: [("nodot", "must have at least one '.' separator"), ("org.1x", "bad character '1'"),
                      ("org." + String(repeating: "a", count: 220), "the length of the name is greater than 223")])
    func refusesAPackageNameAndroidRefuses(package: String, reason: String) {
        #expect(throws: SetMemberError(fileName: "x.apk", reason: "Invalid manifest package: \(reason)")) {
            try Self.member(SplitAPK.make(package: package))
        }
    }

    @Test func thePackageAndroidNeedsNoSeparator() throws {
        #expect(try Self.member(SplitAPK.make(package: "android")).packageName == "android")
    }

    @Test func aUsesSplitWithoutItsNameIsRefused() {
        #expect(throws: SetMemberError(fileName: "x.apk", reason: "<uses-split> tag requires 'android:name' attribute")) {
            try Self.member(SplitAPK.make(split: "feature", elements: [AXMLBuilder.Element("uses-split")]))
        }
    }

    @Test func aSplitThatNeedsANewerAndroidIsRefused() throws {
        // Android's parse refuses any APK of the session (ApkLiteParseUtils.java:619-624); a base's refusal is the
        // analyzer's, which marks the app incompatible as for one APK (ADR-0039).
        let usesSdk = AXMLBuilder.Element("uses-sdk", [.android("minSdkVersion", 0x0101_020C, .int(99))])
        #expect(throws: SetMemberError(fileName: "x.apk", reason: "Requires newer sdk version #99 (current version is #35)")) {
            try Self.member(SplitAPK.make(split: "config.de", elements: [usesSdk]))
        }
        #expect(try Self.member(SplitAPK.make(elements: [usesSdk])).isBase)
    }

    // MARK: - Attributes as Android's installer reads them (XmlBlock.java:281-302, 313-320, 426-449, 484-530)

    static func refusal(_ data: Data) -> String? {
        do {
            _ = try Self.member(data)
            return nil
        } catch {
            return (error as? SetMemberError)?.reason ?? "\(error)"
        }
    }

    @Test func whatAndroidReadsOneWayIsReadAsItsInstallerDoes() throws {
        // Android reads package, split and configForSplit raw first everywhere (ParsingPackageUtils.java:702-703,
        // 751-752), and the split attributes by name alone (PackageImpl.java:2720-2736): a typed value beside them is no
        // second reading.
        let package = AXMLBuilder.Attribute(namespace: nil, name: "package", resourceID: nil, value: .string("org.example.typed"),
                                            raw: "org.example.raw")
        #expect(try Self.member(SplitAPK.make(package: nil, attributes: [package])).packageName == "org.example.raw")
        let emptyRaw = AXMLBuilder.Attribute(namespace: nil, name: "split", resourceID: nil, value: .string("config.arm64_v8a"),
                                             raw: "")
        #expect(try Self.member(SplitAPK.make(attributes: [emptyRaw])).isBase)
        #expect(try Self.member(SplitAPK.make(attributes: [.plain("split", .bool(true))])).splitName == "true")
        #expect(try Self.member(SplitAPK.make(attributes: [.plain("split", .typed(type: 0x04, data: 0x7FC0_0000))])).splitName
            == "NaN")
        let configForSplit = AXMLBuilder.Attribute(namespace: nil, name: "configForSplit", resourceID: nil, value: .string("feature"),
                                                   raw: "other")
        #expect(try Self.member(SplitAPK.make(attributes: [.plain("split", .string("feature.config.de")), configForSplit]))
            .configForSplit == "other")
        let color = AXMLBuilder.Attribute.android("isFeatureSplit", 0x0101_055B, .typed(type: 0x1C, data: 1))
        #expect(try Self.member(SplitAPK.make(attributes: [.plain("split", .string("dynamic")), color])).isFeatureSplit)
        let required = AXMLBuilder.Attribute(namespace: AXMLBuilder.androidNamespace, name: "requiredSplitTypes",
                                             resourceID: 0x0101_064E, value: .string("base__abi"), raw: "")
        #expect(try Self.member(SplitAPK.make(attributes: [required])).requiredSplitTypes.isEmpty)
        let types = AXMLBuilder.Attribute(namespace: AXMLBuilder.androidNamespace, name: "splitTypes", resourceID: 0x0101_064F,
                                          value: .string("base__abi"), raw: "base__density")
        #expect(try Self.member(SplitAPK.make(attributes: [types])).splitTypes == ["base__density"])
    }

    @Test(arguments: [
        // TypedArray.getInteger throws on a string (TypedArray.java:651), and Android's package parser fails.
        ([AXMLBuilder.Attribute.android("versionCode", 0x0101_021B, .string("5"))], "Android cannot read the manifest's versionCode"),
        ([AXMLBuilder.Attribute.android("versionCodeMajor", 0x0101_0576, .string("2"))],
         "Android cannot read the manifest's versionCodeMajor"),
        // The installer finds an attribute without a resource ID, the package parser does not: the library would key
        // what the guest does not see (ADR-0048 decision 2).
        ([AXMLBuilder.Attribute(namespace: AXMLBuilder.androidNamespace, name: "versionCodeMajor", resourceID: nil, value: .int(2))],
         "its manifest's versionCodeMajor reads as 2 and as 0"),
    ])
    func versionsAndroidReadsTwoWaysOrCannotReadAreRefused(attributes: [AXMLBuilder.Attribute], reason: String) {
        #expect(Self.refusal(SplitAPK.make(versionCode: attributes.contains { $0.name == "versionCode" } ? nil : 1,
                                           attributes: attributes))
            == "Not a valid APK: \(reason)")
    }

    @Test func anAttributeOnlyItsResourceIDFindsKeepsThatReadingForTheVersionsOnly() throws {
        // Obfuscators rename an attribute; Android's installer then misses it. Its package parser finds versionCode and
        // versionCodeMajor by resource ID (PackageImpl.java:2721-2723), and that reading stands (the decision).
        // Nothing reads the split attributes so: a renamed isFeatureSplit leaves a configuration split.
        let member = try Self.member(SplitAPK.make(versionCode: nil, split: "config.arm64_v8a", attributes: [
            .android("vc", 0x0101_021B, .int(7)), .android("vcm", 0x0101_0576, .int(1)), .android("fs", 0x0101_055B, .bool(true)),
            .android("rst", 0x0101_064E, .string("base__abi")), .android("st", 0x0101_064F, .string("feature__abi")),
        ]))
        #expect(member.versionCode == 1 << 32 | 7)
        #expect(!member.isFeatureSplit)
        #expect(member.role == .config(module: nil, suffix: "arm64_v8a"))
        #expect(member.requiredSplitTypes.isEmpty)
        #expect(member.splitTypes.isEmpty)
    }

    @Test func theManifestIsReadWithinTheAnalyzersLimit() {
        // 16 MiB, as APKAnalyzer reads it: a crafted string pool of a few hundred KB of deflated data could otherwise
        // cost about a gigabyte to decode. The entry claims one byte more in both its headers;
        // only that size is read before the refusal.
        var zip = ZipBuilder()
        zip.add("AndroidManifest.xml", AXMLBuilder().build(AXMLBuilder.Element("manifest", [.plain("package", .string(SplitAPK.package))], [])))
        var data = zip.build().data
        let claimed = UInt32(16 << 20) + 1
        for (signature, field) in [([0x50, 0x4B, 0x03, 0x04] as [UInt8], 22), ([0x50, 0x4B, 0x01, 0x02], 24)] {
            let header = data.firstRange(of: Data(signature))!.lowerBound
            data.replaceSubrange(header + field..<header + field + 4, with: withUnsafeBytes(of: claimed.littleEndian) { Data($0) })
        }
        let tooLarge = APKError.entryTooLarge("AndroidManifest.xml", UInt64(claimed))
        #expect(Self.refusal(data) == "Not a valid APK: \(tooLarge)")
    }

    @Test func androidsRefusalsComeBeforeTheReadingsAreCompared() throws {
        #expect(throws: SetMemberError(fileName: "x.apk", reason: "Invalid manifest split: bad character '5'")) {
            try Self.member(SplitAPK.make(attributes: [.plain("split", .int(5))]))
        }
        #expect(throws: SetMemberError(fileName: "x.apk", reason: "Invalid manifest split: bad character '#'")) {
            try Self.member(SplitAPK.make(attributes: [.plain("split", .typed(type: 0x1C, data: 0xFF00_0000))]))
        }
    }

    @Test func aTypedNullOrAMissingRawStringIsUnreadable() {
        // getAttributeValue throws for TYPE_NULL and for a raw string the pool cannot give; Android's parse fails.
        #expect(throws: SetMemberError(fileName: "x.apk", reason: "Not a valid APK: Android cannot read the manifest's split")) {
            try Self.member(SplitAPK.make(attributes: [.plain("split", .typed(type: 0x00, data: 0))]))
        }
        var pastThePool = AXMLBuilder.Attribute.plain("split", .bool(true))
        pastThePool.rawIndex = 0x7FFF_FFFF
        #expect(throws: SetMemberError(fileName: "x.apk", reason: "Not a valid APK: Android cannot read the manifest's split")) {
            try Self.member(SplitAPK.make(attributes: [pastThePool]))
        }
    }

    @Test func theVersionCodeIsUnsignedBelowItsMajor() throws {
        // PackageInfo.composeLongVersionCode.
        #expect(try Self.member(SplitAPK.make(versionCode: -1)).versionCode == 0xFFFF_FFFF)
    }

    @Test func flagsAndNumbersAreIntegerTypesOnly() throws {
        // getAttributeBooleanValue and getAttributeIntValue take any integer type's data, else the default.
        let string = AXMLBuilder.Attribute.android("isSplitRequired", 0x0101_0591, .string("true"))
        #expect(!(try Self.member(SplitAPK.make(attributes: [string])).splitRequired))
        let color = AXMLBuilder.Attribute.android("isSplitRequired", 0x0101_0591, .typed(type: 0x1C, data: 1))
        #expect(try Self.member(SplitAPK.make(attributes: [color])).splitRequired)
        // By namespace and name, never by resource ID alone.
        let bare = AXMLBuilder.Attribute(namespace: nil, name: "isSplitRequired", resourceID: 0x0101_0591, value: .bool(true))
        #expect(!(try Self.member(SplitAPK.make(attributes: [bare])).splitRequired))
    }

    @Test func anEmptySplitNameIsTheBase() throws {
        #expect(try Self.member(SplitAPK.make(split: "")).splitName == nil)
    }

    @Test(arguments: ["AndroidManifest.xml", "lib/arm64-v8a/libbad.so", "META-INF/CERT.RSA", "assets/bad.bin"],
          CraftedZIP.Offset.unreadable)
    func anEntryAndroidOpensButCannotReadFailsOnlyWhereItIsRead(name: String, offset: CraftedZIP.Offset) throws {
        // ADR-0048 b1.2: Android opens such an APK; its installer reads the manifest and the signatures, and a
        // library only when it extracts it (step b6).
        if name == "AndroidManifest.xml" {
            var zip = ZipBuilder()
            zip.add("assets/x", "x")
            let unreadable = APKError.inconsistentEntry(name, "its local header runs into the central directory")
            #expect(throws: SetMemberError(fileName: "x.apk", reason: "Not a valid APK: \(unreadable)")) {
                try Self.member(CraftedZIP.adding(name, at: offset, to: zip.build().data))
            }
            return
        }
        let member = try Self.member(CraftedZIP.adding(name, at: offset, to: SplitAPK.make(signers: [])))
        #expect(member.packageName == SplitAPK.package)
        #expect(member.abis == (name.hasPrefix("lib/") ? ["arm64-v8a"] : []))
    }

    @Test func groupsAUnitsAPKsByPackageAndVersion() throws {
        // ADR-0048 decision 2: each group is a set, in the order of its first APK.
        let a = try Self.member(SplitAPK.make(package: "org.example.a"), named: "a.apk")
        let b = try Self.member(SplitAPK.make(package: "org.example.b"), named: "b.apk")
        let newerA = try Self.member(SplitAPK.make(package: "org.example.a", versionCode: 2), named: "a-2.apk")
        let a2 = try Self.member(SplitAPK.make(package: "org.example.a", split: "config.de"), named: "a2.apk")
        #expect(SetGrouping.byPackageAndVersion([a, b, newerA, a2]) == [[a, a2], [b], [newerA]])
    }
}
