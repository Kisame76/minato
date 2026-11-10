import Foundation

/// The colours of an app's starting window (ADR-0076 decision 2), Android 12+'s splash screen: the colour the guest
/// framework computed from the starting activity's theme as Android 15's WM Shell does, and the icon's background
/// colour from the theme, if any. The guest sends them once per process (APKRT_REQUEST_STARTING_WINDOW); the host keeps
/// them per mode in the installed version's folder, so the next start shows them at once, and an update, whose version
/// has a folder of its own, starts from Android's default again. A line this version cannot read counts as none.
public struct StartingWindow: Sendable, Equatable {
    /// The configuration's night mode, as the session maps iOS's appearance to uiMode (GuestConfiguration.night).
    public enum Mode: String, Sendable, CaseIterable {
        case day, night

        public init(dark: Bool) { self = dark ? .night : .day }
    }

    /// 0xRRGGBB.
    public var splash: UInt32
    /// windowSplashScreenIconBackgroundColor as 0xRRGGBB; nil where the theme names none.
    public var iconBackground: UInt32?

    public init(splash: UInt32, iconBackground: UInt32? = nil) {
        self.splash = splash & 0xFF_FFFF
        self.iconBackground = iconBackground.map { $0 & 0xFF_FFFF }
    }

    /// The request's numbers: a0 the splash colour as an opaque 0xFFRRGGBB, a1 0 for no icon background or an opaque
    /// 0xFFRRGGBB; nil for anything else.
    public init?(request splash: UInt64, iconBackground: UInt64) {
        guard splash >> 24 == 0xFF else { return nil }
        guard iconBackground == 0 || iconBackground >> 24 == 0xFF else { return nil }
        self.init(splash: UInt32(truncatingIfNeeded: splash),
                  iconBackground: iconBackground == 0 ? nil : UInt32(truncatingIfNeeded: iconBackground))
    }

    /// Android 15's splash_window_background_default: #FFFFFF by day, #000000 at night.
    public static func standard(_ mode: Mode) -> StartingWindow {
        StartingWindow(splash: mode == .day ? 0xFF_FFFF : 0x00_0000)
    }

    /// "#RRGGBB" of the splash colour.
    public var hex: String { Self.hex(splash) }

    static let fileName = "starting-window"

    /// The mode's stored colours from the installed version's `folder`, or Android's default (fromCache false).
    public static func load(from folder: URL, mode: Mode) -> (window: StartingWindow, fromCache: Bool) {
        if let stored = lines(in: folder)[mode] { return (stored, true) }
        return (standard(mode), false)
    }

    /// Keeps `window` for `mode`; the other mode's line stays.
    public static func store(_ window: StartingWindow, mode: Mode, in folder: URL) throws {
        var stored = lines(in: folder)
        stored[mode] = window
        let text = Mode.allCases.compactMap { mode in
            stored[mode].map { "\(mode.rawValue) \($0.hex)" + ($0.iconBackground.map { " icon \(hex($0))" } ?? "") }
        }.map { $0 + "\n" }.joined()
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        try Data(text.utf8).write(to: folder.appendingPathComponent(fileName), options: .atomic)
    }

    /// The file's lines that parse: "<mode> #RRGGBB" with an optional " icon #RRGGBB".
    private static func lines(in folder: URL) -> [Mode: StartingWindow] {
        guard let text = try? String(contentsOf: folder.appendingPathComponent(fileName), encoding: .utf8) else {
            return [:]
        }
        var stored: [Mode: StartingWindow] = [:]
        for line in text.split(separator: "\n") {
            let words = line.split(separator: " ").map(String.init)
            guard let mode = words.first.flatMap(Mode.init(rawValue:)), words.count == 2 || words.count == 4,
                  let splash = colour(words[1])
            else { continue }
            if words.count == 4 {
                guard words[2] == "icon", let icon = colour(words[3]) else { continue }
                stored[mode] = StartingWindow(splash: splash, iconBackground: icon)
            } else {
                stored[mode] = StartingWindow(splash: splash)
            }
        }
        return stored
    }

    private static func colour(_ word: String) -> UInt32? {
        guard word.count == 7, word.first == "#" else { return nil }
        let digits = word.dropFirst()
        guard digits.allSatisfy(\.isHexDigit) else { return nil }
        return UInt32(digits, radix: 16)
    }

    private static func hex(_ value: UInt32) -> String {
        let digits = String(value & 0xFF_FFFF, radix: 16, uppercase: true)
        return "#" + String(repeating: "0", count: 6 - digits.count) + digits
    }
}
