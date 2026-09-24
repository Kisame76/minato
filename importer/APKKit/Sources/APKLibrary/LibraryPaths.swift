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

    public func installDirectory(for package: String) -> URL {
        applicationSupport.appendingPathComponent("apps", isDirectory: true).appendingPathComponent(package, isDirectory: true)
    }

    /// Converted native code: each ELF original next to its signed container (ADR-0008).
    public func libraryDirectory(for package: String) -> URL {
        installDirectory(for: package).appendingPathComponent("lib", isDirectory: true)
    }

    /// Stands in for /data/data/<package>, /sdcard/Android/data/<package> and /sdcard/Android/obb/<package>.
    public func dataDirectory(for package: String) -> URL {
        applicationSupport.appendingPathComponent("data", isDirectory: true).appendingPathComponent(package, isDirectory: true)
    }
}
