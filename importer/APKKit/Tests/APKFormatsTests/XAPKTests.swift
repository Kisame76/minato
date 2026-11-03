import APKFormats
import APKKit
import APKTestSupport
import Foundation
import Testing

/// An XAPK's APKs and OBBs (ADR-0049 decisions 3 and 4, test 4): its manifest.json names files and decides nothing
/// else, and an OBB goes to Android/obb/<package>/ under a name of the pattern, or is left out and named.
@Suite("XAPK")
struct XAPKTests {
    static let package = SplitAPK.package
    static let main = "main.7.\(package).obb", patch = "patch.7.\(package).obb"
    static let base = SplitAPK.make(), abi = SplitAPK.make(split: "config.arm64_v8a"),
               density = SplitAPK.make(split: "config.xxhdpi")
    static let icon = Data([0x89, 0x50, 0x4E, 0x47])

    static func obbPath(_ name: String, in folder: String = package) -> String { "Android/obb/\(folder)/\(name)" }

    /// APKPure's manifest.json, its numbers as numbers or as text, with `fields` added.
    static func manifest(_ fields: [String: Any] = [:], numbersAsText: Bool = false) -> Data {
        func number(_ value: Int) -> Any { numbersAsText ? String(value) : value }
        var object: [String: Any] = [
            "xapk_version": number(2), "package_name": package, "name": "Sets", "version_code": number(7),
            "version_name": "1.0", "min_sdk_version": number(24), "target_sdk_version": number(35),
            "permissions": ["android.permission.INTERNET"], "total_size": number(123_456),
        ]
        object.merge(fields) { $1 }
        return try! JSONSerialization.data(withJSONObject: object, options: [.sortedKeys])
    }

    static func splitAPKs(_ files: [(file: String, id: String)]) -> [String: Any] {
        ["split_apks": files.map { ["file": $0.file, "id": $0.id] }]
    }

    /// An `expansions` element; a nil `installPath` leaves the key out.
    static func expansion(_ file: String, installPath: String?) -> [String: Any] {
        var element: [String: Any] = ["file": file, "install_location": "EXTERNAL_STORAGE"]
        element["install_path"] = installPath
        return element
    }

    static func open(_ entries: [(String, Data)], method: ZipBuilder.Method = .deflated) throws -> (Container, ContainerTests.File) {
        let file = try ContainerTests.File(ContainerTests.zip(entries, method: method), name: "game.xapk")
        return (try Container.open(file.url), file)
    }

    // MARK: - The APKs (decision 3)

    @Test func aSplitXAPKGivesItsAPKsUnderNamesOfOurs() throws {
        let manifest = Self.manifest(Self.splitAPKs([("\(Self.package).apk", "base"), ("config.arm64_v8a.apk", "config.arm64_v8a"),
                                                     ("config.xxhdpi.apk", "config.xxhdpi")]))
        let (container, _) = try Self.open([("manifest.json", manifest), ("\(Self.package).apk", Self.base),
                                            ("config.arm64_v8a.apk", Self.abi), ("config.xxhdpi.apk", Self.density),
                                            ("icon.png", Self.icon)])
        #expect(container.kind == .xapk)
        let contents = try container.xapkContents()

        #expect(contents.apks.map(\.entry.name) == ["\(Self.package).apk", "config.arm64_v8a.apk", "config.xxhdpi.apk"])
        #expect(contents.apks.map(\.fileName) == ["1.apk", "2.apk", "3.apk"])
        #expect(contents.obbs.isEmpty)
        #expect(contents.ignored.isEmpty)
        let unit = try ContainerExtractionTests.Unit()
        let files = try container.extract(contents.apks + contents.obbs.map(\.item), into: unit.destination)
        let members = try files.map { try SetMember.read(ZipArchive(url: $0), fileName: $0.lastPathComponent, sha256: "") }
        let checked = try SetChecks.check(members).get()
        #expect(checked.base?.fileName == "1.apk")
        #expect(checked.splits.map(\.splitName) == ["config.arm64_v8a", "config.xxhdpi"])
    }

    @Test func theBaseUnderAnotherNameCountsWhereSplitAPKsNamesIt() throws {
        // split_apks names files, so a base without the .apk extension counts; the manifest of each APK is the truth,
        // not the id. A file it names that the container lacks is named.
        let manifest = Self.manifest(Self.splitAPKs([("Sets", "config.xxhdpi"), ("config.xxhdpi.apk", "base"),
                                                     ("config.de.apk", "config.de")]))
        let (container, _) = try Self.open([("manifest.json", manifest), ("Sets", Self.base),
                                            ("config.xxhdpi.apk", Self.density)])
        let contents = try container.xapkContents()

        #expect(contents.apks.map(\.entry.name) == ["Sets", "config.xxhdpi.apk"])
        #expect(contents.ignored == [IgnoredFile(path: "config.de.apk", reason: "manifest.json names it, but the container holds no such file")])
        let unit = try ContainerExtractionTests.Unit()
        let files = try container.extract(contents.apks, into: unit.destination)
        let members = try files.map { try SetMember.read(ZipArchive(url: $0), fileName: $0.lastPathComponent, sha256: "") }
        #expect(try SetChecks.check(members).get().base?.fileName == "1.apk")
    }

    @Test func aFolderTheManifestNamesIsNoFile() throws {
        // A folder's entry, named by split_apks or by an expansion, holds no file to take.
        let folder = "Android/obb/\(Self.package)/"
        let manifest = Self.manifest(Self.splitAPKs([("splits.apk/", "config.de")])
                                     .merging(["expansions": [Self.expansion(folder, installPath: Self.obbPath(Self.main))]]) { $1 })
        let (container, _) = try Self.open([("manifest.json", manifest), ("\(Self.package).apk", Self.base),
                                            ("splits.apk/", Data()), (folder, Data())])
        let contents = try container.xapkContents()

        #expect(contents.apks.map(\.entry.name) == ["\(Self.package).apk"])
        #expect(contents.obbs.isEmpty)
        let reason = "manifest.json names it, but the container holds no such file"
        #expect(contents.ignored == [IgnoredFile(path: "splits.apk/", reason: reason), IgnoredFile(path: folder, reason: reason)])
    }

    @Test func everyAPKCountsAlsoWhereSplitAPKsIsMissing() throws {
        // An OBB variant names no split_apks; the APK is any .apk entry, as in a loose set, hidden ones and macOS's
        // copies aside.
        let (container, _) = try Self.open([("manifest.json", Self.manifest()), ("\(Self.package).apk", Self.base),
                                            ("extra/config.xxhdpi.apk", Self.density), (".hidden.apk", Self.base),
                                            ("__MACOSX/._\(Self.package).apk", Self.base)])
        #expect(try container.xapkContents().apks.map(\.entry.name) == ["\(Self.package).apk", "extra/config.xxhdpi.apk"])
    }

    @Test(arguments: [false, true])
    func numbersAsTextOrAsNumbersReadAlike(_ numbersAsText: Bool) throws {
        let manifest = Self.manifest(["expansions": [Self.expansion(Self.obbPath(Self.main), installPath: Self.obbPath(Self.main))]],
                                     numbersAsText: numbersAsText)
        let (container, _) = try Self.open([("manifest.json", manifest), ("\(Self.package).apk", Self.base),
                                            (Self.obbPath(Self.main), Data("obb".utf8))])
        let contents = try container.xapkContents()

        #expect(contents.apks.map(\.entry.name) == ["\(Self.package).apk"])
        #expect(contents.obbs.map(\.item.fileName) == [Self.main])
        #expect(contents.ignored.isEmpty)
    }

    @Test func anXAPKWithoutAnAPKIsRefused() throws {
        let (container, _) = try Self.open([("manifest.json", Self.manifest()), (Self.obbPath(Self.main), Data("obb".utf8))])
        #expect(throws: ContainerError.noAPK) { try container.xapkContents() }
    }

    @Test func anXAPKOverItsAPKsIsRefusedCountingThoseSplitAPKsNames() throws {
        // 511 .apk entries and 2 files only split_apks names: 513 APKs.
        let named = [("first", "config.a"), ("second", "config.b")]
        let apks = (0..<Container.maxAPKs - 1).map { ("split_\($0).apk", Data()) }
        let (container, _) = try Self.open([("manifest.json", Self.manifest(Self.splitAPKs(named)))] + apks
                                           + named.map { ($0.0, Data()) }, method: .stored)
        #expect(throws: ContainerError.overCap("it holds 513 APKs, more than the 512 Minato takes from a container")) {
            try container.xapkContents()
        }
    }

    // MARK: - manifest.json

    @Test func aManifestOverItsCapIsRefusedAndOneAtItIsRead() throws {
        // A manifest.json of `size` bytes, its name padded.
        let empty = Self.manifest(["name": ""]).count
        func padded(_ size: Int) -> Data { Self.manifest(["name": String(repeating: "a", count: size - empty)]) }
        let atCap = padded(Int(Container.maxJSONSize)), over = padded(Int(Container.maxJSONSize) + 1)
        #expect(atCap.count == Int(Container.maxJSONSize))
        let (read, _) = try Self.open([("manifest.json", atCap), ("\(Self.package).apk", Self.base)])
        #expect(try read.xapkContents().apks.count == 1)
        let (container, _) = try Self.open([("manifest.json", over), ("\(Self.package).apk", Self.base)])
        #expect(throws: ContainerError.overCap("its manifest.json has \(over.count) bytes, more than the 1048576 Minato reads")) {
            try container.xapkContents()
        }
    }

    @Test(arguments: ["{", "[]", #""text""#, "null"])
    func aManifestThatIsNoObjectNamesNothingAndIsNamed(_ text: String) throws {
        // Its lists of files are missing, so the container's own files count: the APKs and what lies in
        // Android/obb/<package>/.
        let (container, _) = try Self.open([("manifest.json", Data(text.utf8)), ("\(Self.package).apk", Self.base),
                                            (Self.obbPath(Self.main), Data("obb".utf8))])
        let contents = try container.xapkContents()

        #expect(contents.apks.map(\.entry.name) == ["\(Self.package).apk"])
        #expect(contents.obbs.map(\.item.fileName) == [Self.main])
        #expect(contents.ignored == [IgnoredFile(path: "manifest.json",
                                                 reason: "it is no JSON object, so only the container’s own files count")])
    }

    @Test func listsOfAnotherShapeNameNothing() throws {
        // split_apks or expansions that are no list, and elements without a file as text, name no file.
        let manifest = Self.manifest(["split_apks": ["file": "x.apk"],
                                      "expansions": [["file": 7], ["install_path": Self.obbPath(Self.main)], "text"]])
        let (container, _) = try Self.open([("manifest.json", manifest), ("\(Self.package).apk", Self.base),
                                            ("x", Self.base)])
        let contents = try container.xapkContents()

        #expect(contents.apks.map(\.entry.name) == ["\(Self.package).apk"])
        #expect(contents.obbs.isEmpty)
        #expect(contents.ignored.isEmpty)
    }

    // MARK: - The OBBs (decision 4)

    @Test func anOBBXAPKGivesItsAPKAndItsOBBsUnderTheirNames() throws {
        // The expansion's install_path gives the name; patch lies in Android/obb/<package>/ without an expansion.
        // The expansions come first, then the archive's order.
        let manifest = Self.manifest(["expansions": [Self.expansion("data/main.obb", installPath: Self.obbPath(Self.main))]])
        let (container, _) = try Self.open([("manifest.json", manifest), ("\(Self.package).apk", Self.base),
                                            (Self.obbPath(Self.patch), Data("patch".utf8)), ("data/main.obb", Data("main".utf8)),
                                            ("icon.png", Self.icon)])
        let contents = try container.xapkContents()

        #expect(contents.apks.map(\.fileName) == ["1.apk"])
        #expect(contents.obbs.map(\.item.entry.name) == ["data/main.obb", Self.obbPath(Self.patch)])
        #expect(contents.obbs.map(\.item.fileName) == [Self.main, Self.patch])
        #expect(contents.ignored.isEmpty)
        let unit = try ContainerExtractionTests.Unit()
        let files = try container.extract(contents.apks + contents.obbs.map(\.item), into: unit.destination)
        #expect(files.map(\.lastPathComponent) == ["1.apk", Self.main, Self.patch])
        #expect(try files.map { try Data(contentsOf: $0) } == [Self.base, Data("main".utf8), Data("patch".utf8)])
    }

    @Test func anExpansionWithoutAnInstallPathGoesByItsFile() throws {
        let manifest = Self.manifest(["expansions": [Self.expansion(Self.obbPath(Self.main), installPath: nil),
                                                     Self.expansion(Self.main, installPath: nil)]])
        let (container, _) = try Self.open([("manifest.json", manifest), ("\(Self.package).apk", Self.base),
                                            (Self.obbPath(Self.main), Data("obb".utf8)), (Self.main, Data("obb".utf8))])
        let contents = try container.xapkContents()

        #expect(contents.obbs.map(\.item.entry.name) == [Self.obbPath(Self.main)])
        #expect(contents.ignored == [IgnoredFile(path: Self.main, reason: "it is not in Android/obb/<package>/")])
    }

    @Test func anOBBOfAnotherPackageIsKeptForItsPackage() throws {
        // Which set an OBB belongs to is the import's question, once the APKs' manifests are read (step c2).
        let other = "main.3.com.other.app.obb"
        let manifest = Self.manifest(["expansions": [Self.expansion(Self.obbPath(other, in: "com.other.app"),
                                                                    installPath: Self.obbPath(other, in: "com.other.app"))]])
        let (container, _) = try Self.open([("manifest.json", manifest), ("\(Self.package).apk", Self.base),
                                            (Self.obbPath(other, in: "com.other.app"), Data("obb".utf8))])
        let contents = try container.xapkContents()

        #expect(contents.obbs.map(\.item.fileName) == [other])
        // Its name read once, so the import never reads a package from a path (the review).
        #expect(contents.obbs.map(\.name) == [OBBName(kind: .main, version: "3", package: "com.other.app")])
        #expect(contents.ignored.isEmpty)
    }

    @Test(arguments: [
        // `..` or a path from the root, whatever follows.
        (Self.obbPath("../../\(Self.main)"), "its path leaves its folder"),
        ("Android/obb/../\(Self.main)", "its path leaves its folder"),
        ("/sdcard/" + Self.obbPath(Self.main), "its path leaves its folder"),
        ("../" + Self.main, "its path leaves its folder"),
        // Anywhere but directly in Android/obb/<package>/.
        ("Android/data/\(Self.package)/files/\(Self.main)", "it is not in Android/obb/<package>/"),
        ("Android/data/\(Self.package)/\(Self.main)", "it is not in Android/obb/<package>/"),
        (Self.obbPath("sub/\(Self.main)"), "it is not in Android/obb/<package>/"),
        ("Android//obb/\(Self.package)/\(Self.main)", "it is not in Android/obb/<package>/"),
        ("android/obb/\(Self.package)/\(Self.main)", "it is not in Android/obb/<package>/"),
        // A name not of the pattern, or one whose package is not its folder's.
        (Self.obbPath("main.obb"), "its name is not main.<version>.<package>.obb or patch.<version>.<package>.obb"),
        (Self.obbPath("main.x7.\(Self.package).obb"), "its name is not main.<version>.<package>.obb or patch.<version>.<package>.obb"),
        (Self.obbPath(".\(Self.main)"), "its name is not main.<version>.<package>.obb or patch.<version>.<package>.obb"),
        (Self.obbPath(Self.main, in: "com.other.app"), "it is in Android/obb/com.other.app/, not in the folder of its package"),
    ] as [(String, String)])
    func anExpansionGoingElsewhereIsIgnoredAndNamed(_ installPath: String, _ reason: String) throws {
        // The entry it names stays out too, though it lies where an OBB may.
        let manifest = Self.manifest(["expansions": [Self.expansion(Self.obbPath(Self.main), installPath: installPath)]])
        let (container, _) = try Self.open([("manifest.json", manifest), ("\(Self.package).apk", Self.base),
                                            (Self.obbPath(Self.main), Data("obb".utf8))])
        let contents = try container.xapkContents()

        #expect(contents.obbs.isEmpty)
        #expect(contents.ignored == [IgnoredFile(path: installPath, reason: reason)])
    }

    @Test func anExpansionWhoseFileIsMissingIsNamed() throws {
        let manifest = Self.manifest(["expansions": [Self.expansion(Self.obbPath(Self.main), installPath: Self.obbPath(Self.main))]])
        let (container, _) = try Self.open([("manifest.json", manifest), ("\(Self.package).apk", Self.base)])
        let contents = try container.xapkContents()

        #expect(contents.obbs.isEmpty)
        #expect(contents.ignored == [IgnoredFile(path: Self.obbPath(Self.main),
                                                 reason: "manifest.json names it, but the container holds no such file")])
    }

    @Test func filesBesideTheOBBsAreNamedButHiddenOnesAndFoldersAreNot() throws {
        // Every other file under Android/ and every other .obb is named; hidden files, folders and macOS's copies
        // are not.
        let entries: [(String, Data)] = [
            ("manifest.json", Self.manifest()), ("\(Self.package).apk", Self.base),
            (Self.obbPath(Self.main), Data("main".utf8)),
            (Self.obbPath("readme.txt"), Data("text".utf8)),
            (Self.obbPath(Self.patch, in: "com.other.app"), Data("patch".utf8)),
            ("Android/data/\(Self.package)/files/level.bin", Data("level".utf8)),
            (Self.patch, Data("patch".utf8)),
            ("extra/MAIN.OBB", Data("main".utf8)),
            (Self.obbPath(".nomedia"), Data()),
            ("Android/obb/\(Self.package)/", Data()),
            ("__MACOSX/" + Self.obbPath("._\(Self.main)"), Data([0])), ("__MACOSX/extra.obb", Data([0])),
            ("icon.png", Self.icon),
        ]
        let (container, _) = try Self.open(entries)
        let contents = try container.xapkContents()

        #expect(contents.obbs.map(\.item.fileName) == [Self.main])
        let name = "its name is not main.<version>.<package>.obb or patch.<version>.<package>.obb"
        #expect(contents.ignored == [
            IgnoredFile(path: Self.obbPath("readme.txt"), reason: name),
            IgnoredFile(path: Self.obbPath(Self.patch, in: "com.other.app"),
                        reason: "it is in Android/obb/com.other.app/, not in the folder of its package"),
            IgnoredFile(path: "Android/data/\(Self.package)/files/level.bin", reason: "it is not in Android/obb/<package>/"),
            IgnoredFile(path: Self.patch, reason: "it is not in Android/obb/<package>/"),
            IgnoredFile(path: "extra/MAIN.OBB", reason: "it is not in Android/obb/<package>/"),
        ])
    }

    @Test func eachPackageKeepsOneMainAndOnePatch() throws {
        // Decision 5's model (the review): the first main and the first patch of a package, the expansions'
        // first; any other, of the same name or another version, is named by its file in the container.
        let main8 = "main.8.\(Self.package).obb", patch9 = "patch.9.\(Self.package).obb"
        let manifest = Self.manifest(["expansions": [Self.expansion("a.obb", installPath: Self.obbPath(Self.main)),
                                                     Self.expansion("b.obb", installPath: Self.obbPath(Self.main)),
                                                     Self.expansion("c.obb", installPath: Self.obbPath(main8)),
                                                     Self.expansion("d.obb", installPath: Self.obbPath(Self.patch))]])
        let (container, _) = try Self.open([("manifest.json", manifest), ("\(Self.package).apk", Self.base),
                                            ("a.obb", Data("a".utf8)), ("b.obb", Data("b".utf8)), ("c.obb", Data("c".utf8)),
                                            ("d.obb", Data("d".utf8)), (Self.obbPath(Self.main), Data("e".utf8)),
                                            (Self.obbPath(patch9), Data("f".utf8))])
        let contents = try container.xapkContents()

        #expect(contents.obbs.map(\.item.entry.name) == ["a.obb", "d.obb"])
        #expect(contents.obbs.map(\.item.fileName) == [Self.main, Self.patch])
        let main = "it is a second main OBB of \(Self.package)", patch = "it is a second patch OBB of \(Self.package)"
        #expect(contents.ignored == [IgnoredFile(path: "b.obb", reason: main), IgnoredFile(path: "c.obb", reason: main),
                                     IgnoredFile(path: Self.obbPath(Self.main), reason: main),
                                     IgnoredFile(path: Self.obbPath(patch9), reason: patch)])
    }

    @Test func oneEntryGivesOneOBBAtMost() throws {
        // Review: expansions naming one entry many times would take it out as many files, past decision
        // 7's cap; the first is judged, the others are named.
        let manifest = Self.manifest(["expansions": [Self.expansion("x.obb", installPath: Self.obbPath(Self.main)),
                                                     Self.expansion("x.obb", installPath: Self.obbPath(Self.patch)),
                                                     Self.expansion("x.obb", installPath: "../x")]])
        let (container, _) = try Self.open([("manifest.json", manifest), ("\(Self.package).apk", Self.base),
                                            ("x.obb", Data("x".utf8))])
        let contents = try container.xapkContents()

        #expect(contents.obbs.map(\.item.fileName) == [Self.main])
        #expect(contents.ignored == [IgnoredFile(path: "x.obb", reason: "another expansion names it already"),
                                     IgnoredFile(path: "x.obb", reason: "another expansion names it already")])
    }

    @Test func whatIsNamedIsEscapedAndCut() throws {
        // Review: a path is raw text from the manifest or the archive, so control and format characters,
        // a bidi override or a new line among them, are written out, and a long one is cut.
        let tricky = "Android/data/evil\u{202E}txt.obb\nx"
        let long = "Android/data/" + String(repeating: "a", count: 300)
        let (container, _) = try Self.open([("manifest.json", Self.manifest()), ("\(Self.package).apk", Self.base),
                                            (tricky, Data()), (long, Data())])
        let ignored = try container.xapkContents().ignored

        #expect(ignored.map(\.path) == ["Android/data/evil\\u{202E}txt.obb\\u{A}x",
                                        String(long.prefix(IgnoredFile.maxPathLength - 1)) + "…"])
        #expect(ignored.allSatisfy { $0.reason == "it is not in Android/obb/<package>/" })
        // An escape is never cut in two.
        let escapes = "Android/data/" + String(repeating: "\u{7}", count: 100)
        let (bells, _) = try Self.open([("manifest.json", Self.manifest()), ("\(Self.package).apk", Self.base), (escapes, Data())])
        let path = try #require(try bells.xapkContents().ignored.first?.path)
        #expect(path.count <= IgnoredFile.maxPathLength && path.hasSuffix("\\u{7}…"))
        // Combining marks, one character however many, count each.
        let marks = "Android/data/a" + String(repeating: "\u{301}", count: 1_000)
        let (combined, _) = try Self.open([("manifest.json", Self.manifest()), ("\(Self.package).apk", Self.base), (marks, Data())])
        let cut = try #require(try combined.xapkContents().ignored.first?.path)
        #expect(cut.unicodeScalars.count == IgnoredFile.maxPathLength)
        // A reason that quotes a folder's name is written out the same way.
        let folder = Self.obbPath(Self.main, in: "evil\u{202E}x")
        let (quoted, _) = try Self.open([("manifest.json", Self.manifest()), ("\(Self.package).apk", Self.base), (folder, Data())])
        #expect(try quoted.xapkContents().ignored.map(\.reason)
            == ["it is in Android/obb/evil\\u{202E}x/, not in the folder of its package"])
    }

    @Test func theNamedFilesAreCountedPastTheirCap() throws {
        let files = (0..<100).map { ("Android/data/\(Self.package)/\($0).bin", Data()) }
        let (container, _) = try Self.open([("manifest.json", Self.manifest()), ("\(Self.package).apk", Self.base)] + files,
                                           method: .stored)
        let ignored = try container.xapkContents().ignored

        #expect(ignored.count == IgnoredFile.maxCount)
        #expect(ignored.dropLast().map(\.path) == files.prefix(IgnoredFile.maxCount - 1).map(\.0))
        #expect(ignored.last == IgnoredFile(path: "…", reason: "\(100 - IgnoredFile.maxCount + 1) more files are left out"))
        // The cap itself names every one.
        let full = Array(files.prefix(IgnoredFile.maxCount))
        let (atCap, _) = try Self.open([("manifest.json", Self.manifest()), ("\(Self.package).apk", Self.base)] + full,
                                       method: .stored)
        #expect(try atCap.xapkContents().ignored.map(\.path) == full.map(\.0))
    }

    @Test func anAPKUnderAndroidObbIsAnAPK() throws {
        // Every .apk entry is an APK (decision 3), wherever it lies, and split_apks's files too, never an OBB.
        let manifest = Self.manifest(Self.splitAPKs([(Self.obbPath(Self.main), "base")])
                                     .merging(["expansions": [Self.expansion(Self.obbPath(Self.main), installPath: nil)]]) { $1 })
        let (container, _) = try Self.open([("manifest.json", manifest), (Self.obbPath(Self.main), Self.base),
                                            (Self.obbPath("x.apk"), Self.density)])
        let contents = try container.xapkContents()

        #expect(contents.apks.map(\.entry.name) == [Self.obbPath(Self.main), Self.obbPath("x.apk")])
        #expect(contents.obbs.isEmpty)
        #expect(contents.ignored.isEmpty)
    }

    // MARK: - OBB names

    @Test func anOBBNameFollowsAndroidsPattern() throws {
        // developer.android.com, "APK expansion files": [main|patch].<expansion-version>.<package>.obb, the package
        // as Android checks one (FrameworkParsingPackageUtils.validateName).
        let main = try #require(OBBName("main.314159.com.example.game.obb"))
        #expect(main.kind == .main)
        #expect(main.version == "314159")
        #expect(main.package == "com.example.game")
        #expect(main.fileName == "main.314159.com.example.game.obb")
        #expect(OBBName("patch.0.a.b.obb") == OBBName(kind: .patch, version: "0", package: "a.b"))
        #expect(OBBName("main.7.Com.Ex_1.obb")?.version == "7")
        #expect(OBBName("main.2147483647.a.b.obb")?.version == "2147483647")
        let long = "main.1." + String(repeating: "a", count: 221) + ".b.obb"
        #expect(OBBName(long)?.package.count == 223)
        // The package takes everything after the version.
        #expect(OBBName("main.1.a.b.obb.obb")?.package == "a.b.obb")
        for name in ["Main.1.a.b.obb", "main.1.a.b.OBB", "main..a.b.obb", "main.1a.a.b.obb", "main.-1.a.b.obb",
                     "main.1.ab.obb", "main.1.a.1b.obb", "main.1.a.b", "main.1.a b.c.obb", "main.\u{0661}.a.b.obb",
                     "patch.1..obb", "obb.1.a.b.obb", "main.1." + String(repeating: "a", count: 222) + ".b.obb",
                     ".main.1.a.b.obb", "main.1.a/b.c.obb",
                     // Review: Android writes the version from an int, so never with a leading zero or
                     // past Int32.max.
                     "main.007.a.b.obb", "main.00.a.b.obb", "main.2147483648.a.b.obb",
                     // Over a file name's 255 bytes by its version alone.
                     "main." + String(repeating: "1", count: 250) + ".a.b.obb"] {
            #expect(OBBName(name) == nil, "\(name)")
        }
    }

    // MARK: - An OBB over 4 GiB, streamed (test 4)

    @Test func anOBBOver4GiBStreamsInAndOutAsHoles() throws {
        // ZIP64 by a streamed test writer that leaves the zeros as holes, so neither memory nor disk pays for them;
        // the APK after it lies past 4 GiB too. Extraction turns a chunk of zeros into a hole as well, and the
        // checksum still runs over every byte.
        var random = SplitMix64(seed: 49)
        let head = Data((0..<65_536).map { _ in UInt8(truncatingIfNeeded: random.next()) })
        let tail = Data((0..<4_099).map { _ in UInt8(truncatingIfNeeded: random.next()) })
        let zeros: UInt64 = (1 << 32) + (1 << 29)
        let unit = try ContainerExtractionTests.Unit()
        let xapk = unit.url.appendingPathComponent("game.xapk")
        let manifest = Self.manifest(["expansions": [Self.expansion(Self.obbPath(Self.main), installPath: Self.obbPath(Self.main))]])
        try SparseZIP.write([("manifest.json", [.bytes(manifest)]),
                             (Self.obbPath(Self.main), [.bytes(head), .zeros(zeros), .bytes(tail)]),
                             ("\(Self.package).apk", [.bytes(Self.base)])], to: xapk)
        #expect(try SparseZIP.allocated(xapk) < 16 << 20)

        let container = try Container.open(xapk)
        let contents = try container.xapkContents()
        let obb = try #require(contents.obbs.first?.item)
        let size = UInt64(head.count) + zeros + UInt64(tail.count)
        #expect(obb.entry.uncompressedSize == size)
        let files = try container.extract(contents.apks + contents.obbs.map(\.item), into: unit.destination, availableSpace: { _ in .max })

        #expect(try Data(contentsOf: files[0]) == Self.base)
        let attributes = try FileManager.default.attributesOfItem(atPath: files[1].path)
        #expect((attributes[.size] as? NSNumber)?.uint64Value == size)
        #expect(try SparseZIP.allocated(files[1]) < 16 << 20)
        let handle = try FileHandle(forReadingFrom: files[1])
        defer { try? handle.close() }
        #expect(try handle.read(upToCount: head.count) == head)
        try handle.seek(toOffset: UInt64(head.count) + zeros - 3)
        #expect(try handle.read(upToCount: 3 + tail.count) == Data(count: 3) + tail)
    }
}
