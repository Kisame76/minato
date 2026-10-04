import Foundation

/// An element's attributes as Android 15's installer reads them (`XmlBlock.Parser`, android-15.0.0_r1): the first
/// attribute of a namespace and name (`ResXMLParser::indexOfAttribute`, `XmlBlock.java:313-320, 426-449`), never one
/// found by resource ID alone. `AndroidManifest` and `UsesSdk` read the installer's attributes through it.
public struct InstallerReading {
    /// A value Android's parse throws on, failing the file.
    public struct Unreadable: Error, Equatable {
        public var name: String
    }

    public var element: XMLElement

    public init(_ element: XMLElement) {
        self.element = element
    }

    public func find(_ name: String, android: Bool) -> XMLAttribute? {
        element.attribute(named: name, namespace: android ? AndroidAttribute.namespace : nil)
    }

    /// `getAttributeValue` (:281-302): the raw string, else the typed value as `TypedValue.coerceToString` writes it.
    /// A raw string the pool cannot give and TYPE_NULL throw there.
    public func text(_ name: String, android: Bool = false) throws -> String? {
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
            // A unit without a name (DIMENSION_UNIT_STRS, FRACTION_UNIT_STRS) throws there. TypedValue.complexToFloat:
            // the signed mantissa above bit 8, scaled by the radix in bits 4 and 5.
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

    /// `getAttributeIntValue` and `getAttributeBooleanValue` (:484-530) of an android: attribute: an integer type's
    /// data, else nil for the default.
    public func integer(_ name: String) -> Int32? {
        guard let attribute = find(name, android: true), (0x10...0x1F).contains(attribute.dataType) else { return nil }
        return Int32(bitPattern: attribute.data)
    }

    /// `Float.toString` where it differs from Swift's text: NaN and the infinities.
    static func javaText(_ number: Float) -> String {
        if number.isNaN { return "NaN" }
        if number.isInfinite { return number < 0 ? "-Infinity" : "Infinity" }
        return String(describing: number)
    }

    // MARK: - Names

    /// Android's limit for a name it checks as a file name (`FrameworkParsingPackageUtils.java:58`).
    static let maxNameLength = 223

    /// `FrameworkParsingPackageUtils.validateName` (:72-103): dot-separated parts, each a letter, then letters, digits
    /// or '_'; as a file name, at most 223 characters; with a separator, at least two parts. Android's problem, or nil.
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

    /// `FileUtils.isValidExtFilename`: not empty, `.` or `..`, no NUL or '/', and at most 255 bytes of UTF-8. Java
    /// checks each UTF-16 unit, so a combining mark after '/' hides nothing.
    public static func isValidExtFilename(_ name: String) -> Bool {
        !name.isEmpty && name != "." && name != ".." && !name.unicodeScalars.contains { $0 == "\0" || $0 == "/" }
            && name.utf8.count <= 255
    }

    /// `requiredSplitTypes` or `splitTypes` (`separateAndValidateSplitTypes`, ApkLiteParseUtils.java:757-773): Java's
    /// trim and split(","), which drops trailing empty parts, then each part trimmed and checked as a name and a file
    /// name. Android's problem refuses the manifest.
    static func splitTypes(_ value: String?) throws -> Set<String> {
        guard let value, !value.isEmpty else { return [] }
        var parts = javaTrim(value).split(separator: ",", omittingEmptySubsequences: false).map(String.init)
        if parts.count > 1 {
            while parts.last?.isEmpty == true { parts.removeLast() }
        }
        var types: Set<String> = []
        for part in parts {
            let type = javaTrim(part)
            if let problem = validateName(type, requireSeparator: false, requireFilename: true) {
                throw APKError.invalidManifest("Invalid manifest split types: \(problem)")
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
}
