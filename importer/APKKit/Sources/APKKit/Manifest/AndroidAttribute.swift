import Foundation

/// A framework attribute (`android:` namespace). The resource ID is what Android itself matches on,
/// so it still works when obfuscators blank the attribute names in the string pool.
struct AndroidAttribute: Sendable {
    let name: String
    let id: UInt32?

    static let namespace = "http://schemas.android.com/apk/res/android"

    // IDs from android.R.attr.
    static let label = AndroidAttribute(name: "label", id: 0x0101_0001)
    static let icon = AndroidAttribute(name: "icon", id: 0x0101_0002)
    static let name = AndroidAttribute(name: "name", id: 0x0101_0003)
    static let hasCode = AndroidAttribute(name: "hasCode", id: 0x0101_000C)
    static let enabled = AndroidAttribute(name: "enabled", id: 0x0101_000E)
    static let debuggable = AndroidAttribute(name: "debuggable", id: 0x0101_000F)
    static let screenOrientation = AndroidAttribute(name: "screenOrientation", id: 0x0101_001E)
    static let configChanges = AndroidAttribute(name: "configChanges", id: 0x0101_001F)
    static let value = AndroidAttribute(name: "value", id: 0x0101_0024)
    static let drawable = AndroidAttribute(name: "drawable", id: 0x0101_0199)
    static let resource = AndroidAttribute(name: "resource", id: 0x0101_0025)
    static let targetActivity = AndroidAttribute(name: "targetActivity", id: 0x0101_0202)
    static let minSdkVersion = AndroidAttribute(name: "minSdkVersion", id: 0x0101_020C)
    static let maxSdkVersion = AndroidAttribute(name: "maxSdkVersion", id: 0x0101_0271)
    static let versionCode = AndroidAttribute(name: "versionCode", id: 0x0101_021B)
    static let versionName = AndroidAttribute(name: "versionName", id: 0x0101_021C)
    static let targetSdkVersion = AndroidAttribute(name: "targetSdkVersion", id: 0x0101_0270)
    static let glEsVersion = AndroidAttribute(name: "glEsVersion", id: 0x0101_0281)
    static let required = AndroidAttribute(name: "required", id: 0x0101_028E)
    static let extractNativeLibs = AndroidAttribute(name: "extractNativeLibs", id: 0x0101_04EA)
    static let roundIcon = AndroidAttribute(name: "roundIcon", id: 0x0101_052C)
    static let isFeatureSplit = AndroidAttribute(name: "isFeatureSplit", id: 0x0101_055B)
    static let versionCodeMajor = AndroidAttribute(name: "versionCodeMajor", id: 0x0101_0576)
    static let isSplitRequired = AndroidAttribute(name: "isSplitRequired", id: 0x0101_0591)
    static let requiredSplitTypes = AndroidAttribute(name: "requiredSplitTypes", id: 0x0101_064E)
    static let splitTypes = AndroidAttribute(name: "splitTypes", id: 0x0101_064F)
    // Matched by name only.
    static let version = AndroidAttribute(name: "version", id: nil)
}

extension XMLElement {
    func android(_ attribute: AndroidAttribute) -> XMLAttribute? {
        if let id = attribute.id, let match = attributes.first(where: { $0.resourceID == id }) {
            return match
        }
        return attributes.first { $0.namespace == AndroidAttribute.namespace && $0.name == attribute.name }
    }
}

extension XMLAttribute {
    var string: String? {
        if case .string(let text) = value { return text }
        return rawString
    }

    var integer: Int? {
        switch value {
        case .integer(let number): return Int(number)
        case .string(let text): return Self.parseInteger(text)
        default: return rawString.flatMap(Self.parseInteger)
        }
    }

    var boolean: Bool? {
        switch value {
        case .boolean(let flag): return flag
        case .integer(let number): return number != 0
        case .string(let text): return Bool(text.lowercased())
        default: return rawString.flatMap { Bool($0.lowercased()) }
        }
    }

    private static func parseInteger(_ text: String) -> Int? {
        let trimmed = text.trimmingCharacters(in: .whitespaces)
        if trimmed.lowercased().hasPrefix("0x") { return Int(trimmed.dropFirst(2), radix: 16) }
        return Int(trimmed)
    }
}
