import Testing

@testable import APKSigning

/// Info.plist markers the stores write into apps they sign (docs/research/store-certificates.md, section 5).
@Suite("Installer detection")
struct InstallerTests {
    static let finalID = "io.github.kisame76.minato.ABCDE12345"

    static func plist(utis: [String] = [], schemes: [String] = [], altBundleID: Bool = false) -> [String: Any] {
        var plist: [String: Any] = [
            "CFBundleIdentifier": finalID,
            // Our own entries, which must not be mistaken for a store's.
            "UTImportedTypeDeclarations": [["UTTypeIdentifier": "com.android.package-archive"]],
        ]
        plist["UTExportedTypeDeclarations"] = utis.map { ["UTTypeIdentifier": $0, "UTTypeDescription": "AltStore Installed App"] }
        plist["CFBundleURLTypes"] = [["CFBundleURLSchemes": ["minato"]]] + schemes.map { ["CFBundleURLSchemes": [$0]] }
        if altBundleID { plist["ALTBundleIdentifier"] = "io.github.kisame76.minato" }
        return plist
    }

    @Test func sideStore070Alpha() {
        let info = Self.plist(utis: ["io.sidestore.Installed.\(Self.finalID)"], schemes: ["sidestore-\(Self.finalID)"], altBundleID: true)
        #expect(Installer.detect(infoDictionary: info) == .store(.sideStore))
    }

    @Test func sideStoreNightlyWithoutALTKeys() {
        let info = Self.plist(utis: ["io.sidestore.Installed.\(Self.finalID)"], schemes: ["sidestore-\(Self.finalID)"])
        #expect(Installer.detect(infoDictionary: info) == .store(.sideStore))
    }

    @Test func sideStoreSchemeAlone() {
        #expect(Installer.detect(infoDictionary: Self.plist(schemes: ["sidestore-\(Self.finalID)"])) == .store(.sideStore))
    }

    @Test func altStoreOnDevice() {
        let info = Self.plist(utis: ["io.altstore.Installed.\(Self.finalID)"], schemes: ["altstore-io.github.kisame76.minato"], altBundleID: true)
        #expect(Installer.detect(infoDictionary: info) == .store(.altStore))
    }

    @Test func altStoreThroughAltServerHasNoUTI() {
        let info = Self.plist(schemes: ["altstore-io.github.kisame76.minato"], altBundleID: true)
        #expect(Installer.detect(infoDictionary: info) == .store(.altStore))
    }

    @Test func altBundleIdentifierAloneMeansAltStore() {
        #expect(Installer.detect(infoDictionary: Self.plist(altBundleID: true)) == .store(.altStore))
    }

    @Test func xcodeWritesNoMarkers() {
        #expect(Installer.detect(infoDictionary: Self.plist()) == .other)
    }

    @Test func malformedValuesAreIgnored() {
        let info: [String: Any] = ["UTExportedTypeDeclarations": "x", "CFBundleURLTypes": [42, ["CFBundleURLSchemes": "sidestore-x"]]]
        #expect(Installer.detect(infoDictionary: info) == .other)
    }
}
