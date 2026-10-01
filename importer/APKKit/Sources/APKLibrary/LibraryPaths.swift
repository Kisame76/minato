import Foundation

/// Where the library keeps things: installed apps and their data under Library/Application Support.
/// There is no folder in the Files app (ADR-0018); Documents/ only receives what iOS hands over.
public struct LibraryPaths: Sendable {
    public let documents: URL
    public let applicationSupport: URL

    public init(documents: URL, applicationSupport: URL) {
        self.documents = documents
        self.applicationSupport = applicationSupport
    }

    /// The app's own container.
    public static func standard() throws -> LibraryPaths {
        let fileManager = FileManager.default
        return LibraryPaths(
            documents: try fileManager.url(for: .documentDirectory, in: .userDomainMask, appropriateFor: nil, create: true),
            applicationSupport: try fileManager.url(for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil, create: true))
    }

    /// Files picked with + or "Open with" wait here, hidden, until they are installed.
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
}
