import APKKit
import Foundation

/// Where the library keeps things: installed apps and their data under Library/Application Support.
/// There is no folder in the Files app (ADR-0018); Documents/ only receives what iOS hands over.
public struct LibraryPaths: Sendable {
    public let documents: URL
    public let applicationSupport: URL
    /// Library/Caches, which iOS may empty: only what a start makes again goes there (ADR-0039 c5).
    public let caches: URL

    /// `caches` is Library/Caches beside `applicationSupport` unless given.
    public init(documents: URL, applicationSupport: URL, caches: URL? = nil) {
        self.documents = documents
        self.applicationSupport = applicationSupport
        self.caches = caches ?? applicationSupport.deletingLastPathComponent().appendingPathComponent("Caches", isDirectory: true)
    }

    /// The app's own container.
    public static func standard() throws -> LibraryPaths {
        let fileManager = FileManager.default
        return LibraryPaths(
            documents: try fileManager.url(for: .documentDirectory, in: .userDomainMask, appropriateFor: nil, create: true),
            applicationSupport: try fileManager.url(for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil, create: true),
            caches: try fileManager.url(for: .cachesDirectory, in: .userDomainMask, appropriateFor: nil, create: true))
    }

    /// Files picked with + or "Open with" wait here, hidden, until they are installed: each pick in a folder of its own,
    /// `<uuid>.partial` while it is taken over (ADR-0048, decision 1).
    public var incoming: URL { applicationSupport.appendingPathComponent("incoming", isDirectory: true) }

    /// Where iOS puts copies of files that "Open with" cannot hand over in place (Mail, Safari).
    public var inbox: URL { documents.appendingPathComponent("Inbox", isDirectory: true) }

    public var indexFile: URL { applicationSupport.appendingPathComponent("library.json") }

    /// Where installed apps live, each in a folder named after its package.
    public var appsDirectory: URL { applicationSupport.appendingPathComponent("apps", isDirectory: true) }

    /// An app's folder, which holds the folder of its installed version.
    public func installDirectory(for package: String) -> URL {
        appsDirectory.appendingPathComponent(package, isDirectory: true)
    }

    /// The installed version's folder (ADR-0034): apps/<package>/<folder>/, or apps/<package>/ itself
    /// for an entry from before version folders.
    public func versionDirectory(for entry: LibraryEntry) -> URL {
        let app = installDirectory(for: entry.id)
        return entry.folder.map { app.appendingPathComponent($0, isDirectory: true) } ?? app
    }

    /// Converted native code of the installed version: each ELF original next to its signed
    /// container (ADR-0008).
    public func libraryDirectory(for entry: LibraryEntry) -> URL {
        versionDirectory(for: entry).appendingPathComponent("lib", isDirectory: true)
    }

    /// Stands in for /data/data/<package>, /sdcard/Android/data/<package> and /sdcard/Android/obb/<package>.
    public func dataDirectory(for package: String) -> URL {
        applicationSupport.appendingPathComponent("data", isDirectory: true).appendingPathComponent(package, isDirectory: true)
    }

    /// Makes the app's data, external and OBB folders, with the two that installd's createAppData makes at an
    /// install in /data/user/0/<package>: cache and code_cache (ADR-0039, decision 6). What is there stays; the
    /// app's files, shared_prefs and the rest come when Context first asks for them, as on Android.
    public func prepareData(for package: String) throws {
        let data = dataDirectory(for: package)
        for folder in ["data/cache", "data/code_cache", "external", "obb"] {
            try FileManager.default.createDirectory(at: data.appendingPathComponent(folder, isDirectory: true),
                                                    withIntermediateDirectories: true)
        }
    }

    /// A Java app's guest /data (ADR-0039 c5): Caches/guest/<package>/data, with ART's dalvik-cache (the lead's
    /// decision). Nothing the app keeps lies there: its data is dataDirectory(for:), which longer mounts put over it.
    public func guestDataDirectory(for package: String) -> URL {
        caches.appendingPathComponent("guest", isDirectory: true).appendingPathComponent(package, isDirectory: true)
            .appendingPathComponent("data", isDirectory: true)
    }

    /// What a Java app's start needs, at every start, since iOS may empty Caches (ADR-0039 c5): the guest's /data with
    /// ART's dalvik-cache/arm64, user/0 and the code path's lib/arm64 with the app's JNI libraries (cloneLibraries), as
    /// framework/tests/app_process_run.py makes them, and the version's files read-only, which an app imported before
    /// c5 gets then. Another code path, an older version's, goes. `codePath` is the guest's
    /// /data/app/~~<a>/<package>-<b>. Returns the guest's /data.
    public func prepareJavaStart(for entry: LibraryEntry, codePath: String) throws -> URL {
        let parts = codePath.split(separator: "/", omittingEmptySubsequences: false)
        guard codePath.hasPrefix("/data/app/"), !parts.contains(".."), !parts.contains(".") else {
            throw CocoaError(.fileWriteInvalidFileName, userInfo: [NSFilePathErrorKey: codePath])
        }
        let data = guestDataDirectory(for: entry.id)
        // Under app/ only the code path stays, at each of its levels.
        var parent = data.appendingPathComponent("app", isDirectory: true)
        for name in codePath.dropFirst("/data/app/".count).split(separator: "/").map(String.init) {
            for other in (try? FileManager.default.contentsOfDirectory(atPath: parent.path)) ?? [] where other != name {
                try FileManager.default.removeItem(at: parent.appendingPathComponent(other))
            }
            parent.appendPathComponent(name, isDirectory: true)
        }
        let lib = data.appendingPathComponent("\(codePath.dropFirst("/data/".count))/lib/arm64", isDirectory: true)
        for folder in [data.appendingPathComponent("dalvik-cache/arm64", isDirectory: true),
                       data.appendingPathComponent("user/0", isDirectory: true), lib] {
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        }
        try cloneLibraries(of: entry, into: lib)
        try makeReadOnly(entry)
        return data
    }

    /// ART's class loader opens <code path>/lib/arm64/<name> before it loads a JNI library (DexPathList's
    /// findNativeLibrary), and the loader then takes the version's container. So `folder` holds clones of the
    /// version's ELF originals, read-only, made again at every start, and nothing else. Clones, not the version's lib/,
    /// so that no guest write reaches an original that resign converts and signs (ADR-0039 c5).
    func cloneLibraries(of entry: LibraryEntry, into folder: URL) throws {
        let fileManager = FileManager.default
        let library = libraryDirectory(for: entry)
        for name in try fileManager.contentsOfDirectory(atPath: folder.path) {
            try fileManager.removeItem(at: folder.appendingPathComponent(name))
        }
        // The originals, as Library.libraryFiles(of:) finds them.
        let originals = ((try? fileManager.contentsOfDirectory(atPath: library.path)) ?? []).filter { $0.hasSuffix(".so") }
        for name in originals {
            let clone = folder.appendingPathComponent(name)
            try fileManager.copyItem(at: library.appendingPathComponent(name), to: clone)
            try Self.makeReadOnly(clone)
        }
    }

    /// The version's APKs and its signers.json, if it has one, read-only.
    func makeReadOnly(_ entry: LibraryEntry) throws {
        let version = versionDirectory(for: entry)
        for name in ([entry.base].compactMap { $0 } + entry.splits).map(\.fileName) + [SignersFile.fileName] {
            let file = version.appendingPathComponent(name)
            guard FileManager.default.fileExists(atPath: file.path) else { continue }
            try Self.makeReadOnly(file)
        }
    }

    /// 0444, so that the guest cannot write the file (ADR-0039 c5). Android 15's installer leaves an app's APKs 0644
    /// and system's, readable to the app but not writable; here the app's process owns the file and the runtime opens
    /// it as the guest asks, so only the mode refuses a write. access() answers as Android's without it, ART's check
    /// for a writable dex file among them, since the runtime reports /data/app's files as system's (ADR-0039 decision
    /// 3); stat() shows that owner with the host's mode, 0444 where Android shows 0644.
    static func makeReadOnly(_ file: URL) throws {
        try FileManager.default.setAttributes([.posixPermissions: 0o444], ofItemAtPath: file.path)
    }
}
