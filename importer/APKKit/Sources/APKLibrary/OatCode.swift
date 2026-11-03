import Foundation

/// What the library knows of an app's compiled code (ADR-0053, M6 c).
public struct CompiledCode: Codable, Equatable, Sendable {
    /// The checksum of the boot class path the oat was made for: ART accepts an odex only for the boot class path
    /// it records, and a framework change moves it, so the odex is then stale and made again.
    public var bootClassPath: String
    /// The format version of the odex's container (APK_CONTAINER_VERSION).
    public var containerVersion: UInt32

    public init(bootClassPath: String, containerVersion: UInt32) {
        self.bootClassPath = bootClassPath
        self.containerVersion = containerVersion
    }
}

extension Library {
    /// Keeps the oat that dex2oat made for the app `id`: base.odex and base.vdex in the version's oat/, the odex's
    /// signed container in oat-containers/. Both folders are made whole or not at all; a container the converter
    /// refuses or cannot write leaves nothing behind and the entry as it was.
    public func installCompiledCode(id: String, odex: URL, vdex: URL, bootClassPath: String,
                                    converter: any ContainerConverting) throws {
        guard let entry = entries.first(where: { $0.id == id }) else { return }
        let manager = FileManager.default
        let code = paths.oatDirectory(for: entry), containers = paths.oatContainerDirectory(for: entry)
        let version = paths.versionDirectory(for: entry)
        let partialCode = version.appendingPathComponent("oat.partial", isDirectory: true)
        let partialContainers = version.appendingPathComponent("oat-containers.partial", isDirectory: true)
        for folder in [partialCode, partialContainers] { try? manager.removeItem(at: folder) }
        do {
            try manager.createDirectory(at: partialCode, withIntermediateDirectories: true)
            try manager.createDirectory(at: partialContainers, withIntermediateDirectories: true)
            try manager.copyItem(at: odex, to: partialCode.appendingPathComponent("base.odex"))
            try manager.copyItem(at: vdex, to: partialCode.appendingPathComponent("base.vdex"))
            try converter.convert(odex, to: partialContainers.appendingPathComponent("base.odex.dylib"))
        } catch {
            for folder in [partialCode, partialContainers] { try? manager.removeItem(at: folder) }
            throw error
        }
        for (partial, final) in [(partialCode, code), (partialContainers, containers)] {
            try? manager.removeItem(at: final)
            try manager.moveItem(at: partial, to: final)
        }
        try record(id: id) {
            $0.compiled = CompiledCode(bootClassPath: bootClassPath, containerVersion: converter.containerVersion)
        }
    }

    /// The folders of the app's compiled code for a start whose boot class path has the checksum `bootClassPath`;
    /// nil while it is not compiled, was made for another boot class path, or its files are gone.
    public func oatFolders(id: String, bootClassPath: String) -> (code: URL, containers: URL)? {
        guard let entry = entries.first(where: { $0.id == id }), entry.compiled?.bootClassPath == bootClassPath else {
            return nil
        }
        let code = paths.oatDirectory(for: entry), containers = paths.oatContainerDirectory(for: entry)
        let manager = FileManager.default
        guard manager.fileExists(atPath: code.appendingPathComponent("base.odex").path),
              manager.fileExists(atPath: code.appendingPathComponent("base.vdex").path),
              manager.fileExists(atPath: containers.appendingPathComponent("base.odex.dylib").path) else { return nil }
        return (code, containers)
    }
}
