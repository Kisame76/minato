import APKKit
import Foundation

/// One APK of an import unit, as Android 15's installer reads it for a session (ADR-0048, decision 2):
/// `ApkLiteParseUtils.parseApkLite` at android-15.0.0_r1, which finds each attribute by namespace and name.
public struct SetMember: Sendable, Equatable {
    /// The name the APK came with, for the reasons.
    public var fileName: String
    public var packageName: String
    /// `versionCodeMajor` in the upper 32 bits, as the session compares it. The installer reads both by name, where
    /// an update's check reads the package parser's, by resource ID (`AndroidManifest.versionCode`).
    public var versionCode: Int64
    /// nil for the base.
    public var splitName: String?
    public var configForSplit: String?
    public var isFeatureSplit: Bool
    /// The first `<uses-split>`; Android ignores the others.
    public var usesSplit: String?
    public var requiredSplitTypes: Set<String>
    public var splitTypes: Set<String>
    /// `<manifest android:isSplitRequired>`, which Android 15 reads.
    public var splitRequired: Bool
    /// What bundletool wrote and Android 15 ignores: `<application android:isSplitRequired>` or the meta-data
    /// `com.android.vending.splits.required` (F1). Minato reads them as "Needs split APKs".
    public var stricterSplitSignal: Bool
    public var targetSdk: Int
    /// The base's check uses its targetSdk, a split's none, so a split may be signed with v1 alone. Android checks
    /// the minimum scheme on the session's signer, which comes from the first APK it stages, in no fixed order
    /// (`PackageInstallerSession.java:1541, 3501-3503`); Minato's rule is that order with the base first.
    public var signature: SignatureVerification
    /// The SHA-256 of each signer's certificate: as the check proved them, in the APK's order and as many as its
    /// signers, when verified; as the APK names them, unproven but as many, when Minato cannot verify it
    /// (`APKSignatures.signerCertificates`); none when unsigned.
    public var certificates: [String]
    public var sha256: String
    /// The ABIs with a `lib/<abi>/lib*.so`, sorted.
    public var abis: [String]

    public var isBase: Bool { splitName == nil }

    public var role: SplitRole {
        guard let name = splitName else { return .base }
        if isFeatureSplit { return .feature(name) }
        if let module = configForSplit {
            let prefix = module.isEmpty ? "config." : module + ".config."
            return .config(module: module.isEmpty ? nil : module,
                           suffix: name.hasPrefix(prefix) ? String(name.dropFirst(prefix.count)) : name)
        }
        if name.hasPrefix("config.") { return .config(module: nil, suffix: String(name.dropFirst("config.".count))) }
        if let separator = name.range(of: ".config.") {
            return .config(module: String(name[..<separator.lowerBound]), suffix: String(name[separator.upperBound...]))
        }
        return .other(name)
    }

    static let androidNamespace = "http://schemas.android.com/apk/res/android"
    /// Android's limit for a name it checks as a file name (`FrameworkParsingPackageUtils.java:58`).
    static let maxNameLength = 223

    /// Reads the APK of `archive`; refuses what Android's parser refuses, with its message, in its order
    /// (`ApkLiteParseUtils.java:406-640, 685-773`).
    public static func read(_ archive: ZipArchive, fileName: String, sha256: String) throws -> SetMember {
        func refusal(_ reason: String) -> SetMemberError { SetMemberError(fileName: fileName, reason: reason) }
        let root: APKKit.XMLElement
        let manifest: AndroidManifest
        do {
            root = try BinaryXML.parse(archive.data(named: "AndroidManifest.xml"))
            manifest = try AndroidManifest(xml: root)
        } catch {
            throw refusal("Not a valid APK: \(error)")
        }

        let attribute = InstallerAttribute(root)
        // Android's installer finds an android: attribute by its name; its package parser, as the analyzer, by its
        // resource ID (`obtainAttributes`). Where obfuscation blanked the name, only the resource ID finds it, and that
        // reading stands (the lead's decision); where the name finds it, the two readings must agree, below.
        func byName(_ name: String) -> Bool { attribute.find(name, android: true) != nil }
        func byResourceID(_ name: String, _ id: UInt32) -> XMLAttribute? {
            root.attribute(resourceID: id) ?? root.attribute(named: name, namespace: androidNamespace)
        }
        let typesIDs: [String: UInt32] = ["requiredSplitTypes": 0x0101_064E, "splitTypes": 0x0101_064F]
        func readTypes(_ name: String) throws -> String? {
            try byName(name) ? attribute.text(name, android: true) : byResourceID(name, typesIDs[name]!).flatMap(Self.typedText)
        }
        let packageName: String
        let splitText: String?, splitName: String?
        let requiredText: String?, typesText: String?
        let requiredSplitTypes: Set<String>, splitTypes: Set<String>
        let configForSplit: String?
        var usesSplit: String?
        do {
            packageName = try attribute.text("package") ?? ""
            if packageName != "android",
               let problem = validateName(packageName, requireSeparator: true, requireFilename: true) {
                throw refusal("Invalid manifest package: \(problem)")
            }
            splitText = try attribute.text("split")
            splitName = splitText?.isEmpty == true ? nil : splitText
            if let splitName, let problem = validateName(splitName, requireSeparator: false, requireFilename: false) {
                throw refusal("Invalid manifest split: \(problem)")
            }
            requiredText = try readTypes("requiredSplitTypes")
            requiredSplitTypes = try Self.splitTypes(requiredText, refusal: refusal)
            typesText = try readTypes("splitTypes")
            splitTypes = try Self.splitTypes(typesText, refusal: refusal)
            configForSplit = try attribute.text("configForSplit")
            if let element = root.children(named: "uses-split").first {
                guard let name = try InstallerAttribute(element).text("name", android: true) else {
                    throw refusal("<uses-split> tag requires 'android:name' attribute")
                }
                usesSplit = name
            }
        } catch let unreadable as InstallerAttribute.Unreadable {
            throw refusal("Not a valid APK: Android cannot read the manifest's \(unreadable.name)")
        }
        // A split's levels only refuse it (:569-625); a base's are the analyzer's, as for one APK (ADR-0039).
        if splitName != nil, let sdkRefusal = manifest.sdkRefusal {
            throw refusal(sdkRefusal)
        }
        let analyzerMinor = Int64(UInt32(truncatingIfNeeded: manifest.versionCode)), analyzerMajor = manifest.versionCode >> 32
        let minor = byName("versionCode") ? Int64(UInt32(bitPattern: attribute.integer("versionCode") ?? 0)) : analyzerMinor
        let major = byName("versionCodeMajor") ? Int64(attribute.integer("versionCodeMajor") ?? 0) : analyzerMajor
        let versionCode = major << 32 | minor
        let isFeatureSplit = byName("isFeatureSplit")
            ? attribute.integer("isFeatureSplit").map { $0 != 0 } ?? false : manifest.isFeatureSplit

        // The analyzer reads the typed value first and android: attributes by resource ID (`AndroidManifest`). Where
        // the two readings differ, the library would key what these checks did not see (the lead's decision).
        func agree(_ name: String, _ installer: String, _ analyzer: String) throws {
            if installer != analyzer {
                throw refusal("Not a valid APK: its manifest's \(name) reads as \(installer) and as \(analyzer)")
            }
        }
        func quoted(_ text: String?) -> String { text.map { "“\($0)”" } ?? "nothing" }
        try agree("package", quoted(packageName), quoted(manifest.packageName))
        try agree("split", quoted(splitText), quoted(root.attribute(named: "split").flatMap(Self.typedText)))
        try agree("configForSplit", quoted(configForSplit),
                  quoted(root.attribute(named: "configForSplit").flatMap(Self.typedText)))
        // Where only the resource ID finds an attribute, its reading is the analyzer's, so these agree by themselves.
        try agree("versionCode", "\(minor)", "\(analyzerMinor)")
        try agree("versionCodeMajor", "\(major)", "\(analyzerMajor)")
        try agree("isFeatureSplit", "\(isFeatureSplit)", "\(manifest.isFeatureSplit)")
        for (name, text) in [("requiredSplitTypes", requiredText), ("splitTypes", typesText)] {
            try agree(name, quoted(text), quoted(byResourceID(name, typesIDs[name]!).flatMap(Self.typedText)))
        }

        let application = root.children(named: "application").first
        // Minato's own reading of bundletool's signals, by resource ID or name as APKKit reads attributes.
        let applicationFlag = application.flatMap {
            $0.attribute(resourceID: 0x0101_0591) ?? $0.attribute(named: "isSplitRequired", namespace: androidNamespace)
        }
        let metaData: Bool = switch manifest.metaData["com.android.vending.splits.required"] {
        case .boolean(let flag): flag
        case .string(let text): text.lowercased() == "true"
        default: false
        }

        let abis = Set(archive.entries.compactMap { entry -> String? in
            let parts = entry.name.split(separator: "/", omittingEmptySubsequences: false)
            guard parts.count == 3, parts[0] == "lib", !parts[1].isEmpty,
                  parts[2].hasPrefix("lib"), parts[2].hasSuffix(".so") else { return nil }
            return String(parts[1])
        })

        let signature = SignatureVerifier.verify(archive, targetSdk: splitName == nil ? manifest.targetSdk : 0)
        let certificates: [String] = switch signature.check {
        case .verified: signature.proof?.certificates.map(SigningCertificate.sha256(of:)) ?? []
        case .unverified: APKSignatures.signerCertificates(archive)
        default: []
        }
        return SetMember(
            fileName: fileName, packageName: packageName, versionCode: versionCode, splitName: splitName,
            configForSplit: configForSplit,
            isFeatureSplit: isFeatureSplit,
            usesSplit: usesSplit, requiredSplitTypes: requiredSplitTypes, splitTypes: splitTypes,
            splitRequired: attribute.integer("isSplitRequired").map { $0 != 0 } ?? false,
            stricterSplitSignal: Self.flag(applicationFlag) || metaData, targetSdk: manifest.targetSdk,
            signature: signature, certificates: certificates, sha256: sha256, abis: abis.sorted())
    }

    /// `FrameworkParsingPackageUtils.validateName` (:72-103): dot-separated parts, each a letter, then letters, digits
    /// or '_'; as a file name, at most 223 characters; with a separator, at least two parts.
    static func validateName(_ name: String, requireSeparator: Bool, requireFilename: Bool) -> String? {
        var hasSeparator = false
        var front = true
        for character in name.unicodeScalars {
            if ("a"..."z").contains(character) || ("A"..."Z").contains(character) {
                front = false
            } else if !front && (("0"..."9").contains(character) || character == "_") {
                continue
            } else if character == "." {
                hasSeparator = true
                front = true
            } else {
                return "bad character '\(character)'"
            }
        }
        if requireFilename {
            if !isValidExtFilename(name) { return "Invalid filename" }
            if name.utf16.count > maxNameLength { return "the length of the name is greater than \(maxNameLength)" }
        }
        return hasSeparator || !requireSeparator ? nil : "must have at least one '.' separator"
    }

    /// `FileUtils.isValidExtFilename`: not empty, `.` or `..`, no NUL or '/', and at most 255 bytes of UTF-8.
    static func isValidExtFilename(_ name: String) -> Bool {
        !name.isEmpty && name != "." && name != ".." && !name.contains("\0") && !name.contains("/") && name.utf8.count <= 255
    }

    /// `requiredSplitTypes` or `splitTypes` (`separateAndValidateSplitTypes`, :757-773): Java's trim and split(","),
    /// which drops trailing empty parts, then each part trimmed and checked as a name and a file name.
    static func splitTypes(_ value: String?, refusal: (String) -> SetMemberError) throws -> Set<String> {
        guard let value, !value.isEmpty else { return [] }
        var parts = javaTrim(value).split(separator: ",", omittingEmptySubsequences: false).map(String.init)
        if parts.count > 1 {
            while parts.last?.isEmpty == true { parts.removeLast() }
        }
        var types: Set<String> = []
        for part in parts {
            let type = javaTrim(part)
            if let problem = validateName(type, requireSeparator: false, requireFilename: true) {
                throw refusal("Invalid manifest split types: \(problem)")
            }
            types.insert(type)
        }
        return types
    }

    /// `String.trim`: drops the characters up to U+0020 at both ends.
    static func javaTrim(_ text: String) -> String {
        let scalars = text.unicodeScalars
        guard let start = scalars.firstIndex(where: { $0.value > 0x20 }),
              let end = scalars.lastIndex(where: { $0.value > 0x20 }) else { return "" }
        return String(scalars[start...end])
    }

    /// The analyzer's text of an attribute: the typed string, else the raw string (APKKit's `XMLAttribute.string`).
    static func typedText(_ attribute: XMLAttribute) -> String? {
        if case .string(let text) = attribute.value { return text }
        return attribute.rawString
    }

    static func flag(_ attribute: XMLAttribute?) -> Bool {
        switch attribute?.value {
        case .boolean(let flag): flag
        case .integer(let number): number != 0
        case .string(let text): text == "true"
        default: false
        }
    }
}

/// An element's attributes as Android's installer reads them (`XmlBlock.Parser`, android-15.0.0_r1): the first
/// attribute of a namespace and name (`ResXMLParser::indexOfAttribute`, `XmlBlock.java:313-320, 426-449`), never one
/// found by resource ID alone. `UsesSdk` reads `<uses-sdk>` the same way.
struct InstallerAttribute {
    /// A value Android's parse throws on, failing the file.
    struct Unreadable: Error {
        var name: String
    }

    var element: APKKit.XMLElement

    init(_ element: APKKit.XMLElement) {
        self.element = element
    }

    func find(_ name: String, android: Bool) -> XMLAttribute? {
        element.attribute(named: name, namespace: android ? SetMember.androidNamespace : nil)
    }

    /// `getAttributeValue` (:281-302): the raw string, else the typed value as `TypedValue.coerceToString` writes it.
    /// A raw string the pool cannot give and TYPE_NULL throw there.
    func text(_ name: String, android: Bool = false) throws -> String? {
        guard let attribute = find(name, android: android) else { return nil }
        if attribute.rawIndex < 0x8000_0000 {
            guard let raw = attribute.rawString else { throw Unreadable(name: name) }
            return raw
        }
        let data = attribute.data
        let value = Int32(bitPattern: data)
        switch attribute.dataType {
        case 0x00:
            throw Unreadable(name: name)
        case 0x01, 0x07:
            return "@\(value)"
        case 0x02:
            return "?\(value)"
        case 0x04:
            return Self.javaText(Float(bitPattern: data))
        case 0x05, 0x06:
            // A unit without a name (DIMENSION_UNIT_STRS, FRACTION_UNIT_STRS) throws there.
            let units = attribute.dataType == 0x05 ? ["px", "dip", "sp", "pt", "in", "mm"] : ["%", "%p"]
            let unit = Int(data & 0xF)
            guard unit < units.count else { throw Unreadable(name: name) }
            let mantissa = Float(Int32(bitPattern: data & 0xFFFF_FF00))
            let scale: Float = [0x1p-8, 0x1p-15, 0x1p-23, 0x1p-31][Int(data >> 4 & 0x3)]
            return Self.javaText(mantissa * scale * (attribute.dataType == 0x06 ? 100 : 1)) + units[unit]
        case 0x11:
            return "0x" + String(data, radix: 16)
        case 0x12:
            return data != 0 ? "true" : "false"
        case 0x1C...0x1F:
            return "#" + String(data, radix: 16)
        case 0x10...0x1F:
            return String(value)
        default:
            return nil
        }
    }

    /// `getAttributeIntValue` and `getAttributeBooleanValue` (:484-530): an integer type's data, else nil for the
    /// default.
    func integer(_ name: String) -> Int32? {
        guard let attribute = find(name, android: true), (0x10...0x1F).contains(attribute.dataType) else { return nil }
        return Int32(bitPattern: attribute.data)
    }

    /// `Float.toString` where the name check sees a difference from Swift's text: NaN and the infinities. Any other
    /// float starts with a digit or '-' in both, which the check refuses at that character.
    static func javaText(_ number: Float) -> String {
        if number.isNaN { return "NaN" }
        if number.isInfinite { return number < 0 ? "-Infinity" : "Infinity" }
        return String(describing: number)
    }
}

/// How an APK takes part in its set (ADR-0048, decision 2), from its manifest, never its file name.
public enum SplitRole: Sendable, Equatable {
    case base
    case feature(String)
    /// A configuration split of the base (`module` nil) or of a feature module; `suffix` names its configuration.
    case config(module: String?, suffix: String)
    /// A split that is neither: Android loads it like any other.
    case other(String)
}

/// An APK that Android's parser refuses, with its message.
public struct SetMemberError: Error, Sendable, Equatable {
    public var fileName: String
    public var reason: String

    public init(fileName: String, reason: String) {
        self.fileName = fileName
        self.reason = reason
    }
}

/// A unit's APKs grouped by package and version code, in the order of each group's first APK (ADR-0048, decision 2).
/// Android's session takes one package and version; a unit may hold several apps or versions, each a set of its own.
public enum SetGrouping {
    public static func byPackageAndVersion(_ members: [SetMember]) -> [[SetMember]] {
        struct Key: Hashable {
            var packageName: String
            var versionCode: Int64
        }
        var groups: [[SetMember]] = []
        var index: [Key: Int] = [:]
        for member in members {
            let key = Key(packageName: member.packageName, versionCode: member.versionCode)
            if let position = index[key] {
                groups[position].append(member)
            } else {
                index[key] = groups.count
                groups.append([member])
            }
        }
        return groups
    }
}
