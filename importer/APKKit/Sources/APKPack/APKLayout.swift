import APKKit
import Foundation

/// A file to pack: its name in the APK (at the top for DEX files, below `lib/arm64-v8a/` or `assets/`), its bytes, and where it
/// comes from, which a message names; by default its name.
public struct PackFile: Sendable, Equatable {
    public var name: String
    public var contents: Data
    public var origin: String

    public init(name: String, contents: Data, origin: String? = nil) {
        self.name = name
        self.contents = contents
        self.origin = origin ?? name
    }
}

/// The unsigned APK, laid out as aapt2 and `zipalign -P 16 4` lay it out (ADR-0031): the manifest
/// first and deflated, then the DEX files in ART's order, then aapt2's resource table and res/ files when
/// make-apk takes its output, then the libraries, then the assets, each of these last groups in byte order
/// of its names.
public enum APKLayout {
    /// Extensions that aapt2 stores uncompressed because they are compressed already (the defaults
    /// in aapt2's Link.cpp; it compares case-sensitively, and so does make-apk).
    static let storedExtensions = [
        ".jpg", ".jpeg", ".png", ".gif", ".webp",
        ".wav", ".mp2", ".mp3", ".ogg", ".aac", ".mid", ".midi", ".smf", ".jet", ".rtttl", ".imy", ".xmf", ".amr", ".awb",
        ".mpg", ".mpeg", ".mp4", ".m4a", ".m4v", ".3gp", ".3gpp", ".3g2", ".3gpp2", ".wma", ".wmv", ".webm", ".mkv",
    ]

    /// Where a stored library must start: Android maps it in place when `extractNativeLibs` is false,
    /// and 16 KiB suits devices with 4 KB and with 16 KB pages.
    static let libraryAlignment = 16_384
    /// Where every other stored entry starts, as zipalign's default.
    static let storedAlignment = 4

    /// `manifest` is the compiled binary XML; DEX files go to the top, named classes.dex, classes2.dex, …
    /// (ADR-0039, decision 11), libraries to `lib/arm64-v8a/`, assets to `assets/`. `resources` is aapt2's
    /// output, whose manifest is `manifest`.
    public static func build(manifest: Data, dex: [PackFile] = [], libraries: [PackFile], assets: [PackFile],
                             resources: LinkedResources? = nil) throws -> Data {
        let parsed = try AndroidManifest(xml: BinaryXML.parse(manifest))
        let code = try inARTsOrder(dex, for: parsed)
        try check(libraries, for: parsed)
        // Android's zip reader refuses an APK with two entries of the same name.
        var assetNames = Set<String>()
        for asset in assets where !assetNames.insert(asset.name).inserted {
            throw PackError(file: asset.origin, message: "given twice")
        }

        // Android maps stored libraries in place and extracts deflated ones.
        let storesLibraries = parsed.extractNativeLibs == false
        var zip = ZipBuilder()
        zip.add("AndroidManifest.xml", manifest, method: .deflated)
        // Stored and aligned, ART maps a DEX file in place; otherwise it extracts it to memory (dex_file_loader.cc).
        for file in code {
            zip.add(file.name, file.contents, method: .stored, alignment: storedAlignment)
        }
        if let resources {
            // Stored and 4-byte aligned, as Android needs the table from targetSdk 30 on; res/ files as aapt2 chose.
            zip.add("resources.arsc", resources.table, method: .stored, alignment: storedAlignment)
            for file in resources.files {
                zip.add(file.name, file.contents, method: file.stored ? .stored : .deflated,
                        alignment: file.stored ? storedAlignment : nil)
            }
        }
        for library in libraries.sorted(by: inByteOrder) {
            zip.add("lib/arm64-v8a/\(library.name)", library.contents, method: storesLibraries ? .stored : .deflated,
                    alignment: storesLibraries ? libraryAlignment : nil)
        }
        for asset in assets.sorted(by: inByteOrder) {
            let stored = storedExtensions.contains { asset.name.hasSuffix($0) }
            zip.add("assets/\(asset.name)", asset.contents, method: stored ? .stored : .deflated,
                    alignment: stored ? storedAlignment : nil)
        }
        return zip.build().data
    }

    /// The DEX files ordered as ART opens them: classes.dex, then classes2.dex and up, until the first
    /// missing name (dex_file_loader.cc). Refuses a file ART would never open, and code that does not
    /// agree with the manifest's `android:hasCode`.
    private static func inARTsOrder(_ dex: [PackFile], for manifest: AndroidManifest) throws -> [PackFile] {
        var byIndex: [Int: PackFile] = [:]
        for file in dex {
            guard let index = multidexIndex(file.name) else {
                throw PackError(file: file.origin, message: "ART opens only DEX files named classes.dex, classes2.dex, classes3.dex and up")
            }
            guard byIndex[index] == nil else { throw PackError(file: file.origin, message: "given twice") }
            guard isDex(file.contents) else {
                throw PackError(file: file.origin, message: #"not a DEX file: it does not start with "dex\n", three digits and a NUL"#)
            }
            byIndex[index] = file
        }
        let ordered = byIndex.keys.sorted().compactMap { byIndex[$0] }
        for (position, file) in ordered.enumerated() where multidexIndex(file.name) != position + 1 {
            throw PackError(file: file.origin,
                            message: "\(multidexName(position + 1)) is missing; ART stops at the first missing name and never opens this one")
        }
        if manifest.hasCode && ordered.isEmpty {
            // Android 15 refuses to install it (ScanPackageUtils.assertCodePolicy).
            throw PackError(file: "classes.dex", message: "missing: the manifest has code (android:hasCode is true or left out), "
                + #"and Android refuses an APK without it: "code is missing""#)
        }
        if !manifest.hasCode, let first = ordered.first {
            throw PackError(file: first.origin, message: #"the manifest sets android:hasCode="false", so Android never loads it"#)
        }
        return ordered
    }

    /// 1 for classes.dex, n for classes<n>.dex with n from 2 and no leading zero; nil for any other name.
    static func multidexIndex(_ name: String) -> Int? {
        if name == "classes.dex" { return 1 }
        guard name.hasPrefix("classes"), name.hasSuffix(".dex") else { return nil }
        let digits = name.dropFirst("classes".count).dropLast(".dex".count)
        guard let first = digits.first, first != "0", digits.allSatisfy({ $0.isASCII && $0.isNumber }),
              let index = Int(digits), index >= 2 else { return nil }
        return index
    }

    static func multidexName(_ index: Int) -> String {
        index == 1 ? "classes.dex" : "classes\(index).dex"
    }

    /// A DEX file starts with "dex\n", a version of three digits and a NUL (DexFile::Header's magic).
    static func isDex(_ contents: Data) -> Bool {
        let magic = Array(contents.prefix(8))
        return magic.count == 8 && magic[0..<4].elementsEqual("dex\n".utf8)
            && magic[4..<7].allSatisfy { (0x30...0x39).contains($0) } && magic[7] == 0
    }

    /// Refuses what Android would skip or Minato could not run, before anything is written.
    private static func check(_ libraries: [PackFile], for manifest: AndroidManifest) throws {
        var names = Set<String>()
        for library in libraries {
            // Android's installer skips other names (ApkParsing.cpp, isFilenameSafe).
            guard library.name.hasPrefix("lib"), library.name.hasSuffix(".so"),
                  library.name.allSatisfy({ $0.isASCII && ($0.isLetter || $0.isNumber || "+,-.=_".contains($0)) }) else {
                throw PackError(file: library.origin,
                                message: "Android installs only libraries named lib*.so with A-Z, a-z, 0-9 and + , - . = _")
            }
            guard names.insert(library.name).inserted else {
                throw PackError(file: library.origin, message: "given twice")
            }
            guard (try? ELFProbe.probe(library.contents.prefix(64 * 1024)))?.isAArch64 == true else {
                throw PackError(file: library.origin, message: "not AArch64 code; Minato runs arm64-v8a only (ADR-0006)")
            }
        }
        if let name = manifest.nativeLibraryName, !names.contains("lib\(name).so") {
            throw PackError(file: "lib\(name).so", message: "missing: the manifest's NativeActivity loads it (android.app.lib_name)")
        }
    }

    static func inByteOrder(_ a: PackFile, _ b: PackFile) -> Bool {
        a.name.utf8.lexicographicallyPrecedes(b.name.utf8)
    }
}

/// Reads the inputs of make-apk from the file system.
public enum PackSources {
    /// Every file below `folder`, named by its path relative to it, in byte order. Names that start
    /// with "." (such as .DS_Store or .git) are left out, with everything below them. A symbolic link
    /// is refused: it could lead in a circle or nowhere.
    public static func assets(in folder: URL) throws -> [PackFile] {
        var files: [PackFile] = []
        func walk(_ directory: URL, _ prefix: String) throws {
            for name in try FileManager.default.contentsOfDirectory(atPath: directory.path) where !name.hasPrefix(".") {
                let url = directory.appending(path: name)
                if try url.resourceValues(forKeys: [.isSymbolicLinkKey]).isSymbolicLink == true {
                    throw PackError(file: prefix + name, message: "a symbolic link; make-apk packs files and folders only")
                }
                var isDirectory: ObjCBool = false
                guard FileManager.default.fileExists(atPath: url.path, isDirectory: &isDirectory) else { continue }
                if isDirectory.boolValue {
                    try walk(url, prefix + name + "/")
                } else {
                    files.append(PackFile(name: prefix + name, contents: try Data(contentsOf: url)))
                }
            }
        }
        try walk(folder, "")
        return files.sorted(by: APKLayout.inByteOrder)
    }
}
