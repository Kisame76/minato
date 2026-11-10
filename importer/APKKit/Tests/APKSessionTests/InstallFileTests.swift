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

    /// A session's set: its paths joined by U+001F, base.apk first; one path for a plain APK.
    @Test func aSetsPathsAreSplitAtTheUnitSeparator() {
        #expect(InstallFile.paths(of: "/data/data/p/a.apk") == ["/data/data/p/a.apk"])
        #expect(InstallFile.paths(of: "/data/data/p/base.apk\u{1F}/data/data/p/split_x.apk")
            == ["/data/data/p/base.apk", "/data/data/p/split_x.apk"])
        for text in ["\u{1F}/data/data/p/a.apk", "/data/data/p/a.apk\u{1F}", "a\u{1F}\u{1F}b"] {
            #expect(InstallFile.paths(of: text) == nil, "\(text.debugDescription)")
        }
        #expect(InstallFile.paths(of: Array(repeating: "a", count: InstallFile.maxSetFiles + 1)
            .joined(separator: "\u{1F}")) == nil)
    }

    @Test func onlyAPathBelowTheAppsOwnDataFolderMaps() {
        let p = Self.package
        #expect(InstallFile.components(of: "/data/user/0/\(p)/cache/install/1.apk", package: p)
            == ["cache", "install", "1.apk"])
        #expect(InstallFile.components(of: "/data/data/\(p)/files/a.apk", package: p) == ["files", "a.apk"])
        #expect(InstallFile.components(of: "/data/data/\(p)/s\u{0600}/x.apk", package: p) == ["s\u{0600}", "x.apk"])
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
            // A mark that joins the slash after it into one grapheme cluster (U+0600 prepends, U+0301 combines with
            // the slash before it): split by characters, "/" and ".." hide inside one component; split by bytes, not.
            "/data/data/\(p)/a\u{0600}/../../other.app/x.apk",
            "/data/data/\(p)/cache/\u{0301}../../other.app/x.apk",
            "/data/data/\(p)/cache/x\u{0000}.apk",
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

        try InstallFile.copy(["cache", "install", "1.apk"], below: folders.data, to: target, limit: 3)

        #expect(try Data(contentsOf: target) == Data("apk".utf8))
    }

    /// A file larger than the limit (the room the copy may take) is refused before a byte is copied, and a component
    /// that is no single name is refused whatever the caller passes.
    @Test func aFileOverTheLimitOrANameWithASlashIsRefused() throws {
        let folders = try Folders()
        defer { folders.remove() }
        let target = folders.root.appending(path: "copy.apk")

        #expect(throws: InstallFile.Failure.tooLarge(3)) {
            try InstallFile.copy(["cache", "install", "1.apk"], below: folders.data, to: target, limit: 2)
        }
        for components in [["cache/install", "1.apk"], ["cache", "..", "cache", "install", "1.apk"], ["."], []] {
            #expect(throws: InstallFile.Failure.self, "\(components)") {
                try InstallFile.copy(components, below: folders.data, to: target, limit: 1 << 20)
            }
        }
        #expect(!FileManager.default.fileExists(atPath: target.path))
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
                try InstallFile.copy(components, below: folders.data, to: target, limit: 1 << 20)
            }
            #expect(!files.fileExists(atPath: target.path), "\(components)")
        }
    }
}
