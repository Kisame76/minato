import APKKit
import Foundation
import Testing

@testable import APKLibrary

/// ADR-0050 c2b: a feature module the version installs as splits lies in local_testing too, as bundletool's
/// local-testing mode puts it, so that feature-delivery's fake answers a request for it with INSTALLED as Android
/// does, instead of MODULE_UNAVAILABLE. The module's master split is `<module>-master.apk`, a configuration split of
/// it `<module>-<suffix>.apk`; the base's configuration splits are no module's.
@Suite("Feature module layout")
struct ModuleLayoutTests {
    typealias Sandbox = LibraryTests.Sandbox

    static func file(_ name: String, split: String?, kind: PackageFile.Kind, configFor: String? = nil) -> PackageFile {
        PackageFile(fileName: name, splitName: split, size: UInt64(name.utf8.count), sha256: "0", kind: kind,
                    configForSplit: configFor)
    }

    /// An installed set of a base, a feature module "extra" with a configuration split, and a language split of the
    /// base.
    static func entry(_ sandbox: Sandbox, module: Bool = true) throws -> LibraryEntry {
        var splits = [file("split_config.en.apk", split: "config.en", kind: .config)]
        if module {
            splits += [file("split_extra.apk", split: "extra", kind: .feature),
                       file("split_extra.config.arm64_v8a.apk", split: "extra.config.arm64_v8a", kind: .config,
                            configFor: "extra")]
        }
        let entry = LibraryEntry(
            id: "org.example.modules", displayName: "modules", state: .ready, report: nil,
            base: file("base.apk", split: nil, kind: .base), splits: splits, obbs: [],
            source: SourceFile(fileName: "modules.apks", size: 1, modifiedMilliseconds: 0), iconFile: nil,
            importedAt: Date(timeIntervalSince1970: 0), folder: "0123456789abcdef")
        let version = sandbox.paths.versionDirectory(for: entry)
        try FileManager.default.createDirectory(at: version, withIntermediateDirectories: true)
        for member in [entry.base!] + splits {
            try Data("bytes of \(member.fileName)".utf8).write(to: version.appendingPathComponent(member.fileName))
        }
        try sandbox.paths.prepareData(for: entry.id)
        return entry
    }

    static func localTesting(_ entry: LibraryEntry, _ sandbox: Sandbox) -> URL {
        PackLayoutTests.localTesting(entry, sandbox)
    }

    /// The module's splits lie in local_testing under bundletool's names, read-only, and the base's language split does
    /// not.
    @Test func aModuleLiesInLocalTestingUnderBundletoolsNames() throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let entry = try Self.entry(sandbox)
        #expect(entry.usesLocalTesting)
        try sandbox.paths.layOutPacks(of: entry)
        let folder = Self.localTesting(entry, sandbox)
        #expect(try PackLayoutTests.contents(folder) == ["extra-arm64_v8a.apk", "extra-master.apk"])
        let version = sandbox.paths.versionDirectory(for: entry)
        let clones = [("extra-master.apk", "split_extra.apk"),
                      ("extra-arm64_v8a.apk", "split_extra.config.arm64_v8a.apk")]
        for (name, original) in clones {
            let clone = folder.appendingPathComponent(name)
            #expect(try Data(contentsOf: clone) == Data(contentsOf: version.appendingPathComponent(original)))
            #expect(try LibraryTests.permissions(clone) == 0o444)
        }
    }

    /// A name that would leave local_testing, or is no name, and a second split of one local name, leave their splits
    /// out (the same rule as the pack slices'), as does a configuration split that does not follow its module's name.
    @Test func namesThatAreNoFileNamesAreLeftOut() {
        func split(_ name: String, _ kind: PackageFile.Kind, configFor: String? = nil) -> PackageFile {
            Self.file("\(name).apk", split: name, kind: kind, configFor: configFor)
        }
        var entry = LibraryEntry(
            id: "org.example.modules", displayName: "modules", state: .ready, report: nil, base: nil,
            splits: [split("extra", .feature), split("extra.config.x/../y", .config, configFor: "extra"),
                     split("extra.config.", .config, configFor: "extra"),
                     split("config.en", .config, configFor: "extra"),
                     split("extra.config.en", .config, configFor: "extra"),
                     split("extra.config.en", .config, configFor: "extra"), split("../up", .feature),
                     split("a/b", .feature), split("extra2.config.en", .config, configFor: "extra2")],
            obbs: [], source: SourceFile(fileName: "modules.apks", size: 1, modifiedMilliseconds: 0), iconFile: nil,
            importedAt: Date(timeIntervalSince1970: 0))
        #expect(entry.moduleSlices.map(\.localName) == ["extra-master.apk", "extra-en.apk"])
        entry.splits = []
        #expect(!entry.usesLocalTesting)
    }

    /// A set without a feature module is as before: no folder, no meta-data (ADR-0050 decision 4).
    @Test func aSetWithoutAModuleLaysOutNothing() throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let entry = try Self.entry(sandbox, module: false)
        #expect(!entry.usesLocalTesting)
        try sandbox.paths.layOutPacks(of: entry)
        #expect(!sandbox.exists(Self.localTesting(entry, sandbox)))
    }
}
