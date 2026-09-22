import Foundation

public struct ResourceVariant: Sendable, Equatable {
    public let config: ResourceConfig
    public let value: ResourceValue
}

public struct ResourceFile: Sendable, Equatable {
    public let config: ResourceConfig
    public let path: String
}

/// Parser for resources.arsc (ResTable). Entries are decoded lazily on lookup, because a large
/// game can hold hundreds of thousands of them and we only ever need a handful.
public struct ResourceTable: Sendable {
    private let strings: StringPool
    private let packages: [UInt8: Package]

    public init(data: Data) throws {
        let reader = ByteReader(data, context: "resource table")
        guard reader.count >= 12, try reader.u16(at: 0) == 0x0002 else {
            throw APKError.invalidResourceTable("missing table header")
        }
        let headerSize = Int(try reader.u16(at: 2))
        let declaredSize = Int(try reader.u32(at: 4))
        guard headerSize >= 12, declaredSize >= headerSize else {
            throw APKError.invalidResourceTable("malformed table header")
        }
        let end = min(declaredSize, reader.count)

        var strings: StringPool?
        var packages: [UInt8: Package] = [:]
        try Self.forEachChunk(in: reader.data, from: headerSize, to: end) { type, chunk in
            switch type {
            case 0x0001 where strings == nil:
                strings = try StringPool(chunk: chunk)
            case 0x0200:
                let package = try Package(chunk: chunk)
                if packages[package.id] == nil { packages[package.id] = package }
            default:
                break
            }
        }
        self.strings = strings ?? .empty
        self.packages = packages
    }

    /// All configurations of a resource with a simple (non-bag) value.
    public func values(for id: UInt32) -> [ResourceVariant] {
        guard let chunks = typeChunks(for: id) else { return [] }
        let index = Int(id & 0xFFFF)
        return chunks.compactMap { chunk in
            guard let entry = chunk.entry(at: index, strings: strings), let value = entry.value else { return nil }
            return ResourceVariant(config: chunk.config, value: value)
        }
    }

    /// Resolves a string resource, following references, for the first matching preferred language.
    /// Falls back to the default configuration, then English, then any configuration.
    public func string(for id: UInt32, preferredLanguages: [String]) -> String? {
        var visited: Set<UInt32> = []
        var current = id
        while visited.insert(current).inserted, visited.count <= 16 {
            guard let chosen = Self.pickLanguage(values(for: current), preferredLanguages) else { return nil }
            switch chosen.value {
            case .string(let text): return text
            case .reference(let target): current = target
            default: return nil
            }
        }
        return nil
    }

    /// File-backed variants of a resource (drawables, mipmaps, layouts), e.g. per density.
    public func files(for id: UInt32) -> [ResourceFile] {
        values(for: id).compactMap { variant in
            guard case .string(let path) = variant.value else { return nil }
            return ResourceFile(config: variant.config, path: path)
        }
    }

    /// "type/name", e.g. "mipmap/ic_launcher".
    public func name(of id: UInt32) -> String? {
        guard let package = packages[UInt8(id >> 24)], let chunks = typeChunks(for: id) else { return nil }
        let index = Int(id & 0xFFFF)
        guard let key = chunks.lazy.compactMap({ $0.entry(at: index, strings: self.strings)?.key }).first,
              let typeName = package.typeName(UInt8((id >> 16) & 0xFF)),
              let keyName = package.keyNames[key]
        else { return nil }
        return "\(typeName)/\(keyName)"
    }

    private func typeChunks(for id: UInt32) -> [TypeChunk]? {
        packages[UInt8(id >> 24)]?.types[UInt8((id >> 16) & 0xFF)]
    }

    static func pickLanguage(_ variants: [ResourceVariant], _ preferredLanguages: [String]) -> ResourceVariant? {
        for preferred in preferredLanguages {
            let language = preferred.split(whereSeparator: { $0 == "-" || $0 == "_" }).first.map { $0.lowercased() }
            if let match = variants.first(where: { $0.config.language == language }) { return match }
        }
        return variants.first { $0.config.language == nil }
            ?? variants.first { $0.config.language == "en" }
            ?? variants.first
    }

    fileprivate static func forEachChunk(in data: Data, from start: Int, to end: Int,
                                         _ body: (UInt16, Data) throws -> Void) throws {
        let reader = ByteReader(data, context: "resource chunks")
        var position = start
        while position + 8 <= end {
            let type = try reader.u16(at: position)
            let headerSize = Int(try reader.u16(at: position + 2))
            let size = Int(try reader.u32(at: position + 4))
            guard headerSize >= 8, size >= headerSize, size <= end - position else {
                throw APKError.invalidResourceTable("malformed chunk at offset \(position)")
            }
            try body(type, reader.data.subdata(in: position..<position + size))
            position += size
        }
    }
}

// MARK: - Package and type chunks

private struct Package: Sendable {
    let id: UInt8
    let typeNames: StringPool
    let keyNames: StringPool
    let typeIDOffset: Int
    let types: [UInt8: [TypeChunk]]

    init(chunk: Data) throws {
        let reader = ByteReader(chunk, context: "package")
        let headerSize = Int(try reader.u16(at: 2))
        guard headerSize >= 284 else { throw APKError.invalidResourceTable("package header of \(headerSize) bytes") }
        id = UInt8(truncatingIfNeeded: try reader.u32(at: 8))
        let typeStrings = Int(try reader.u32(at: 268))
        let keyStrings = Int(try reader.u32(at: 276))
        typeIDOffset = headerSize >= 288 ? Int(try reader.u32(at: 284)) : 0
        typeNames = try Self.pool(in: reader, at: typeStrings)
        keyNames = try Self.pool(in: reader, at: keyStrings)

        var types: [UInt8: [TypeChunk]] = [:]
        try ResourceTable.forEachChunk(in: reader.data, from: headerSize, to: reader.count) { type, body in
            guard type == 0x0201 else { return }
            let chunk = try TypeChunk(chunk: body)
            types[chunk.id, default: []].append(chunk)
        }
        self.types = types
    }

    func typeName(_ typeID: UInt8) -> String? {
        let index = Int(typeID) - 1 - typeIDOffset
        guard index >= 0 else { return nil }
        return typeNames[UInt32(index)]
    }

    private static func pool(in reader: ByteReader, at offset: Int) throws -> StringPool {
        let size = Int(try reader.u32(at: offset + 4))
        guard size >= 8, size <= reader.count - offset else {
            throw APKError.invalidResourceTable("string pool outside package")
        }
        return try StringPool(chunk: reader.data.subdata(in: offset..<offset + size))
    }
}

private struct TypeChunk: Sendable {
    struct Entry {
        let key: UInt32
        /// nil for complex (bag) entries such as styles and arrays.
        let value: ResourceValue?
    }

    let id: UInt8
    let config: ResourceConfig
    private let reader: ByteReader
    private let flags: UInt8
    private let entryCount: Int
    private let offsetsStart: Int
    private let entriesStart: Int

    private static let sparse: UInt8 = 0x01
    private static let offset16: UInt8 = 0x02

    init(chunk: Data) throws {
        reader = ByteReader(chunk, context: "type chunk")
        let headerSize = Int(try reader.u16(at: 2))
        guard headerSize >= 24 else { throw APKError.invalidResourceTable("type header of \(headerSize) bytes") }
        id = reader.data[8]
        flags = reader.data[9]
        entryCount = Int(try reader.u32(at: 12))
        entriesStart = Int(try reader.u32(at: 16))
        let configSize = Int(try reader.u32(at: 20))
        guard configSize >= 4, configSize <= headerSize - 20, entriesStart <= reader.count else {
            throw APKError.invalidResourceTable("malformed type chunk")
        }
        config = try ResourceConfig(bytes: reader.data.subdata(in: 20..<20 + configSize))
        offsetsStart = headerSize
    }

    func entry(at index: Int, strings: StringPool) -> Entry? {
        guard let offset = entryOffset(index) else { return nil }
        return try? decodeEntry(at: offset, strings: strings)
    }

    private func entryOffset(_ index: Int) -> Int? {
        if flags & Self.sparse != 0 {
            // Sorted (index, offset / 4) pairs.
            var low = 0
            var high = entryCount - 1
            while low <= high {
                let middle = (low + high) / 2
                guard let entryIndex = try? reader.u16(at: offsetsStart + middle * 4) else { return nil }
                if Int(entryIndex) == index {
                    return (try? reader.u16(at: offsetsStart + middle * 4 + 2)).map { entriesStart + Int($0) * 4 }
                }
                if Int(entryIndex) < index { low = middle + 1 } else { high = middle - 1 }
            }
            return nil
        }
        guard index >= 0, index < entryCount else { return nil }
        if flags & Self.offset16 != 0 {
            guard let offset = try? reader.u16(at: offsetsStart + index * 2), offset != 0xFFFF else { return nil }
            return entriesStart + Int(offset) * 4
        }
        guard let offset = try? reader.u32(at: offsetsStart + index * 4), offset != 0xFFFF_FFFF else { return nil }
        return entriesStart + Int(offset)
    }

    private func decodeEntry(at offset: Int, strings: StringPool) throws -> Entry {
        let entryFlags = try reader.u16(at: offset + 2)
        if entryFlags & 0x0008 != 0 {
            // Compact entry (Android 14): key index, flags with the data type in the high byte, data.
            let key = UInt32(try reader.u16(at: offset))
            let value = ResourceValue(type: UInt8(entryFlags >> 8), data: try reader.u32(at: offset + 4), strings: strings)
            return Entry(key: key, value: value)
        }
        let size = Int(try reader.u16(at: offset))
        let key = try reader.u32(at: offset + 4)
        guard entryFlags & 0x0001 == 0 else { return Entry(key: key, value: nil) }
        let valueStart = offset + size
        let type = UInt8(truncatingIfNeeded: try reader.u32(at: valueStart) >> 24)
        let data = try reader.u32(at: valueStart + 4)
        return Entry(key: key, value: ResourceValue(type: type, data: data, strings: strings))
    }
}
