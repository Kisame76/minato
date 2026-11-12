import APKKit
import Foundation

/// A file an import leaves out, with the reason (ADR-0049 decision 4): its path in the container, or the path its
/// manifest.json sends it to where that path is what is wrong. Both are raw text from the container, so control and
/// format characters, a new line or a bidi override among them, are written out as `\u{…}`, and a long one is cut.
public struct IgnoredFile: Sendable, Hashable, Codable {
    /// [guess] At most this many are named; the last counts the rest (`capped`).
    public static let maxCount = 64
    /// [guess] A path is cut to this many characters, a reason to twice as many.
    public static let maxPathLength = 256

    public var path: String
    public var reason: String

    public init(path: String, reason: String) {
        self.path = Self.printable(path, limit: Self.maxPathLength)
        self.reason = Self.printable(reason, limit: 2 * Self.maxPathLength)
    }

    /// `files`, or past `maxCount` its first ones and a last that counts the rest.
    public static func capped(_ files: [IgnoredFile]) -> [IgnoredFile] {
        guard files.count > maxCount else { return files }
        return files.prefix(maxCount - 1) + [IgnoredFile(path: "…", reason: "\(files.count - maxCount + 1) more files are left out")]
    }

    /// `text` with each control, format or separator character written out; past `limit` Unicode scalars, which also
    /// bounds a run of combining marks, cut with "…" to at most `limit`, never in an escape.
    static func printable(_ text: String, limit: Int) -> String {
        var out = "", count = 0
        // The longest start of whole pieces that leaves room for "…".
        var fitting = ""
        for scalar in text.unicodeScalars {
            let piece = switch scalar.properties.generalCategory {
            case .control, .format, .lineSeparator, .paragraphSeparator:
                "\\u{\(String(scalar.value, radix: 16, uppercase: true))}"
            default:
                String(scalar)
            }
            out += piece
            count += piece.unicodeScalars.count
            if count < limit { fitting = out }
            if count > limit { return fitting + "…" }
        }
        return out
    }
}

/// What an XAPK holds for an import (ADR-0049 decisions 3 and 4).
public struct XAPKContents: Sendable, Equatable {
    /// An OBB taken out under its name, read once, so the import never reads a package from a path.
    public struct OBB: Sendable, Equatable {
        public var item: Container.Item
        public var name: OBBName
    }

    /// Its APKs under names of ours, `1.apk` on, in the archive's order.
    public var apks: [Container.Item]
    /// Its OBBs, at most one main and one patch of a package, the expansions' first, then in the archive's order. Each
    /// belongs to the set of the package its name gives, which the import decides once the APKs' manifests are read
    /// (step c2).
    public var obbs: [OBB]
    /// What it leaves out, in the order found, `IgnoredFile.capped`.
    public var ignored: [IgnoredFile]
}

extension Container {
    /// An XAPK's APKs and OBBs. Its `manifest.json`, read within its cap, decides only which files count (decision
    /// 3): the APKs are every `.apk` entry, as in a loose set, and the files `split_apks` names; the OBBs are the files
    /// `expansions` names, each going to its `install_path` or else to its `file`, and the files in `Android/` no
    /// expansion names. An OBB goes directly to `Android/obb/<package>/`, as APKPure puts it, under a name of the
    /// pattern for that package (decision 4). An entry gives one OBB at most, and a package one main and one patch, as
    /// decision 5 keeps them, so the OBBs never outnumber the entries. Any other expansion or file
    /// in `Android/`, any other `.obb`, and a file named but missing are left out and named; hidden files, folders and
    /// macOS's copies count for nothing.
    public func xapkContents() throws -> XAPKContents {
        var ignored: [IgnoredFile] = []
        var manifest: [String: Any] = [:]
        if let entry = archive.entry(named: "manifest.json") {
            if let object = try Self.jsonObject(entry, in: archive) {
                manifest = object
            } else {
                ignored.append(IgnoredFile(path: "manifest.json",
                                           reason: "it is no JSON object, so only the container’s own files count"))
            }
        }

        var named = Set<Data>()
        for file in Self.objects(manifest["split_apks"]).compactMap({ $0["file"] as? String }) {
            guard let entry = archive.entry(named: file), !entry.isDirectory else {
                ignored.append(IgnoredFile(path: file, reason: Self.missing))
                continue
            }
            named.insert(entry.nameBytes)
        }
        let apks = archive.entries.filter { Self.isLooseAPK($0) || named.contains($0.nameBytes) }
        guard !apks.isEmpty else { throw ContainerError.noAPK }
        guard apks.count <= Self.maxAPKs else {
            throw ContainerError.overCap("it holds \(apks.count) APKs, more than the \(Self.maxAPKs) Minato takes from a container")
        }

        var obbs: [XAPKContents.OBB] = []
        var kinds = Set<String>()
        func take(_ entry: ZipEntry, to path: String, namedAs file: String) {
            switch Self.obbName(at: path) {
            case .failure(let unplaced):
                ignored.append(IgnoredFile(path: path, reason: unplaced.reason))
            case .success(let name):
                if kinds.insert("\(name.kind.rawValue) \(name.package)").inserted {
                    obbs.append(XAPKContents.OBB(item: Item(entry: entry, fileName: name.fileName), name: name))
                } else {
                    ignored.append(IgnoredFile(path: file, reason: "it is a second \(name.kind.rawValue) OBB of \(name.package)"))
                }
            }
        }
        // An entry an expansion names is judged by the first such expansion, and an APK stays one.
        let apkNames = Set(apks.map(\.nameBytes))
        var judged = apkNames
        for expansion in Self.objects(manifest["expansions"]) {
            guard let file = expansion["file"] as? String else { continue }
            guard let entry = archive.entry(named: file), !entry.isDirectory else {
                ignored.append(IgnoredFile(path: file, reason: Self.missing))
                continue
            }
            guard !apkNames.contains(entry.nameBytes) else { continue }
            guard judged.insert(entry.nameBytes).inserted else {
                ignored.append(IgnoredFile(path: file, reason: "another expansion names it already"))
                continue
            }
            take(entry, to: expansion["install_path"] as? String ?? file, namedAs: file)
        }
        for entry in archive.entries where !entry.isDirectory && !judged.contains(entry.nameBytes) {
            let name = Self.fileName(of: entry)
            guard !entry.name.hasPrefix("__MACOSX/"), !name.hasPrefix("."),
                  entry.name.hasPrefix("Android/") || name.lowercased().hasSuffix(".obb") else { continue }
            take(entry, to: entry.name, namedAs: entry.name)
        }
        return XAPKContents(apks: apks.enumerated().map { Item(entry: $1, fileName: "\($0 + 1).apk") }, obbs: obbs,
                            ignored: IgnoredFile.capped(ignored))
    }

    /// Why a path is no place for an OBB.
    struct Unplaced: Error, Equatable {
        var reason: String
    }

    /// The OBB name `path` gives: `Android/obb/<package>/<name>`, whose name follows the pattern and gives that
    /// package (decision 4), or why it gives none.
    static func obbName(at path: String) -> Result<OBBName, Unplaced> {
        let parts = path.split(separator: "/", omittingEmptySubsequences: false)
        if path.hasPrefix("/") || parts.contains("..") { return .failure(Unplaced(reason: "its path leaves its folder")) }
        guard parts.count == 4, parts[0] == "Android", parts[1] == "obb" else {
            return .failure(Unplaced(reason: "it is not in Android/obb/<package>/"))
        }
        guard let name = OBBName(String(parts[3])) else {
            return .failure(Unplaced(reason: "its name is not main.<version>.<package>.obb or patch.<version>.<package>.obb"))
        }
        guard name.package == parts[2] else {
            return .failure(Unplaced(reason: "it is in Android/obb/\(parts[2])/, not in the folder of its package"))
        }
        return .success(name)
    }

    private static let missing = "manifest.json names it, but the container holds no such file"

    /// The objects of a JSON list; anything else holds none.
    private static func objects(_ value: Any?) -> [[String: Any]] {
        (value as? [Any] ?? []).compactMap { $0 as? [String: Any] }
    }
}
