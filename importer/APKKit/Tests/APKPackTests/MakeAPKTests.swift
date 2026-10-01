import APKKit
import APKTestSupport
import CryptoKit
import Foundation
import Testing

@testable import APKPack

/// make-apk's command line (ADR-0031, decision 2; test 9).
@Suite("make-apk")
struct MakeAPKTests {
    @Test func readsEveryOption() throws {
        let options = try MakeAPKOptions.parse([
            "--manifest", "app/AndroidManifest.xml", "--lib", "build/liba.so", "--assets", "app/assets",
            "--lib", "build/libb.so", "--out", "build/app.apk", "--key", "keys/other", "--dex", "build/classes.dex",
            "--dex", "build/classes2.dex",
        ])

        #expect(options == MakeAPKOptions(manifest: "app/AndroidManifest.xml", libraries: ["build/liba.so", "build/libb.so"],
                                          assets: "app/assets", output: "build/app.apk", key: "keys/other",
                                          dex: ["build/classes.dex", "build/classes2.dex"]))
    }

    @Test func needsNeitherLibrariesNorAssets() throws {
        let options = try MakeAPKOptions.parse(["--out", "a.apk", "--manifest", "m.xml"])

        #expect(options == MakeAPKOptions(manifest: "m.xml", libraries: [], assets: nil, output: "a.apk", key: nil))
    }

    @Test(arguments: [
        ["--out", "a.apk"],
        ["--manifest", "m.xml"],
        ["--manifest", "m.xml", "--out", "a.apk", "--sign"],
        ["--manifest", "m.xml", "--sign", "a.apk"],
        ["--manifest", "m.xml", "--out"],
        ["--manifest", "m.xml", "--manifest", "n.xml", "--out", "a.apk"],
        ["--manifest", "m.xml", "--assets", "a", "--assets", "b", "--out", "a.apk"],
        ["--manifest", "m.xml", "--out", "a.apk", "extra"],
        ["--manifest", "m.xml", "--out", "a.apk", "--out", "b.apk"],
        ["--manifest", "m.xml", "--out", "a.apk", "--lib"],
        ["--manifest", "m.xml", "--out", "a.apk", "--key"],
        ["--manifest", "m.xml", "--out", "a.apk", "--dex"],
        ["--manifest", "m.xml", "--dex", "--out", "a.apk"],
        ["--manifest", "m.xml", "--key", "a", "--key", "b", "--out", "a.apk"],
        ["--manifest", "--out", "--out", "a.apk"],
        ["--manifest", "", "--out", "a.apk"],
        [],
    ])
    func refusesWrongArgumentsWithStatus64(arguments: [String]) throws {
        let error = try #require(throws: UsageError.self) { try MakeAPKOptions.parse(arguments) }
        #expect(!error.message.isEmpty)

        var errors: [String] = []
        let status = MakeAPK.run(arguments, keyFolder: Self.keyFolder, output: { _ in }, errors: { errors.append($0) })
        #expect(status == 64)
        #expect(errors.joined().contains("usage: make-apk"))
    }

    @Test func saysWhichOptionLacksItsValue() throws {
        // A value that looks like an option means the value was left out.
        let error = try #require(throws: UsageError.self) {
            try MakeAPKOptions.parse(["--lib", "--out", "a.apk", "--manifest", "m.xml"])
        }

        #expect(error.message == "--lib needs a value")
    }

    static let keyFolder = URL.temporaryDirectory.appending(path: "make-apk-test-key", directoryHint: .isDirectory)

    /// A test app on disk: manifest, one library and an assets folder, in a fresh folder.
    static func project() throws -> URL {
        let folder = URL.temporaryDirectory.appending(path: "make-apk-project-\(UUID().uuidString)", directoryHint: .isDirectory)
        try FileManager.default.createDirectory(at: folder.appending(path: "assets/sub"), withIntermediateDirectories: true)
        try Data(ManifestCompilerTests.nativeActivity.utf8).write(to: folder.appending(path: "AndroidManifest.xml"))
        try LayoutTests.library().write(to: folder.appending(path: "libactivity-test.so"))
        try Data("hello".utf8).write(to: folder.appending(path: "assets/hello.txt"))
        try Data("image".utf8).write(to: folder.appending(path: "assets/sub/image.png"))
        return folder
    }

    @Test func packsAndSignsAndSaysWhatItWrote() throws {
        let project = try Self.project()
        defer { try? FileManager.default.removeItem(at: project) }
        let apk = project.appending(path: "out/activity-test.apk")
        try FileManager.default.createDirectory(at: apk.deletingLastPathComponent(), withIntermediateDirectories: true)
        var lines: [String] = []
        var errors: [String] = []

        let status = MakeAPK.run([
            "--manifest", project.appending(path: "AndroidManifest.xml").path,
            "--lib", project.appending(path: "libactivity-test.so").path,
            "--assets", project.appending(path: "assets").path,
            "--out", apk.path,
        ], keyFolder: Self.keyFolder, output: { lines.append($0) }, errors: { errors.append($0) })

        #expect(status == 0)
        #expect(errors.isEmpty)
        let data = try Data(contentsOf: apk)
        let sha256 = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
        #expect(lines.count == 1)
        #expect(lines.first?.contains(apk.path) == true)
        #expect(lines.first?.contains("\(data.count) bytes") == true)
        #expect(lines.first?.contains(sha256) == true)
        let archive = try ZipArchive(data: data)
        #expect(archive.entries.map(\.name) == [
            "AndroidManifest.xml", "lib/arm64-v8a/libactivity-test.so", "assets/hello.txt", "assets/sub/image.png",
        ])
        #expect(APKSignatures.read(archive).schemes == [.v2])
        // Written through a temporary file and renamed: nothing else is left in the folder.
        #expect(try FileManager.default.contentsOfDirectory(atPath: apk.deletingLastPathComponent().path) == ["activity-test.apk"])
        #expect(try SigningTests.apkCheck(data).contains("RESULT: signatures valid"))
    }

    /// `--key` signs with the key in another folder, made there on first use: a test app of another signer
    /// (ADR-0034, test 4).
    @Test func signsWithTheKeyItNames() throws {
        let project = try Self.project()
        let other = project.appending(path: "other-key", directoryHint: .isDirectory)
        defer { try? FileManager.default.removeItem(at: project) }
        let apk = project.appending(path: "other.apk")

        var errors: [String] = []
        let status = MakeAPK.run([
            "--manifest", project.appending(path: "AndroidManifest.xml").path,
            "--lib", project.appending(path: "libactivity-test.so").path, "--out", apk.path, "--key", other.path,
        ], keyFolder: Self.keyFolder, output: { _ in }, errors: { errors.append($0) })

        #expect(status == 0)
        #expect(errors.isEmpty)
        let signer = APKSignatures.read(try ZipArchive(data: Data(contentsOf: apk))).signer
        let certificate = try DebugKey.load(from: other).certificate
        #expect(signer?.current == [SigningCertificate(der: certificate).sha256])
        #expect(try DebugKey.load(from: Self.keyFolder).certificate != certificate)
    }

    /// An app with Java code: the manifest leaves hasCode at its default, true, and D8's output comes with --dex
    /// (ADR-0039, decision 11).
    @Test func packsJavaCode() throws {
        let project = try Self.project()
        defer { try? FileManager.default.removeItem(at: project) }
        let manifest = project.appending(path: "AndroidManifest.xml")
        try Data(ManifestCompilerTests.nativeActivity.replacingOccurrences(of: #" android:hasCode="false""#, with: "").utf8)
            .write(to: manifest)
        let d8 = project.appending(path: "build/d8", directoryHint: .isDirectory)
        try FileManager.default.createDirectory(at: d8, withIntermediateDirectories: true)
        try LayoutTests.dex(0x01).write(to: d8.appending(path: "classes.dex"))
        try LayoutTests.dex(0x02).write(to: d8.appending(path: "classes2.dex"))
        let apk = project.appending(path: "java.apk")

        let (status, errors) = Self.run(project, ["--dex", d8.appending(path: "classes2.dex").path,
                                                  "--dex", d8.appending(path: "classes.dex").path, "--out", apk.path])

        #expect(status == 0, "\(errors)")
        let data = try Data(contentsOf: apk)
        let archive = try ZipArchive(data: data)
        #expect(archive.entries.map(\.name) == ["AndroidManifest.xml", "classes.dex", "classes2.dex",
                                                "lib/arm64-v8a/libactivity-test.so"])
        #expect(try archive.data(named: "classes2.dex") == LayoutTests.dex(0x02))
        #expect(try SigningTests.apkCheck(data).contains("RESULT: signatures valid"))
    }

    @Test func namesADexFileByItsPath() throws {
        let project = try Self.project()
        defer { try? FileManager.default.removeItem(at: project) }
        let dex = project.appending(path: "deep/dir/classes.dex")
        try FileManager.default.createDirectory(at: dex.deletingLastPathComponent(), withIntermediateDirectories: true)
        try Data("not dex".utf8).write(to: dex)

        let (status, errors) = Self.run(project, ["--dex", dex.path, "--out", project.appending(path: "app.apk").path])

        #expect(status == 1)
        #expect(errors.contains("\(dex.path): not a DEX file"), "\(errors)")
    }

    @Test func namesTheFileAndLineOfAProblemAndWritesNothing() throws {
        let project = try Self.project()
        defer { try? FileManager.default.removeItem(at: project) }
        let manifest = project.appending(path: "AndroidManifest.xml")
        try Data(ManifestCompilerTests.manifest(line: #"<application android:theme="x"/>"#).utf8).write(to: manifest)
        let apk = project.appending(path: "app.apk")
        var errors: [String] = []

        let status = MakeAPK.run(["--manifest", manifest.path, "--out", apk.path], keyFolder: Self.keyFolder,
                                 output: { _ in }, errors: { errors.append($0) })

        #expect(status == 1)
        #expect(errors.joined().contains("\(manifest.path):3:"))
        #expect(!FileManager.default.fileExists(atPath: apk.path))
    }

    @Test func namesAnInputThatDoesNotExist() throws {
        let project = try Self.project()
        defer { try? FileManager.default.removeItem(at: project) }
        let missing = project.appending(path: "libmissing.so").path
        var errors: [String] = []

        let status = MakeAPK.run([
            "--manifest", project.appending(path: "AndroidManifest.xml").path,
            "--lib", project.appending(path: "libactivity-test.so").path, "--lib", missing,
            "--out", project.appending(path: "app.apk").path,
        ], keyFolder: Self.keyFolder, output: { _ in }, errors: { errors.append($0) })

        #expect(status == 1)
        #expect(errors.joined().contains(missing))
    }

    /// Runs make-apk with the test key on the project's manifest and library, plus these arguments.
    static func run(_ project: URL, _ arguments: [String]) -> (status: Int32, errors: String) {
        var errors: [String] = []
        let status = MakeAPK.run(["--manifest", project.appending(path: "AndroidManifest.xml").path,
                                  "--lib", project.appending(path: "libactivity-test.so").path] + arguments,
                                 keyFolder: keyFolder, output: { _ in }, errors: { errors.append($0) })
        return (status, errors.joined(separator: "\n"))
    }

    @Test func namesAnOutputFolderThatDoesNotExist() throws {
        let project = try Self.project()
        defer { try? FileManager.default.removeItem(at: project) }
        let output = project.appending(path: "missing/app.apk").path

        let (status, errors) = Self.run(project, ["--out", output])

        #expect(status == 1)
        #expect(errors.contains("\(output): cannot write: there is no folder \(project.appending(path: "missing").path)"), "\(errors)")
    }

    @Test func takesAnOutputWithoutAFolder() throws {
        // A bare file name goes to the current folder.
        try MakeAPK.checkDestination("make-apk-\(UUID().uuidString).apk")
    }

    @Test func namesAnOutputThatIsAFolder() throws {
        let project = try Self.project()
        defer { try? FileManager.default.removeItem(at: project) }

        let (status, errors) = Self.run(project, ["--out", project.path])

        #expect(status == 1)
        #expect(errors.contains("\(project.path): a folder"), "\(errors)")
    }

    @Test(arguments: [("missing", "no such folder"), ("AndroidManifest.xml", "not a folder")])
    func namesAnAssetsPathThatIsNoFolder(name: String, problem: String) throws {
        let project = try Self.project()
        defer { try? FileManager.default.removeItem(at: project) }
        let assets = project.appending(path: name).path

        let (status, errors) = Self.run(project, ["--assets", assets, "--out", project.appending(path: "app.apk").path])

        #expect(status == 1)
        #expect(errors.contains("\(assets): \(problem)"), "\(errors)")
    }

    @Test func namesAnAssetByItsPath() throws {
        let project = try Self.project()
        defer { try? FileManager.default.removeItem(at: project) }
        let assets = project.appending(path: "assets")
        try FileManager.default.createSymbolicLink(at: assets.appending(path: "sub/link"),
                                                   withDestinationURL: assets.appending(path: "hello.txt"))

        let (status, errors) = Self.run(project, ["--assets", assets.path, "--out", project.appending(path: "app.apk").path])

        #expect(status == 1)
        #expect(errors.contains("\(assets.path)/sub/link: a symbolic link"), "\(errors)")
    }

    @Test func namesAnAssetsFolderItCannotRead() throws {
        let project = try Self.project()
        let locked = project.appending(path: "assets/locked")
        try FileManager.default.createDirectory(at: locked, withIntermediateDirectories: true)
        try FileManager.default.setAttributes([.posixPermissions: 0], ofItemAtPath: locked.path)
        defer {
            try? FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: locked.path)
            try? FileManager.default.removeItem(at: project)
        }
        let assets = project.appending(path: "assets").path

        let (status, errors) = Self.run(project, ["--assets", assets, "--out", project.appending(path: "app.apk").path])

        #expect(status == 1)
        #expect(errors.contains("\(assets): cannot read:"), "\(errors)")
    }

    @Test(arguments: [
        ("libother.so", false, "not AArch64 code"),
        ("other.so", true, "Android installs only libraries named lib*.so"),
        ("libactivity-test.so", true, "given twice"),
    ])
    func namesALibraryByItsPath(name: String, isLibrary: Bool, problem: String) throws {
        let project = try Self.project()
        defer { try? FileManager.default.removeItem(at: project) }
        let library = project.appending(path: "deep/dir/\(name)")
        try FileManager.default.createDirectory(at: library.deletingLastPathComponent(), withIntermediateDirectories: true)
        try (isLibrary ? LayoutTests.library() : Data("not ELF".utf8)).write(to: library)

        let (status, errors) = Self.run(project, ["--lib", library.path, "--out", project.appending(path: "app.apk").path])

        #expect(status == 1)
        #expect(errors.contains("\(library.path): \(problem)"), "\(errors)")
    }
}
