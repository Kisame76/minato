import APKTestSupport
import Foundation
import Testing

@testable import APKKit

@Suite("ResourceTable")
struct ResourceTableTests {
    typealias Config = ARSCBuilder.Config

    @Test func resolvesDefaultString() throws {
        var builder = ARSCBuilder()
        let appName = builder.add("string", "app_name", [(.default, .string("My Game"))])
        let table = try ResourceTable(data: builder.build())

        #expect(table.string(for: appName, preferredLanguages: []) == "My Game")
    }

    @Test func picksPreferredLanguageThenDefault() throws {
        var builder = ARSCBuilder()
        let appName = builder.add("string", "app_name", [
            (.default, .string("My Game")),
            (Config(language: "de"), .string("Mein Spiel")),
            (Config(language: "fr"), .string("Mon Jeu")),
        ])
        let table = try ResourceTable(data: builder.build())

        #expect(table.string(for: appName, preferredLanguages: ["de"]) == "Mein Spiel")
        #expect(table.string(for: appName, preferredLanguages: ["de-AT"]) == "Mein Spiel")
        #expect(table.string(for: appName, preferredLanguages: ["es"]) == "My Game")
        #expect(table.string(for: appName, preferredLanguages: ["fr", "de"]) == "Mon Jeu")
    }

    @Test func fallsBackToEnglishWithoutDefault() throws {
        var builder = ARSCBuilder()
        let appName = builder.add("string", "app_name", [
            (Config(language: "fr"), .string("Mon Jeu")),
            (Config(language: "en"), .string("My Game")),
        ])
        let table = try ResourceTable(data: builder.build())

        #expect(table.string(for: appName, preferredLanguages: ["de"]) == "My Game")
    }

    @Test func followsStringReferences() throws {
        var builder = ARSCBuilder()
        let realName = builder.nextID("string")
        builder.add("string", "real_name", [(.default, .string("Real Name"))])
        let alias = builder.add("string", "app_name", [(.default, .reference(realName))])
        let table = try ResourceTable(data: builder.build())

        #expect(table.string(for: alias, preferredLanguages: []) == "Real Name")
    }

    @Test func stopsAtReferenceCycles() throws {
        var builder = ARSCBuilder()
        let first = builder.nextID("string")
        let second = first + 1
        builder.add("string", "a", [(.default, .reference(second))])
        builder.add("string", "b", [(.default, .reference(first))])
        let table = try ResourceTable(data: builder.build())

        #expect(table.string(for: first, preferredLanguages: []) == nil)
    }

    @Test func listsFileVariantsWithDensity() throws {
        var builder = ARSCBuilder()
        builder.add("string", "app_name", [(.default, .string("Game"))])
        let icon = builder.add("mipmap", "ic_launcher", [
            (Config(density: 160, sdkVersion: 4), .string("res/mipmap-mdpi-v4/ic_launcher.png")),
            (Config(density: 480, sdkVersion: 4), .string("res/mipmap-xxhdpi-v4/ic_launcher.png")),
            (Config(density: 0xFFFE, sdkVersion: 26), .string("res/mipmap-anydpi-v26/ic_launcher.xml")),
        ])
        let table = try ResourceTable(data: builder.build())
        let files = table.files(for: icon)

        #expect(files.count == 3)
        #expect(files.first { $0.config.density == 480 }?.path == "res/mipmap-xxhdpi-v4/ic_launcher.png")
        #expect(files.first { $0.config.density == 160 }?.path == "res/mipmap-mdpi-v4/ic_launcher.png")
        let adaptive = try #require(files.first { $0.path.hasSuffix(".xml") })
        #expect(adaptive.config.density == 0xFFFE)
        #expect(adaptive.config.sdkVersion == 26)
    }

    @Test func namesResources() throws {
        var builder = ARSCBuilder()
        builder.add("string", "app_name", [(.default, .string("Game"))])
        let icon = builder.add("mipmap", "ic_launcher", [(.default, .string("res/icon.png"))])
        let table = try ResourceTable(data: builder.build())

        #expect(table.name(of: icon) == "mipmap/ic_launcher")
    }

    @Test(arguments: [ARSCBuilder.EntryEncoding.offset32, .offset16, .sparse])
    func readsEntryEncodings(encoding: ARSCBuilder.EntryEncoding) throws {
        var builder = ARSCBuilder()
        builder.encoding = encoding
        let one = builder.add("string", "one", [(.default, .string("one")), (Config(language: "de"), .string("eins"))])
        let two = builder.add("string", "two", [(.default, .string("two"))])
        let three = builder.add("string", "three", [(.default, .string("three")), (Config(language: "de"), .string("drei"))])
        let table = try ResourceTable(data: builder.build())

        #expect(table.string(for: one, preferredLanguages: ["de"]) == "eins")
        #expect(table.string(for: two, preferredLanguages: ["de"]) == "two")
        #expect(table.string(for: three, preferredLanguages: ["de"]) == "drei")
    }

    @Test func readsCompactEntries() throws {
        var builder = ARSCBuilder()
        builder.compactEntries = true
        let appName = builder.add("string", "app_name", [(.default, .string("Compact Game"))])
        let color = builder.add("color", "background", [(.default, .color(0xFF11_2233))])
        let table = try ResourceTable(data: builder.build())

        #expect(table.string(for: appName, preferredLanguages: []) == "Compact Game")
        #expect(table.values(for: color).map(\.value) == [.color(0xFF11_2233)])
        #expect(table.name(of: color) == "color/background")
    }

    @Test func returnsNothingForUnknownIDs() throws {
        var builder = ARSCBuilder()
        let appName = builder.add("string", "app_name", [(.default, .string("Game"))])
        let table = try ResourceTable(data: builder.build())

        #expect(table.string(for: 0x0104_0000, preferredLanguages: []) == nil) // framework package
        #expect(table.string(for: appName + 5, preferredLanguages: []) == nil)
        #expect(table.string(for: 0x7F7F_0000, preferredLanguages: []) == nil)
        #expect(table.name(of: 0x7F7F_0000) == nil)
    }

    @Test func rejectsDataThatIsNotAResourceTable() {
        #expect(throws: APKError.self) { _ = try ResourceTable(data: Data("resources".utf8)) }
        #expect(throws: APKError.self) { _ = try ResourceTable(data: Data()) }
    }

    @Test func survivesCorruptedInput() throws {
        var builder = ARSCBuilder()
        let appName = builder.add("string", "app_name", [(.default, .string("Game")), (Config(language: "de"), .string("Spiel"))])
        let icon = builder.add("mipmap", "ic_launcher", [(Config(density: 480), .string("res/icon.png"))])
        let valid = builder.build()
        var generator = SplitMix64(seed: 0x5EED)
        for _ in 0..<2_000 {
            var corrupted = valid
            for _ in 0..<Int.random(in: 1...8, using: &generator) {
                let position = Int.random(in: 0..<corrupted.count, using: &generator)
                corrupted[position] = UInt8.random(in: 0...255, using: &generator)
            }
            // Must either parse or throw, and lookups on a parsed table must not trap either.
            if let table = try? ResourceTable(data: corrupted) {
                _ = table.string(for: appName, preferredLanguages: ["de"])
                _ = table.files(for: icon)
                _ = table.name(of: icon)
            }
        }
    }
}
