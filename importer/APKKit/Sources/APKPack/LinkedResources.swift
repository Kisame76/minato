import APKKit
import Foundation

/// What make-apk's --resources takes from aapt2's linked APK (ADR-0039, decision 11): the compiled manifest, the
/// resource table and the res/ files, each res/ file with the method aapt2 chose for it. Everything else has an
/// option of its own, and make-apk signs.
public struct LinkedResources: Sendable, Equatable {
    public struct File: Sendable, Equatable {
        public var name: String
        public var contents: Data
        public var stored: Bool
    }

    public var manifest: Data
    /// resources.arsc, which aapt2 stores, as Android needs it from targetSdk 30 on.
    public var table: Data
    /// The res/ entries in byte order of their names.
    public var files: [File]

    /// Reads aapt2's output; `origin` is the path a message names.
    public static func read(_ apk: Data, origin: String) throws -> LinkedResources {
        let archive: ZipArchive
        do {
            archive = try ZipArchive(data: apk)
        } catch {
            throw PackError(file: origin, message: "not a ZIP archive: \(error)")
        }
        var manifest: Data?
        var table: Data?
        var files: [File] = []
        var names = Set<String>()
        for entry in archive.entries where !entry.isDirectory {
            guard names.insert(entry.name).inserted else {
                throw PackError(file: origin, message: "\(entry.name) is in it twice")
            }
            switch entry.name {
            case "AndroidManifest.xml":
                manifest = try contents(of: entry, in: archive, origin: origin)
            case "resources.arsc":
                // Android refuses a compressed table from targetSdk 30 on (ParsingPackageUtils,
                // INSTALL_PARSE_FAILED_RESOURCES_ARSC_COMPRESSED); aapt2 stores it.
                guard entry.method == .stored else {
                    throw PackError(file: origin, message: "resources.arsc is compressed, which Android refuses "
                                    + "from targetSdk 30 on; aapt2 link stores it")
                }
                table = try contents(of: entry, in: archive, origin: origin)
            case let name where name.hasPrefix("res/"):
                files.append(File(name: name, contents: try contents(of: entry, in: archive, origin: origin),
                                  stored: entry.method == .stored))
            case let name:
                throw PackError(file: origin, message: "\(name): \(refusal(of: name))")
            }
        }
        guard let manifest else { throw PackError(file: origin, message: "no AndroidManifest.xml in it") }
        guard let table else { throw PackError(file: origin, message: "no resources.arsc in it") }
        files.sort { $0.name.utf8.lexicographicallyPrecedes($1.name.utf8) }
        return LinkedResources(manifest: manifest, table: table, files: files)
    }

    private static func refusal(of name: String) -> String {
        if APKLayout.multidexIndex(name) != nil || name.hasSuffix(".dex") { return "Java code comes with --dex" }
        if name.hasPrefix("lib/") { return "libraries come with --lib" }
        if name.hasPrefix("assets/") { return "assets come with --assets" }
        if name.hasPrefix("META-INF/") { return "make-apk signs the APK itself" }
        return "make-apk takes only AndroidManifest.xml, resources.arsc and res/ from aapt2's output"
    }

    private static func contents(of entry: ZipEntry, in archive: ZipArchive, origin: String) throws -> Data {
        do {
            return try archive.data(for: entry)
        } catch {
            throw PackError(file: origin, message: "cannot read \(entry.name): \(error)")
        }
    }
}
