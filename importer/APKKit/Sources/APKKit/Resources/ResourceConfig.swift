import Foundation

/// The qualifiers of one resource configuration (ResTable_config) that matter for APK import.
public struct ResourceConfig: Sendable, Equatable, Hashable, Codable {
    /// ISO 639 language, e.g. "de"; nil for the default configuration.
    public var language: String?
    /// ISO 3166 region, e.g. "AT".
    public var region: String?
    /// Screen density in dpi; 0 = default, 0xFFFE = anydpi, 0xFFFF = nodpi.
    public var density: UInt16
    /// Minimum platform version qualifier, e.g. 26 for `-v26`.
    public var sdkVersion: UInt16

    public static let densityAny: UInt16 = 0xFFFE
    public static let densityNone: UInt16 = 0xFFFF

    public init(language: String? = nil, region: String? = nil, density: UInt16 = 0, sdkVersion: UInt16 = 0) {
        self.language = language
        self.region = region
        self.density = density
        self.sdkVersion = sdkVersion
    }

    /// Decodes the fields we use from a ResTable_config; shorter (older) configs leave the rest at defaults.
    init(bytes: Data) throws {
        let reader = ByteReader(bytes, context: "resource config")
        let size = Int(try reader.u32(at: 0))
        self.init()
        if size >= 12 {
            language = Self.decodeLocale(reader.data[8], reader.data[9], base: UInt8(ascii: "a"))
            region = Self.decodeLocale(reader.data[10], reader.data[11], base: UInt8(ascii: "0"))
        }
        if size >= 16 { density = try reader.u16(at: 14) }
        if size >= 26 { sdkVersion = try reader.u16(at: 24) }
    }

    /// Two-letter codes are stored as ASCII; three-letter codes are packed into 15 bits.
    private static func decodeLocale(_ first: UInt8, _ second: UInt8, base: UInt8) -> String? {
        guard first != 0 else { return nil }
        if first & 0x80 == 0 {
            return String(bytes: [first, second], encoding: .ascii)
        }
        let letters = [
            second & 0x1F,
            ((second & 0xE0) >> 5) | ((first & 0x03) << 3),
            (first & 0x7C) >> 2,
        ]
        return String(bytes: letters.map { base &+ $0 }, encoding: .ascii)
    }
}
