import Foundation

/// Compile mode's folder for one app (ADR-0053, M6 c, E5): Caches/compile/<package>, the compile start's /out, where
/// dex2oat writes base.odex and base.vdex and the runtime the exit status. Compile mode cannot sign, so the library
/// takes the folder at its next start (`Library.takeCompileWork`).
public struct CompileWork: Equatable, Sendable {
    public enum Outcome: Equatable, Sendable {
        /// dex2oat ended with 0.
        case compiled
        /// It ended otherwise, or iOS ended the process (no status), as for using too much memory.
        case failed
        /// The user cancelled it.
        case cancelled
    }

    public let package: String
    public let folder: URL

    public init(caches: URL, package: String) {
        self.package = package
        folder = Self.directory(caches: caches).appendingPathComponent(package, isDirectory: true)
    }

    static func directory(caches: URL) -> URL {
        caches.appendingPathComponent("compile", isDirectory: true)
    }

    /// Where the runtime writes the exit status as the program ends (apkrt_report_exit_status), also at a crash.
    public var statusFile: URL { folder.appendingPathComponent("exit-status") }

    private var cancelMark: URL { folder.appendingPathComponent("cancelled") }

    /// The folder made fresh, with dex2oat's /data folders (GuestStart(compile:)'s mounts).
    public func prepare() throws {
        let manager = FileManager.default
        try? manager.removeItem(at: folder)
        for path in ["data/dalvik-cache/arm64", "data/local/tmp"] {
            try manager.createDirectory(at: folder.appendingPathComponent(path, isDirectory: true),
                                        withIntermediateDirectories: true)
        }
    }

    /// The user cancelled the compile: it records no failure, so the app is compiled after a later session.
    public func cancel() throws {
        try Data().write(to: cancelMark)
    }

    public var outcome: Outcome {
        if FileManager.default.fileExists(atPath: cancelMark.path) { return .cancelled }
        let status = (try? String(contentsOf: statusFile, encoding: .utf8))?
            .trimmingCharacters(in: .whitespacesAndNewlines)
        return status == "0" ? .compiled : .failed
    }

    /// The folders compile mode left.
    public static func all(caches: URL) -> [CompileWork] {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: directory(caches: caches).path)) ?? []
        return names.sorted().map { CompileWork(caches: caches, package: $0) }
    }
}

extension Library {
    /// Takes what compile mode left (E5) for the boot class path `bootClassPath`: keeps the odex of a compile that
    /// ended with 0, signed by `converter` (`adoptCompiledOutput`), records a failure for one that failed, ended
    /// without a whole oat or was ended by iOS, and only removes one the user cancelled. Without a converter, nothing
    /// signs yet: a compiled output stays for a later start.
    public func takeCompileWork(bootClassPath: String, converter: (any ContainerConverting)?) throws {
        for work in CompileWork.all(caches: paths.caches) {
            guard entries.contains(where: { $0.id == work.package }) else {
                try? FileManager.default.removeItem(at: work.folder)
                continue
            }
            switch work.outcome {
            case .compiled:
                guard let converter else { continue }
                do {
                    try adoptCompiledOutput(id: work.package, output: work.folder, bootClassPath: bootClassPath,
                                            converter: converter)
                } catch {
                    try recordCompileFailure(id: work.package, bootClassPath: bootClassPath)
                }
            case .failed:
                try? FileManager.default.removeItem(at: work.folder)
                try recordCompileFailure(id: work.package, bootClassPath: bootClassPath)
            case .cancelled:
                try? FileManager.default.removeItem(at: work.folder)
            }
        }
    }
}
