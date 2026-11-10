import Darwin
import Foundation
import Testing

@testable import APKSession

/// An app installer's handed-over APK (APKRT_REQUEST_CONFIRM_INSTALL): the guest names a path, which the host takes
/// only inside the app's own data folder and copies without following a link at any level, from one descriptor.
@Suite("Install file")
struct InstallFileTests {
    static let package = "io.example.store"

    @Test func theTextIsAPathAndAPackage() {
        #expect(InstallFile.parse("/data/user/0/p/cache/a.apk\np") == .init(path: "/data/user/0/p/cache/a.apk",
                                                                              package: "p"))
        for text in ["", "a", "a\n", "\nb", "a\nb\nc"] {
            #expect(InstallFile.parse(text) == nil, "\(text.debugDescription)")
        }
    }

    @Test func onlyAPathBelowTheAppsOwnDataFolderMaps() {
        let p = Self.package
        #expect(InstallFile.components(of: "/data/user/0/\(p)/cache/install/1.apk", package: p)
            == ["cache", "install", "1.apk"])
        #expect(InstallFile.components(of: "/data/data/\(p)/files/a.apk", package: p) == ["files", "a.apk"])
        for path in [
            "/data/user/0/other.app/cache/1.apk",   // another app's
            "/data/user/0/\(p)x/cache/1.apk",       // a package name that starts with this one
            "/data/user/0/\(p)",                    // the folder itself
            "/data/user/0/\(p)/",
            "/data/user/0/\(p)/cache/../../other.app/1.apk",
            "/data/user/0/\(p)/./cache/1.apk",
            "/data/user/0/\(p)//cache/1.apk",
            "/storage/emulated/0/Download/1.apk",   // shared storage: not the app's own data
            "data/user/0/\(p)/cache/1.apk",
        ] {
            #expect(InstallFile.components(of: path, package: p) == nil, "\(path)")
        }
    }

    /// A data folder with cache/install/1.apk, and the folder outside it that links may point to.
    struct Folders {
        let root: URL
        var data: URL { root.appending(path: "data") }
        var outside: URL { root.appending(path: "outside") }

        init() throws {
            root = FileManager.default.temporaryDirectory.appending(path: "install-file-\(UUID().uuidString)")
            try FileManager.default.createDirectory(at: data.appending(path: "cache/install"),
                                                    withIntermediateDirectories: true)
            try FileManager.default.createDirectory(at: outside, withIntermediateDirectories: true)
            try Data("apk".utf8).write(to: data.appending(path: "cache/install/1.apk"))
            try Data("secret".utf8).write(to: outside.appending(path: "secret"))
        }

        func remove() { try? FileManager.default.removeItem(at: root) }
    }

    @Test func aRegularFileIsCopied() throws {
        let folders = try Folders()
        defer { folders.remove() }
        let target = folders.root.appending(path: "copy.apk")

        try InstallFile.copy(["cache", "install", "1.apk"], below: folders.data, to: target)

        #expect(try Data(contentsOf: target) == Data("apk".utf8))
    }

    @Test func noLinkLeadsOutAndOnlyARegularFileIsTaken() throws {
        let folders = try Folders()
        defer { folders.remove() }
        let files = FileManager.default
        try files.createSymbolicLink(at: folders.data.appending(path: "cache/install/link.apk"),
                                     withDestinationURL: folders.outside.appending(path: "secret"))
        try files.createSymbolicLink(at: folders.data.appending(path: "linked"), withDestinationURL: folders.outside)
        #expect(mkfifo(folders.data.appending(path: "cache/fifo").path, 0o600) == 0)

        for components in [["cache", "install", "link.apk"], ["linked", "secret"], ["cache", "install"],
                           ["cache", "fifo"], ["cache", "missing.apk"]] {
            let target = folders.root.appending(path: "copy-\(components.joined(separator: "-"))")
            #expect(throws: InstallFile.Failure.self, "\(components)") {
                try InstallFile.copy(components, below: folders.data, to: target)
            }
            #expect(!files.fileExists(atPath: target.path), "\(components)")
        }
    }
}
