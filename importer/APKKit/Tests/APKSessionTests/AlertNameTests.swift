import Testing

@testable import APKSession

/// An app's label as Minato's own alerts name it (ADR-0063 decision 2's permission alert): the same characters go as in
/// a notification's title (NotificationText, ADR-0074), so that a label cannot make the question read differently.
@Suite struct AlertNameTests {
    @Test func aPlainLabelStays() {
        #expect(AlertName.plain("Pocket Garden") == "Pocket Garden")
        #expect(AlertName.plain("Café ☕️ 日本") == "Café ☕️ 日本")
    }

    @Test func controlsAndBidirectionalOverridesGo() {
        #expect(AlertName.plain("Evil\u{202E}txt.exe") == "Eviltxt.exe")
        #expect(AlertName.plain("A\u{200B}B\u{2066}C\u{2069}\u{FEFF}") == "ABC")
        #expect(AlertName.plain("A\u{0007}B\u{0085}C\u{007F}") == "ABC")
    }

    @Test func lineBreaksAndTabsAreSpaces() {
        #expect(AlertName.plain("One\nTwo\tThree\u{2028}Four") == "One Two Three Four")
        #expect(AlertName.plain("  Spaced  \n") == "Spaced")
    }

    @Test func aLongLabelIsCut() {
        let long = String(repeating: "x", count: 200)
        #expect(AlertName.plain(long) == String(repeating: "x", count: AlertName.limit) + "…")
        let emoji = String(repeating: "👩‍👩‍👧", count: 100)
        #expect(AlertName.plain(emoji)?.count == AlertName.limit + 1)
    }

    @Test func nothingLeftIsNone() {
        #expect(AlertName.plain("\u{202E}\u{200B}\n ") == nil)
        #expect(AlertName.plain("") == nil)
    }
}
