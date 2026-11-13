import Foundation

/// `<uses-sdk>` as Android 15 installs an APK, which reads it twice:
/// - the installer's own parse first (ApkLiteParseUtils.java:569-625). It finds a level by namespace and name, and reads
///   it as text: the raw string, else the typed value as TypedValue.coerceToString writes it (XmlBlock.java:281-301).
///   Integer.parseInt makes the text a number, and any other text is a codename. That the install runs this parse is
///   assumed: PackageInstallerSession and pm are in services/, which is not checked out;
/// - then the package parser's (ParsingPackageUtils.java:1640-1720). It finds a level by resource ID, as
///   RetrieveAttributes walks the attributes (AttributeResolution.cpp:352-420), and reads it as a TypedArray: a string
///   is a codename, TYPE_NULL and @null are absent, and any other type gives its data.
///
/// Either parse refuses a codename, as a release build does, and a minSdk above 35 (FrameworkParsingPackageUtils.java:
/// 298-330, 363-397). The levels are the package parser's. Where it would resolve a reference, Minato stops, a logged
/// departure (ADR-0039): only a crafted file, with a numeric raw string beside the reference, gets past the first parse.
struct UsesSdk: Equatable {
    var minSdk = 1
    var targetSdk = 0
    /// Android's message when either parse refuses the levels.
    var refusal: String?
    /// The attribute whose reference the package parser would resolve.
    var reference: String?

    private static let minName = "minSdkVersion"
    private static let targetName = "targetSdkVersion"
    private static let minID: UInt32 = 0x0101_020C
    private static let targetID: UInt32 = 0x0101_0270

    /// A level as either parse reads it.
    private enum Level: Equatable {
        case absent
        case number(Int)
        case codename(String)
        /// A resource ID, which only the package parser would resolve.
        case reference(Int)
    }

    /// Each parse takes every `<uses-sdk>` in turn and stops at its first refusal; the last levels count.
    init(manifest root: XMLElement) throws {
        let elements = root.children(named: "uses-sdk")
        for element in elements {
            let read = Self.levels(min: try Self.installerLevel(element, Self.minName),
                                   target: try Self.installerLevel(element, Self.targetName))
            if let refusal = read.refusal {
                self.refusal = refusal
                break
            }
        }
        for element in elements {
            let attributes = Self.retrieve(element)
            let min: Level, target: Level
            do {
                (min, target) = (try Self.packageParserLevel(attributes.min), try Self.packageParserLevel(attributes.target))
            } catch where refusal != nil {
                // The installer refused first, so the package parser never reads it.
                break
            }
            let read = Self.levels(min: min, target: target)
            minSdk = read.min
            targetSdk = read.target
            guard read.refusal == nil, read.reference == nil else {
                if refusal == nil {
                    refusal = read.refusal
                    reference = read.reference
                }
                break
            }
        }
    }

    /// parseUsesSdk with what one parse read (ApkLiteParseUtils.java:576-601, ParsingPackageUtils.java:1648-1680): the
    /// levels, a codename's at CUR_DEVELOPMENT, and the verdicts of computeTargetSdkVersion, then computeMinSdkVersion.
    /// A reference stops Minato where a verdict needs its value.
    private static func levels(min: Level, target: Level) -> (min: Int, target: Int, refusal: String?, reference: String?) {
        var minVersion = 1
        var minCode: String?
        var minAssigned = false
        switch min {
        case .absent:
            break
        case .number(let level), .reference(let level):
            minVersion = level
            minAssigned = true
        case .codename(let code):
            minCode = code
            minAssigned = !code.isEmpty
        }
        var targetVersion = 0
        var targetCode: String?
        switch target {
        case .absent:
            targetVersion = minVersion
            targetCode = minCode
        case .number(let level), .reference(let level):
            targetVersion = level
        case .codename(let code):
            targetCode = code
            if !minAssigned { minCode = code }
        }
        let development = AndroidManifest.developmentSdk
        let levels = (min: minCode == nil ? minVersion : development, target: targetCode == nil ? targetVersion : development)

        if case .reference = target {
            return (levels.min, levels.target, nil, targetName)
        }
        if case .reference = min, case .absent = target {
            return (levels.min, levels.target, nil, minName)
        }
        if let targetCode {
            return (levels.min, levels.target, codename(targetCode), nil)
        }
        if case .reference = min {
            return (levels.min, levels.target, nil, minName)
        }
        if let minCode {
            return (levels.min, levels.target, codename(minCode), nil)
        }
        let platform = Int(APKSignatures.platformSDK)
        if minVersion > platform {
            return (levels.min, levels.target, "Requires newer sdk version #\(minVersion) (current version is #\(platform))", nil)
        }
        return (levels.min, levels.target, nil, nil)
    }

    private static func codename(_ code: String) -> String {
        "Requires development platform \(code) but this is a release platform."
    }

    // MARK: - The installer's parse

    /// XmlBlock.getAttributeValue(namespace, name), as `InstallerReading` reads it: the first attribute of that
    /// namespace and name, as text. What throws there, a raw string the pool cannot give (StringBlock.java:106-126),
    /// TYPE_NULL or a unit without a name, makes Android refuse the file as unparsable (ApkLiteParseUtils.java:390).
    private static func installerLevel(_ element: XMLElement, _ name: String) throws -> Level {
        let text: String?
        do {
            text = try InstallerReading(element).text(name, android: true)
        } catch {
            throw APKError.invalidManifest("<uses-sdk>'s \(name) is a value Android's installer cannot read")
        }
        guard let text, !text.isEmpty else { return .absent }
        return parseInt(text).map(Level.number) ?? .codename(text)
    }

    /// Integer.parseInt(text, 10): an optional sign, then decimal digits of any script, as Character.digit takes each
    /// UTF-16 unit, within Int32.
    static func parseInt(_ text: String) -> Int? {
        let units = Array(text.utf16)
        guard let first = units.first else { return nil }
        var index = 0
        var negative = false
        if first < 0x30 {
            guard first == 0x2D || first == 0x2B, units.count > 1 else { return nil }
            negative = first == 0x2D
            index = 1
        }
        let limit = negative ? -Int64(Int32.min) : Int64(Int32.max)
        var result: Int64 = 0
        for unit in units[index...] {
            guard let scalar = Unicode.Scalar(unit), scalar.properties.numericType == .decimal,
                  let digit = scalar.properties.numericValue else { return nil }
            result = result * 10 + Int64(digit)
            guard result <= limit else { return nil }
        }
        return Int(negative ? -result : result)
    }

    // MARK: - The package parser's

    /// The attributes RetrieveAttributes gives TypedArray for the two levels: one walk in the attributes' order, which
    /// skips those with a lower resource ID (or none) and takes one with the requested ID.
    private static func retrieve(_ element: XMLElement) -> (min: XMLAttribute?, target: XMLAttribute?) {
        let attributes = element.attributes
        var found: [XMLAttribute?] = []
        var index = 0
        for requested in [minID, targetID] {
            while index < attributes.count, requested > attributes[index].resourceID ?? 0 {
                index += 1
            }
            if index < attributes.count, requested == attributes[index].resourceID {
                found.append(attributes[index])
                index += 1
            } else {
                found.append(nil)
            }
        }
        return (found[0], found[1])
    }

    /// A level as TypedArray.peekValue gives it: TYPE_NULL and @null are absent, a string is a codename, a reference
    /// its resource ID, and any other type its data. A string the pool cannot give throws there (TypedArray.java:1389,
    /// 1395-1404), and Android refuses the file as unparsable.
    private static func packageParserLevel(_ attribute: XMLAttribute?) throws -> Level {
        guard let attribute else { return .absent }
        let number = Int(Int32(bitPattern: attribute.data))
        switch attribute.dataType {
        case 0x00:
            return .absent
        case 0x01, 0x07:
            return attribute.data == 0 ? .absent : .reference(number)
        case 0x03:
            guard case .string(let text) = attribute.value else {
                throw APKError.invalidManifest("<uses-sdk>'s \(attribute.name) is a string Android's package parser cannot read")
            }
            return .codename(text)
        default:
            return .number(number)
        }
    }
}
