import CryptoKit
import Foundation

/// The arguments of make-apk (ADR-0031, decision 2).
public struct MakeAPKOptions: Equatable, Sendable {
    public var manifest: String
    public var libraries: [String]
    public var assets: String?
    public var output: String
    /// The folder of the key that signs, instead of the debug key: a test app of another signer (ADR-0034).
    public var key: String?
    /// D8's output, classes.dex, classes2.dex, …: the app's Java code (ADR-0039, decision 11).
    public var dex: [String]

    public init(manifest: String, libraries: [String], assets: String?, output: String, key: String? = nil,
                dex: [String] = []) {
        self.manifest = manifest
        self.libraries = libraries
        self.assets = assets
        self.output = output
        self.key = key
        self.dex = dex
    }

    /// `--manifest` and `--out` once each, `--lib` and `--dex` any number of times, `--assets` and `--key` at most once.
    public static func parse(_ arguments: [String]) throws -> MakeAPKOptions {
        var manifest: String?
        var libraries: [String] = []
        var assets: String?
        var output: String?
        var key: String?
        var dex: [String] = []
        var remaining = arguments[...]
        while let option = remaining.popFirst() {
            guard ["--manifest", "--lib", "--dex", "--assets", "--out", "--key"].contains(option) else {
                throw UsageError(message: "unknown argument \(option)")
            }
            // A value that is empty or looks like an option means the value was left out.
            guard let value = remaining.popFirst(), !value.isEmpty, !value.hasPrefix("--") else {
                throw UsageError(message: "\(option) needs a value")
            }
            switch option {
            case "--lib":
                libraries.append(value)
            case "--dex":
                dex.append(value)
            case "--manifest":
                guard manifest == nil else { throw UsageError(message: "--manifest is given twice") }
                manifest = value
            case "--assets":
                guard assets == nil else { throw UsageError(message: "--assets is given twice") }
                assets = value
            case "--key":
                guard key == nil else { throw UsageError(message: "--key is given twice") }
                key = value
            default:
                guard output == nil else { throw UsageError(message: "--out is given twice") }
                output = value
            }
        }
        guard let manifest else { throw UsageError(message: "--manifest is missing") }
        guard let output else { throw UsageError(message: "--out is missing") }
        return MakeAPKOptions(manifest: manifest, libraries: libraries, assets: assets, output: output, key: key,
                              dex: dex)
    }
}

/// Wrong arguments: make-apk ends with exit status 64 (EX_USAGE) and prints how to call it.
public struct UsageError: Error, Equatable, Sendable {
    public var message: String

    public static let exitStatus: Int32 = 64
    public static let usage =
        "usage: make-apk --manifest <AndroidManifest.xml> [--lib <lib*.so>]... [--dex <classes*.dex>]... [--assets <folder>] [--key <folder>] --out <file.apk>"
}

/// make-apk: packs one of our test apps into an APK signed with the debug key (ADR-0031).
public enum MakeAPK {
    /// Runs make-apk and returns its exit status: 0, 1 when it cannot pack, 64 for wrong arguments.
    /// `output` gets the one line that says what was written, `errors` every message.
    public static func run(_ arguments: [String], keyFolder: URL = DebugKey.standardFolder,
                           output: (String) -> Void, errors: (String) -> Void) -> Int32 {
        do {
            let options = try MakeAPKOptions.parse(arguments)
            try checkDestination(options.output)
            let apk = try pack(options, keyFolder: keyFolder)
            // Written to a temporary file and renamed, so a half-written APK never exists.
            let destination = URL(fileURLWithPath: options.output)
            do {
                try apk.write(to: destination, options: .atomic)
            } catch {
                throw PackError(file: options.output, message: "cannot write: \(error.localizedDescription)")
            }
            let sha256 = SHA256.hash(data: apk).map { String(format: "%02x", $0) }.joined()
            output("make-apk: wrote \(options.output) (\(apk.count) bytes, SHA-256 \(sha256))")
            return 0
        } catch let error as UsageError {
            errors("make-apk: \(error.message)\n\(UsageError.usage)")
            return UsageError.exitStatus
        } catch let error as PackError {
            errors("make-apk: \(error)")
            return 1
        } catch {
            errors("make-apk: \(error)")
            return 1
        }
    }

    /// Refuses an output that cannot be written, before anything is packed.
    static func checkDestination(_ path: String) throws {
        var isDirectory: ObjCBool = false
        if FileManager.default.fileExists(atPath: path, isDirectory: &isDirectory), isDirectory.boolValue {
            throw PackError(file: path, message: "a folder; --out takes the path of the APK file")
        }
        let folder = (path as NSString).deletingLastPathComponent
        guard folder.isEmpty || FileManager.default.fileExists(atPath: folder, isDirectory: &isDirectory) && isDirectory.boolValue else {
            throw PackError(file: path, message: "cannot write: there is no folder \(folder)")
        }
    }

    /// The signed APK for these inputs. A message names each input as it was given.
    static func pack(_ options: MakeAPKOptions, keyFolder: URL) throws -> Data {
        let manifest = try ManifestCompiler.compile(read(options.manifest), file: options.manifest)
        let libraries = try options.libraries.map { path in
            PackFile(name: URL(fileURLWithPath: path).lastPathComponent, contents: try read(path), origin: path)
        }
        let dex = try options.dex.map { path in
            PackFile(name: URL(fileURLWithPath: path).lastPathComponent, contents: try read(path), origin: path)
        }
        let assets = try options.assets.map(Self.assets(in:)) ?? []
        let unsigned = try APKLayout.build(manifest: manifest, dex: dex, libraries: libraries, assets: assets)
        let key = options.key.map { URL(fileURLWithPath: $0, isDirectory: true) } ?? keyFolder
        return try APKSigner.sign(unsigned, with: DebugKey.load(from: key))
    }

    /// The files below the assets folder.
    private static func assets(in folder: String) throws -> [PackFile] {
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(atPath: folder, isDirectory: &isDirectory) else {
            throw PackError(file: folder, message: "no such folder")
        }
        guard isDirectory.boolValue else {
            throw PackError(file: folder, message: "not a folder; --assets takes the folder whose files go to assets/")
        }
        do {
            return try PackSources.assets(in: URL(fileURLWithPath: folder, isDirectory: true))
        } catch let error as PackError {
            // PackSources names a file by its path below the folder.
            throw PackError(file: (folder as NSString).appendingPathComponent(error.file), line: error.line, message: error.message)
        } catch {
            throw PackError(file: folder, message: "cannot read: \(error.localizedDescription)")
        }
    }

    private static func read(_ path: String) throws -> Data {
        do {
            return try Data(contentsOf: URL(fileURLWithPath: path))
        } catch {
            throw PackError(file: path, message: "cannot read: \(error.localizedDescription)")
        }
    }
}
