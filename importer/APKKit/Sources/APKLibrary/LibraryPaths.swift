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

    /// Stands in for /data/data/<package>, /data/user_de/0/<package>, /sdcard/Android/data/<package> and
    /// /sdcard/Android/obb/<package>: data, data_de, external and obb.
    public func dataDirectory(for package: String) -> URL {
        applicationSupport.appendingPathComponent("data", isDirectory: true).appendingPathComponent(package, isDirectory: true)
    }

    /// Makes the app's data, device protected data, external and OBB folders, with the two that installd's
    /// createAppData makes at an install in /data/user/0/<package> and in /data/user_de/0/<package>: cache and
    /// code_cache (ADR-0039, decision 6). What is there stays, so an app installed before gets the ones it lacks at its
    /// next start; the app's files, shared_prefs and the rest come when Context first asks for them, as on Android. The
    /// OBB folder is excluded from backups each time, also an app's from before, as a store gives its files again (the
    /// M5 plan's decision 9, ADR-0049 c2).
    ///
    /// The OBB folder is the root of the guest's OBB mounts, so no guest can put anything in its place. Should a link
    /// stand there all the same, it goes as a link, never followed, and anything else that is no folder stops the
    /// import and stays as it is (the lead's reviewer). It is read with lstat, never through a URL, which keeps the
    /// resource values it read once.
    public func prepareData(for package: String) throws {
        let data = dataDirectory(for: package)
        var obb = data.appendingPathComponent("obb", isDirectory: true)
        var status = stat()
        if lstat(obb.path, &status) == 0 {
            switch status.st_mode & S_IFMT {
            case S_IFDIR: break
            case S_IFLNK: try FileManager.default.removeItem(at: obb)
            default: throw LibraryError.obbFolderIsNoFolder(package)
            }
        }
        for folder in ["data/cache", "data/code_cache", "data_de/cache", "data_de/code_cache", "external", "obb"] {
            try FileManager.default.createDirectory(at: data.appendingPathComponent(folder, isDirectory: true),
                                                    withIntermediateDirectories: true)
        }
        var excluded = URLResourceValues()
        excluded.isExcludedFromBackup = true
        try obb.setResourceValues(excluded)
    }

    /// A Java app's guest /data (ADR-0039 c5): Caches/guest/<package>/data, with ART's dalvik-cache (the lead's
    /// decision). Nothing the app keeps lies there: its data is dataDirectory(for:), which longer mounts put over it.
    public func guestDataDirectory(for package: String) -> URL {
        caches.appendingPathComponent("guest", isDirectory: true).appendingPathComponent(package, isDirectory: true)
            .appendingPathComponent("data", isDirectory: true)
    }

    /// What a Java app's start needs, at every start, since iOS may empty Caches (ADR-0039 c5): the guest's /data with
    /// ART's dalvik-cache/arm64, user/0, data and user_de/0 (the parents of the app's data mounts, which the guest
    /// stats with Android 15's owner, root's 0751, ADR-0052; /data/local stays absent, as the owner model counts
    /// /data/local/tmp as the app's) and the code path's lib/arm64 with the app's JNI libraries (cloneLibraries), as
    /// framework/tests/app_process_run.py makes them, and the version's files read-only, which an app imported before
    /// c5 gets then; the version's kept pack slices in local_testing of the app's external files folder (layOutPacks,
    /// ADR-0050). Another code path, an older version's, goes. `codePath` is the guest's
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
                       data.appendingPathComponent("user/0", isDirectory: true),
                       data.appendingPathComponent("data", isDirectory: true),
                       data.appendingPathComponent("user_de/0", isDirectory: true), lib] {
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        }
        try cloneLibraries(of: entry, into: lib)
        try makeReadOnly(entry)
        try layOutPacks(of: entry)
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

    /// The version's kept pack slices and its feature modules' splits in local_testing of the app's external files
    /// folder, /sdcard/Android/data/<package>/files/local_testing, where Google's Play libraries look for them once the
    /// meta-data local_testing_dir names it (ADR-0050 decision 2): APFS clones, read-only as their originals in packs/
    /// are, made again at every start in the folder emptied first, which stays out of backups as packs/ does (ADR-0049
    /// decision 9). The guest can write there, so nothing here follows a link (the lead's review): external and files,
    /// the app's own folders, are opened as themselves, and a link or file in place of one stops the start; one in
    /// place of local_testing, Minato's folder, goes as itself; what local_testing holds goes by unlinkat, a link as a
    /// link; the clones come by clonefileat, each made read-only whatever its original's mode. The app's data folders are
    /// made first, as the session makes them before a start (App's review). An app without packs or a feature module
    /// keeps its external folder as it is.
    func layOutPacks(of entry: LibraryEntry) throws {
        guard entry.usesLocalTesting else { return }
        try prepareData(for: entry.id)
        let external = try Self.openFolder(dataDirectory(for: entry.id).appendingPathComponent("external").path)
        defer { close(external) }
        let files = try Self.openFolder(named: "files", in: external)
        defer { close(files) }
        let folder = try Self.openFolder(named: "local_testing", in: files, replacing: true)
        defer { close(folder) }
        try Self.empty(folder)
        let version = try Self.openFolder(versionDirectory(for: entry).path)
        defer { close(version) }
        func clone(from source: Int32, _ name: String, as localName: String) throws {
            guard clonefileat(source, name, folder, localName, UInt32(CLONE_NOFOLLOW)) == 0,
                  fchmodat(folder, localName, 0o444, AT_SYMLINK_NOFOLLOW) == 0 else {
                throw Self.lastError()
            }
        }
        // The pack slices come from packs/ under their names, a feature module's splits from the version's folder under
        // their names in bundletool's local-testing set (ADR-0050 c2b).
        if let packs = entry.packs, !packs.isEmpty {
            let source = try Self.openFolder(versionDirectory(for: entry).appendingPathComponent("packs").path)
            defer { close(source) }
            for pack in packs {
                try clone(from: source, pack.fileName, as: pack.fileName)
            }
        }
        for (split, localName) in entry.moduleSlices {
            try clone(from: version, split.fileName, as: localName)
        }
        try Self.excludeFromBackups(folder)
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

    // MARK: - Folders the guest can write, by descriptor (ADR-0050 decision 2)

    private static let folderFlags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC

    /// The folder at `path`, whose last part may be the guest's, opened as itself, never through a link.
    private static func openFolder(_ path: String) throws -> Int32 {
        let descriptor = Darwin.open(path, folderFlags)
        guard descriptor >= 0 else { throw lastError() }
        return descriptor
    }

    /// The folder `name` in the folder `parent`, made when it is missing, opened as itself: a link or a file of that
    /// name throws, or with `replacing` goes as itself, and a new folder takes its place.
    private static func openFolder(named name: String, in parent: Int32, replacing: Bool = false) throws -> Int32 {
        let descriptor = openat(parent, name, folderFlags)
        if descriptor >= 0 { return descriptor }
        if replacing, errno == ELOOP || errno == ENOTDIR {
            guard unlinkat(parent, name, 0) == 0 else { throw lastError() }
        } else if errno != ENOENT {
            throw lastError()
        }
        guard mkdirat(parent, name, 0o755) == 0 else { throw lastError() }
        let made = openat(parent, name, folderFlags)
        guard made >= 0 else { throw lastError() }
        return made
    }

    /// Removes what the folder `descriptor` stands for holds: a folder with what it holds, each made writable first, as
    /// the guest may have made one read-only, and a link as itself, never what it points at.
    private static func empty(_ descriptor: Int32) throws {
        guard fchmod(descriptor, 0o755) == 0 else { throw lastError() }
        for name in try names(in: descriptor) {
            var info = stat()
            guard fstatat(descriptor, name, &info, AT_SYMLINK_NOFOLLOW) == 0 else { throw lastError() }
            if info.st_mode & S_IFMT == S_IFDIR {
                let folder = openat(descriptor, name, folderFlags)
                guard folder >= 0 else { throw lastError() }
                defer { close(folder) }
                try empty(folder)
                guard unlinkat(descriptor, name, AT_REMOVEDIR) == 0 else { throw lastError() }
            } else {
                guard unlinkat(descriptor, name, 0) == 0 else { throw lastError() }
            }
        }
    }

    /// The names in the folder `descriptor` stands for, without "." and "..".
    private static func names(in descriptor: Int32) throws -> [String] {
        let copy = dup(descriptor)
        guard copy >= 0 else { throw lastError() }
        guard let stream = fdopendir(copy) else {
            let error = lastError()
            close(copy)
            throw error
        }
        defer { closedir(stream) }
        rewinddir(stream)
        var names: [String] = []
        while let entry = readdir(stream) {
            let name = withUnsafeBytes(of: entry.pointee.d_name) { bytes in
                String(decoding: bytes.prefix(Int(entry.pointee.d_namlen)), as: UTF8.self)
            }
            if name != "." && name != ".." {
                names.append(name)
            }
        }
        return names
    }

    /// Leaves the folder `descriptor` stands for out of backups, by the path the descriptor has, which names no link
    /// (F_GETPATH), and only while that path is the folder.
    private static func excludeFromBackups(_ descriptor: Int32) throws {
        var buffer = [CChar](repeating: 0, count: Int(MAXPATHLEN))
        guard fcntl(descriptor, F_GETPATH, &buffer) == 0 else { throw lastError() }
        let path = String(decoding: buffer.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }, as: UTF8.self)
        var opened = stat()
        var named = stat()
        guard fstat(descriptor, &opened) == 0, lstat(path, &named) == 0 else { throw lastError() }
        guard opened.st_dev == named.st_dev, opened.st_ino == named.st_ino else { throw POSIXError(.ENOENT) }
        var url = URL(fileURLWithPath: path, isDirectory: true)
        var values = URLResourceValues()
        values.isExcludedFromBackup = true
        try url.setResourceValues(values)
    }

    private static func lastError() -> POSIXError {
        POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
    }
}
