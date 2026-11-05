import Foundation

/// An app's label as Minato's own alerts name it (ADR-0063 decision 2's permission alert), fit the way a
/// notification's title is (NotificationText, ADR-0074): a tab, a new line and a line or paragraph separator are a
/// space; the C0 and C1 controls, the bidirectional controls and the invisible characters go, and so do tag characters
/// outside a subdivision's flag (the black flag, its tags and their cancel tag), which stays; spaces around it go; and
/// a label of more than `limit` characters is cut there, with "…". Nil when nothing is left, so the alert says "this
/// app" instead.
public enum AlertName {
    /// At most this many characters, as a label fits on one or two lines of an alert.
    public static let limit = 40

    public static func plain(_ label: String) -> String? {
        let given = Array(label.unicodeScalars)
        var scalars = String.UnicodeScalarView()
        var at = 0
        while at < given.count {
            let value = given[at].value
            if value == 0x09 || value == 0x0A || value == 0x2028 || value == 0x2029 {
                scalars.append(" ")
            } else if value == blackFlag {
                let end = tagSequenceEnd(given, from: at + 1)
                scalars.append(contentsOf: given[at..<end])
                at = end
                continue
            } else if !dropped(value) {
                scalars.append(given[at])
            }
            at += 1
        }
        let text = String(scalars).trimmingCharacters(in: .whitespaces)
        guard !text.isEmpty else { return nil }
        return text.count > limit ? String(text.prefix(limit)) + "…" : text
    }

    /// The black flag, which tag characters and their cancel tag make a subdivision's flag (an emoji tag sequence).
    private static let blackFlag: UInt32 = 0x1F3F4

    /// Where the tags from `from` end, their cancel tag included; `from` when no tag and its cancel tag follow, as
    /// NotificationText.tagSequenceEnd.
    private static func tagSequenceEnd(_ scalars: [Unicode.Scalar], from: Int) -> Int {
        var at = from
        while at < scalars.count {
            let value = scalars[at].value
            if value == 0xE007F { return at > from ? at + 1 : from }
            guard (0xE0020...0xE007E).contains(value) else { return from }
            at += 1
        }
        return from
    }

    /// The characters NotificationText.dropped drops: controls, bidirectional controls (U+061C too), invisible ones
    /// (U+034F too) and tag characters.
    private static func dropped(_ c: UInt32) -> Bool {
        c < 0x20 || (0x7F...0x9F).contains(c) || (0x200B...0x200F).contains(c) || (0x2028...0x202E).contains(c)
            || (0x2060...0x206F).contains(c) || c == 0xFEFF || (0xFFF9...0xFFFB).contains(c) || c == 0x061C
            || c == 0x034F || (0xE0000...0xE007F).contains(c)
    }
}
