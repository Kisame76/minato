import Foundation

/// An app's label as Minato's own alerts name it (ADR-0063 decision 2's permission alert), fit the way a
/// notification's title is (NotificationText, ADR-0074): a tab, a new line and a line or paragraph separator are a
/// space; the C0 and C1 controls, the bidirectional controls and the invisible characters go; spaces around it go; and
/// a label of more than `limit` characters is cut there, with "…". Nil when nothing is left, so the alert says "this
/// app" instead.
public enum AlertName {
    /// At most this many characters, as a label fits on one or two lines of an alert.
    public static let limit = 40

    public static func plain(_ label: String) -> String? {
        var scalars = String.UnicodeScalarView()
        for scalar in label.unicodeScalars {
            let value = scalar.value
            if value == 0x09 || value == 0x0A || value == 0x2028 || value == 0x2029 {
                scalars.append(" ")
            } else if !dropped(value) {
                scalars.append(scalar)
            }
        }
        let text = String(scalars).trimmingCharacters(in: .whitespaces)
        guard !text.isEmpty else { return nil }
        return text.count > limit ? String(text.prefix(limit)) + "…" : text
    }

    /// The characters NotificationText.dropped drops: controls, bidirectional controls, invisible ones.
    private static func dropped(_ c: UInt32) -> Bool {
        c < 0x20 || (0x7F...0x9F).contains(c) || (0x200B...0x200F).contains(c) || (0x2028...0x202E).contains(c)
            || (0x2060...0x206F).contains(c) || c == 0xFEFF || (0xFFF9...0xFFFB).contains(c)
    }
}
