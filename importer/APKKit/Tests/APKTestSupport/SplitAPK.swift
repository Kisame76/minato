import Foundation

/// Base and split APKs of one app for the split-set tests (ADR-0048): the manifest attributes Android's installer
/// reads, with libraries under `lib/<abi>/`, signed for real with `signers` (none: unsigned).
public enum SplitAPK {
    public static let package = "org.example.sets"

    /// `split` nil makes the base. `manifestSplitRequired` is `<manifest android:isSplitRequired>`, which Android 15
    /// reads; `applicationSplitRequired` and `splitsRequiredMetaData` are what bundletool wrote (ADR-0048, F1).
    /// `attributes` follow the others on `<manifest>` and `elements` lead its children, e.g. to craft a value; a nil
    /// `package` or `versionCode` leaves the attribute out. `nativeActivity` makes NativeActivity the launcher, loading
    /// that library, and `extractNativeLibs` sets `<application android:extractNativeLibs>`. `v2`, when given, signs in
    /// place of `signers`, e.g. a signer that names another key's certificate.
    public static func make(package: String? = SplitAPK.package, versionCode: Int32? = 1, versionCodeMajor: Int32? = nil,
                            split: String? = nil, configForSplit: String? = nil, isFeatureSplit: Bool = false,
                            manifestSplitRequired: Bool = false, applicationSplitRequired: Bool = false,
                            splitsRequiredMetaData: Bool = false, requiredSplitTypes: String? = nil,
                            splitTypes: String? = nil, usesSplit: String? = nil, targetSdk: Int32? = nil,
                            nativeActivity: String? = nil, extractNativeLibs: Bool? = nil, abis: [String] = [],
                            files: [String: Data] = [:], attributes: [AXMLBuilder.Attribute] = [],
                            elements: [AXMLBuilder.Element] = [], signers: [TestKey] = [.ec], v1Only: Bool = false,
                            v2: [TestSigner.Signer]? = nil) -> Data {
        var manifestAttributes: [AXMLBuilder.Attribute] = package.map { [.plain("package", .string($0))] } ?? []
        if let split { manifestAttributes.append(.plain("split", .string(split))) }
        if let configForSplit { manifestAttributes.append(.plain("configForSplit", .string(configForSplit))) }
        if let versionCode { manifestAttributes.append(.android("versionCode", 0x0101_021B, .int(versionCode))) }
        if let versionCodeMajor { manifestAttributes.append(.android("versionCodeMajor", 0x0101_0576, .int(versionCodeMajor))) }
        if isFeatureSplit { manifestAttributes.append(.android("isFeatureSplit", 0x0101_055B, .bool(true))) }
        if manifestSplitRequired { manifestAttributes.append(.android("isSplitRequired", 0x0101_0591, .bool(true))) }
        if let requiredSplitTypes {
            manifestAttributes.append(.android("requiredSplitTypes", 0x0101_064E, .string(requiredSplitTypes)))
        }
        if let splitTypes { manifestAttributes.append(.android("splitTypes", 0x0101_064F, .string(splitTypes))) }
        manifestAttributes += attributes

        var children = elements
        if let targetSdk {
            children.append(AXMLBuilder.Element("uses-sdk", [.android("targetSdkVersion", 0x0101_0270, .int(targetSdk))]))
        }
        if let usesSplit {
            children.append(AXMLBuilder.Element("uses-split", [.android("name", 0x0101_0003, .string(usesSplit))]))
        }
        var applicationAttributes: [AXMLBuilder.Attribute] = [.android("hasCode", 0x0101_000C, .bool(false))]
        if applicationSplitRequired { applicationAttributes.append(.android("isSplitRequired", 0x0101_0591, .bool(true))) }
        if let extractNativeLibs {
            applicationAttributes.append(.android("extractNativeLibs", 0x0101_04EA, .bool(extractNativeLibs)))
        }
        var applicationChildren: [AXMLBuilder.Element] = []
        if splitsRequiredMetaData {
            applicationChildren.append(AXMLBuilder.Element("meta-data", [
                .android("name", 0x0101_0003, .string("com.android.vending.splits.required")),
                .android("value", 0x0101_0024, .bool(true)),
            ]))
        }
        if let nativeActivity {
            let name = { (value: String) in AXMLBuilder.Attribute.android("name", 0x0101_0003, .string(value)) }
            applicationChildren.append(AXMLBuilder.Element("activity", [name("android.app.NativeActivity")], [
                AXMLBuilder.Element("meta-data", [name("android.app.lib_name"),
                                                  .android("value", 0x0101_0024, .string(nativeActivity))]),
                AXMLBuilder.Element("intent-filter", [], [
                    AXMLBuilder.Element("action", [name("android.intent.action.MAIN")]),
                    AXMLBuilder.Element("category", [name("android.intent.category.LAUNCHER")]),
                ]),
            ]))
        }
        children.append(AXMLBuilder.Element("application", applicationAttributes, applicationChildren))

        var zip = ZipBuilder()
        zip.add("AndroidManifest.xml", AXMLBuilder().build(AXMLBuilder.Element("manifest", manifestAttributes, children)))
        for abi in abis {
            zip.add("lib/\(abi)/libsets.so", ELFBuilder(segments: [.load(align: 0x4000)]).build())
        }
        for (name, content) in files.sorted(by: { $0.key < $1.key }) {
            zip.add(name, content)
        }
        if let v2 { return TestSigner.sign(zip, v2: v2) }
        if signers.isEmpty { return zip.build().data }
        if v1Only { return TestSigner.sign(zip, v1: TestSigner.V1(signers)) }
        return TestSigner.sign(zip, v2: signers.map { TestSigner.Signer($0) })
    }
}
