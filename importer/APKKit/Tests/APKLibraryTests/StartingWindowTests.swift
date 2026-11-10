import APKTestSupport
import Foundation
import Testing

@testable import APKLibrary

/// The starting window's colours (ADR-0076 decision 2): kept per app and mode in its installed version's folder, from
/// the guest's request; Android's default where there is none.
@Suite("Starting window")
struct StartingWindowTests {
    typealias Sandbox = LibraryTests.Sandbox

    private static func folder(_ sandbox: Sandbox, _ version: String = "v1") -> URL {
        sandbox.paths.installDirectory(for: "org.example.game").appendingPathComponent(version, isDirectory: true)
    }

    /// Android 15's splash_window_background_default: white by day, black at night.
    @Test func withoutAFileEachModeHasAndroidsDefault() throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }

        let day = StartingWindow.load(from: Self.folder(sandbox), mode: .day)
        let night = StartingWindow.load(from: Self.folder(sandbox), mode: .night)

        #expect(day.window == StartingWindow(splash: 0xFFFFFF) && !day.fromCache)
        #expect(night.window == StartingWindow(splash: 0x000000) && !night.fromCache)
        #expect(StartingWindow.Mode(dark: true) == .night && StartingWindow.Mode(dark: false) == .day)
    }

    @Test func aStoredModeIsReadBackFromTheCacheAndTheOtherKeepsItsLine() throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let folder = Self.folder(sandbox)

        try StartingWindow.store(StartingWindow(splash: 0x3DDC84), mode: .day, in: folder)
        try StartingWindow.store(StartingWindow(splash: 0x121212, iconBackground: 0xFF5722), mode: .night, in: folder)
        try StartingWindow.store(StartingWindow(splash: 0x6200EE), mode: .day, in: folder)

        let day = StartingWindow.load(from: folder, mode: .day)
        let night = StartingWindow.load(from: folder, mode: .night)
        #expect(day.window == StartingWindow(splash: 0x6200EE) && day.fromCache)
        #expect(night.window == StartingWindow(splash: 0x121212, iconBackground: 0xFF5722) && night.fromCache)
        let text = try String(contentsOf: folder.appendingPathComponent("starting-window"), encoding: .utf8)
        #expect(text == "day #6200EE\nnight #121212 icon #FF5722\n")
    }

    /// An update installs into a new version folder: its first start shows the default again, as after Android's
    /// update.
    @Test func anUpdateStartsFromTheDefault() throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }

        try StartingWindow.store(StartingWindow(splash: 0x3DDC84), mode: .day, in: Self.folder(sandbox, "v1"))

        #expect(!StartingWindow.load(from: Self.folder(sandbox, "v2"), mode: .day).fromCache)
    }

    /// A line this version cannot read is passed over, as a missing one.
    @Test func aLineThatDoesNotParseIsTheDefault() throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let folder = Self.folder(sandbox)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        try Data("day #12345\nnight #00FF00 icon nope\nevening #123456\n".utf8)
            .write(to: folder.appendingPathComponent("starting-window"))

        #expect(!StartingWindow.load(from: folder, mode: .day).fromCache)
        #expect(!StartingWindow.load(from: folder, mode: .night).fromCache)
    }

    /// APKRT_REQUEST_STARTING_WINDOW: a0 an opaque 0xFFRRGGBB, a1 0 (no icon background) or an opaque one.
    @Test func theRequestTakesOnlyOpaqueColours() {
        #expect(StartingWindow(request: 0xFF3D_DC84, iconBackground: 0) == StartingWindow(splash: 0x3DDC84))
        #expect(StartingWindow(request: 0xFF00_0000, iconBackground: 0xFFFF_5722)
            == StartingWindow(splash: 0x000000, iconBackground: 0xFF5722))
        #expect(StartingWindow(request: 0x803D_DC84, iconBackground: 0) == nil)
        #expect(StartingWindow(request: 0x3D_DC84, iconBackground: 0) == nil)
        #expect(StartingWindow(request: 0xFF3D_DC84, iconBackground: 0x10FF_5722) == nil)
        #expect(StartingWindow(request: 0x1_FF3D_DC84, iconBackground: 0) == nil)
        #expect(StartingWindow(splash: 0x0A0B0C).hex == "#0A0B0C")
    }
}
