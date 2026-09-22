import Foundation

/// Encodes a resources.arsc (ResTable) with one package, the way aapt2 lays it out.
public struct ARSCBuilder {
    public struct Config: Hashable, Sendable {
        public var language: String?
        public var region: String?
        public var density: UInt16
        public var sdkVersion: UInt16

        public init(language: String? = nil, region: String? = nil, density: UInt16 = 0, sdkVersion: UInt16 = 0) {
            self.language = language
            self.region = region
            self.density = density
            self.sdkVersion = sdkVersion
        }

        public static let `default` = Config()
    }

    public enum Value: Sendable {
        case string(String)
        case reference(UInt32)
        case int(Int32)
        case color(UInt32)
    }

    public enum EntryEncoding: Sendable {
        case offset32
        case offset16
        case sparse
    }

    private struct Entry {
        var name: String
        var variants: [(Config, Value)]
    }

    public var packageID: UInt8 = 0x7F
    public var packageName = "org.example.game"
    public var encoding = EntryEncoding.offset32
    public var compactEntries = false

    private var typeNames: [String] = []
    private var entriesByType: [String: [Entry]] = [:]

    public init() {}

    /// Adds a resource and returns its ID (0xPPTTEEEE).
    @discardableResult
    public mutating func add(_ type: String, _ name: String, _ variants: [(Config, Value)]) -> UInt32 {
        if !typeNames.contains(type) { typeNames.append(type) }
        entriesByType[type, default: []].append(Entry(name: name, variants: variants))
        let typeID = UInt32(typeNames.firstIndex(of: type)! + 1)
        let entryIndex = UInt32(entriesByType[type]!.count - 1)
        return UInt32(packageID) << 24 | typeID << 16 | entryIndex
    }

    /// ID the next resource of `type` would get, without adding it.
    public func nextID(_ type: String) -> UInt32 {
        let typeIndex = typeNames.firstIndex(of: type) ?? typeNames.count
        let entryIndex = entriesByType[type]?.count ?? 0
        return UInt32(packageID) << 24 | UInt32(typeIndex + 1) << 16 | UInt32(entryIndex)
    }

    public func build() -> Data {
        var globalStrings: [String] = []
        var keyNames: [String] = []
        func globalIndex(_ string: String) -> UInt32 {
            if let index = globalStrings.firstIndex(of: string) { return UInt32(index) }
            globalStrings.append(string)
            return UInt32(globalStrings.count - 1)
        }
        func keyIndex(_ name: String) -> UInt32 {
            if let index = keyNames.firstIndex(of: name) { return UInt32(index) }
            keyNames.append(name)
            return UInt32(keyNames.count - 1)
        }

        // Pre-register all strings so that pools are complete before chunks are written.
        for type in typeNames {
            for entry in entriesByType[type]! {
                _ = keyIndex(entry.name)
                for (_, value) in entry.variants {
                    if case .string(let text) = value { _ = globalIndex(text) }
                }
            }
        }

        var typeChunks = ByteWriter()
        for (typeOffset, type) in typeNames.enumerated() {
            let typeID = UInt8(typeOffset + 1)
            let entries = entriesByType[type]!

            typeChunks.u16(0x0202)
            typeChunks.u16(16)
            typeChunks.u32(UInt32(16 + 4 * entries.count))
            typeChunks.u8(typeID)
            typeChunks.u8(0)
            typeChunks.u16(0)
            typeChunks.u32(UInt32(entries.count))
            entries.forEach { _ in typeChunks.u32(0) }

            var configs: [Config] = []
            for entry in entries {
                for (config, _) in entry.variants where !configs.contains(config) {
                    configs.append(config)
                }
            }
            for config in configs {
                typeChunks.bytes(typeChunk(typeID: typeID, entries: entries, config: config, globalIndex: globalIndex, keyIndex: keyIndex))
            }
        }

        let typePool = StringPoolWriter.chunk(typeNames, utf8: false)
        let keyPool = StringPoolWriter.chunk(keyNames, utf8: false)
        var package = ByteWriter()
        let packageHeaderSize = 288
        package.u16(0x0200)
        package.u16(UInt16(packageHeaderSize))
        package.u32(UInt32(packageHeaderSize + typePool.count + keyPool.count + typeChunks.count))
        package.u32(UInt32(packageID))
        let nameUnits = Array(packageName.utf16.prefix(127))
        nameUnits.forEach { package.u16($0) }
        package.zeros((128 - nameUnits.count) * 2)
        package.u32(UInt32(packageHeaderSize))
        package.u32(UInt32(typeNames.count))
        package.u32(UInt32(packageHeaderSize + typePool.count))
        package.u32(UInt32(keyNames.count))
        package.u32(0)
        package.bytes(typePool)
        package.bytes(keyPool)
        package.bytes(typeChunks.data)

        let globalPool = StringPoolWriter.chunk(globalStrings, utf8: true)
        var table = ByteWriter()
        table.u16(0x0002)
        table.u16(12)
        table.u32(UInt32(12 + globalPool.count + package.count))
        table.u32(1)
        table.bytes(globalPool)
        table.bytes(package.data)
        return table.data
    }

    private func typeChunk(typeID: UInt8, entries: [Entry], config: Config,
                           globalIndex: (String) -> UInt32, keyIndex: (String) -> UInt32) -> Data {
        var entryData = ByteWriter()
        var offsets: [(index: Int, offset: Int)] = []
        for (index, entry) in entries.enumerated() {
            guard let value = entry.variants.first(where: { $0.0 == config })?.1 else { continue }
            offsets.append((index, entryData.count))
            let (type, data): (UInt8, UInt32) = switch value {
            case .string(let text): (0x03, globalIndex(text))
            case .reference(let id): (0x01, id)
            case .int(let number): (0x10, UInt32(bitPattern: number))
            case .color(let argb): (0x1C, argb)
            }
            if compactEntries {
                entryData.u16(UInt16(keyIndex(entry.name)))
                entryData.u16(UInt16(type) << 8 | 0x0008)
                entryData.u32(data)
            } else {
                entryData.u16(8)
                entryData.u16(0)
                entryData.u32(keyIndex(entry.name))
                entryData.u16(8)
                entryData.u8(0)
                entryData.u8(type)
                entryData.u32(data)
            }
        }

        var offsetTable = ByteWriter()
        var flags: UInt8 = 0
        var entryCount = entries.count
        switch encoding {
        case .offset32:
            for index in 0..<entries.count {
                offsetTable.u32(offsets.first { $0.index == index }.map { UInt32($0.offset) } ?? 0xFFFF_FFFF)
            }
        case .offset16:
            flags = 0x02
            for index in 0..<entries.count {
                offsetTable.u16(offsets.first { $0.index == index }.map { UInt16($0.offset / 4) } ?? 0xFFFF)
            }
            offsetTable.pad(toMultipleOf: 4)
        case .sparse:
            flags = 0x01
            entryCount = offsets.count
            for item in offsets {
                offsetTable.u16(UInt16(item.index))
                offsetTable.u16(UInt16(item.offset / 4))
            }
        }

        let configSize = 64
        let headerSize = 20 + configSize
        var chunk = ByteWriter()
        chunk.u16(0x0201)
        chunk.u16(UInt16(headerSize))
        chunk.u32(UInt32(headerSize + offsetTable.count + entryData.count))
        chunk.u8(typeID)
        chunk.u8(flags)
        chunk.u16(0)
        chunk.u32(UInt32(entryCount))
        chunk.u32(UInt32(headerSize + offsetTable.count))
        chunk.bytes(encodeConfig(config, size: configSize))
        chunk.bytes(offsetTable.data)
        chunk.bytes(entryData.data)
        return chunk.data
    }

    private func encodeConfig(_ config: Config, size: Int) -> Data {
        var out = ByteWriter()
        out.u32(UInt32(size))
        out.u16(0) // mcc
        out.u16(0) // mnc
        out.bytes(Self.code(config.language))
        out.bytes(Self.code(config.region))
        out.u8(0) // orientation
        out.u8(0) // touchscreen
        out.u16(config.density)
        out.u8(0) // keyboard
        out.u8(0) // navigation
        out.u8(0) // input flags
        out.u8(0)
        out.u16(0) // screen width
        out.u16(0) // screen height
        out.u16(config.sdkVersion)
        out.u16(0) // minor version
        out.zeros(size - out.count)
        return out.data
    }

    private static func code(_ text: String?) -> [UInt8] {
        guard let text, text.count == 2 else { return [0, 0] }
        return Array(text.utf8)
    }
}
