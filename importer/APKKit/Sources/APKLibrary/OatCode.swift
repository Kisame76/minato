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

/// How often iOS ended the compile of an app's version for a boot class path before it had a status (E5): its memory,
/// or a swipe away while compile mode was suspended. The second end records a failure.
public struct CompileInterruption: Codable, Equatable, Sendable {
    public var bootClassPath: String
    public var count: Int

    public init(bootClassPath: String, count: Int) {
        self.bootClassPath = bootClassPath
        self.count = count
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
            $0.compileFailed = nil
            $0.compileInterrupted = nil
        }
    }

    /// Records that compiling the app `id` for the boot class path `bootClassPath` failed or that iOS ended it, so
    /// that compile mode does not follow every session of the app (E5); a new version or boot class path tries again.
    public func recordCompileFailure(id: String, bootClassPath: String) throws {
        try record(id: id) { $0.compileFailed = bootClassPath }
    }

    /// Counts an end of the compile that iOS made (E5): the first for a boot class path tries again after the next
    /// session, and the second records the failure, so that a compile iOS cannot finish never loops.
    public func recordCompileInterruption(id: String, bootClassPath: String) throws {
        try record(id: id) {
            let earlier = $0.compileInterrupted?.bootClassPath == bootClassPath ? $0.compileInterrupted?.count ?? 0 : 0
            $0.compileInterrupted = CompileInterruption(bootClassPath: bootClassPath, count: earlier + 1)
            if earlier + 1 >= 2 { $0.compileFailed = bootClassPath }
        }
    }

    /// Takes what a compile start left in its output folder (GuestStart(compile:)): base.odex and base.vdex in its
    /// data/local/tmp, which is dex2oat's /data/local/tmp, kept as `installCompiledCode` keeps them. The folder is
    /// removed in every case, so that a killed or failed dex2oat leaves neither a half odex nor a reason to look again;
    /// the entry stays as it was and the error says why.
    public func adoptCompiledOutput(id: String, output: URL, bootClassPath: String,
                                    converter: any ContainerConverting) throws {
        defer { try? FileManager.default.removeItem(at: output) }
        let written = output.appendingPathComponent("data/local/tmp", isDirectory: true)
        try installCompiledCode(id: id, odex: written.appendingPathComponent("base.odex"),
                                vdex: written.appendingPathComponent("base.vdex"), bootClassPath: bootClassPath,
                                converter: converter)
    }

    /// The folders of the app's compiled code for a start whose boot class path has the checksum `bootClassPath`;
    /// nil while it is not compiled, was made for another boot class path, or its files are gone.
    public func oatFolders(id: String, bootClassPath: String) -> (code: URL, containers: URL)? {
        guard let entry = entries.first(where: { $0.id == id }) else { return nil }
        return paths.oatFolders(for: entry, bootClassPath: bootClassPath)
    }
}

extension LibraryPaths {
    /// Whether compile mode compiles the app after its session (E5): an app with Java code, which has no compiled code
    /// for the boot class path `bootClassPath` and whose compile did not fail for it.
    public func needsCompiling(_ entry: LibraryEntry, bootClassPath: String) -> Bool {
        guard case .art = entry.report?.runtime, entry.compileFailed != bootClassPath else { return false }
        return oatFolders(for: entry, bootClassPath: bootClassPath) == nil
    }

    /// `Library.oatFolders` for an entry in hand, which a start reads without the library: the entry's record of what
    /// its compiled code was made for, and the three files there.
    public func oatFolders(for entry: LibraryEntry, bootClassPath: String) -> (code: URL, containers: URL)? {
        guard entry.compiled?.bootClassPath == bootClassPath else { return nil }
        let code = oatDirectory(for: entry), containers = oatContainerDirectory(for: entry)
        let manager = FileManager.default
        guard manager.fileExists(atPath: code.appendingPathComponent("base.odex").path),
              manager.fileExists(atPath: code.appendingPathComponent("base.vdex").path),
              manager.fileExists(atPath: containers.appendingPathComponent("base.odex.dylib").path) else { return nil }
        return (code, containers)
    }
}
