import Testing

@testable import APKSession

/// ADR-0068 step 2c: the guest's served editor ("start", "show", "hide", "stop" of APKRT_REQUEST_IME) becomes UIKit's
/// keyboard, and what UIKit's keyboard types becomes the guest's text records for that editor's connection.
@Suite struct SoftKeyboardTests {
    // EditorInfo's constants at android-15.0.0_r1 (InputType, EditorInfo).
    static let text: Int32 = 0x1, number: Int32 = 0x2, phone: Int32 = 0x3, datetime: Int32 = 0x4
    static let uri: Int32 = 0x10, email: Int32 = 0x20, password: Int32 = 0x80, visiblePassword: Int32 = 0x90
    static let webEmail: Int32 = 0xd0, webPassword: Int32 = 0xe0, numberPassword: Int32 = 0x10
    static let capCharacters: Int32 = 0x1000, capWords: Int32 = 0x2000, capSentences: Int32 = 0x4000
    static let multiLine: Int32 = 0x20000, noSuggestions: Int32 = 0x80000
    static let signed: Int32 = 0x1000, decimal: Int32 = 0x2000
    static let go: Int32 = 2, search: Int32 = 3, send: Int32 = 4, next: Int32 = 5, done: Int32 = 6, previous: Int32 = 7
    static let noEnterAction = Int32(bitPattern: 0x4000_0000)

    private func editor(_ inputType: Int32, _ imeOptions: Int32 = 0, hint: String = "") -> EditorSpec {
        EditorSpec(inputType: inputType, imeOptions: imeOptions, capsMode: 0, selectionStart: 0, selectionEnd: 0,
                   hint: hint)
    }

    private func traits(_ inputType: Int32, _ imeOptions: Int32 = 0) -> KeyboardTraits {
        KeyboardTraits(editor: editor(inputType, imeOptions))
    }

    private func shown(_ inputType: Int32 = text, _ imeOptions: Int32 = 0, serial: Int64 = 5) -> SoftKeyboard {
        var keyboard = SoftKeyboard()
        _ = keyboard.handle(.start(serial: serial, editor: editor(inputType, imeOptions)))
        _ = keyboard.handle(.show(serial: serial))
        return keyboard
    }

    // MARK: - The request's text

    @Test func startCarriesTheEditorAndAHintThatMayHoldTabs() {
        let request = ImeRequest(text: "start\t180225\t-2147483642\t8192\t3\t4\t2131230801\tName\tand\tmore",
                                 serial: 9)
        #expect(request == .start(serial: 9, editor: EditorSpec(
            inputType: 180225, imeOptions: Int32.min + 6, capsMode: 8192, selectionStart: 3, selectionEnd: 4,
            hint: "Name\tand\tmore")))
    }

    @Test func showHideAndStopCarryOnlyTheSerial() {
        #expect(ImeRequest(text: "show", serial: 3) == .show(serial: 3))
        #expect(ImeRequest(text: "hide", serial: 0) == .hide(serial: 0))
        #expect(ImeRequest(text: "stop", serial: 4) == .stop(serial: 4))
    }

    @Test func anyOtherTextIsNoRequest() {
        #expect(ImeRequest(text: "start\t1\t0\t0\t0\t0\t7", serial: 1) == nil)  // no hint field
        #expect(ImeRequest(text: "start\tx\t0\t0\t0\t0\t7\t", serial: 1) == nil)
        #expect(ImeRequest(text: "start\t4294967297\t0\t0\t0\t0\t7\t", serial: 1) == nil)
        #expect(ImeRequest(text: "show\t", serial: 1) == nil)
        #expect(ImeRequest(text: "open", serial: 1) == nil)
        #expect(ImeRequest(text: "", serial: 1) == nil)
    }

    // MARK: - EditorInfo → UIKit's traits

    @Test func textVariationsChooseTheKeyboard() {
        #expect(traits(Self.text).kind == .standard)
        #expect(traits(Self.text | Self.uri).kind == .url)
        #expect(traits(Self.text | Self.email).kind == .email)
        #expect(traits(Self.text | Self.webEmail).kind == .email)
        #expect(traits(Self.text | Self.visiblePassword).kind == .ascii)
        #expect(traits(0).kind == .standard)
    }

    @Test func numbersPhonesAndDatesChooseTheirPads() {
        #expect(traits(Self.number).kind == .numberPad)
        #expect(traits(Self.number | Self.decimal).kind == .decimalPad)
        #expect(traits(Self.number | Self.signed).kind == .numbersAndPunctuation)
        #expect(traits(Self.number | Self.signed | Self.decimal).kind == .numbersAndPunctuation)
        #expect(traits(Self.phone).kind == .phonePad)
        #expect(traits(Self.datetime).kind == .numbersAndPunctuation)
    }

    @Test func passwordsAreSecureExceptTheVisibleOne() {
        #expect(traits(Self.text | Self.password).secure)
        #expect(traits(Self.text | Self.webPassword).secure)
        #expect(traits(Self.number | Self.numberPassword).secure)
        #expect(traits(Self.number | Self.numberPassword).kind == .numberPad)
        #expect(!traits(Self.text | Self.visiblePassword).secure)
        #expect(!traits(Self.text).secure)
        #expect(!traits(Self.number).secure)
    }

    @Test func capitalizationComesFromTheTextFlags() {
        #expect(traits(Self.text).capitalization == .none)
        #expect(traits(Self.text | Self.capSentences).capitalization == .sentences)
        #expect(traits(Self.text | Self.capWords).capitalization == .words)
        #expect(traits(Self.text | Self.capCharacters | Self.capWords).capitalization == .characters)
        // A number's flags share the bits and mean signed and decimal.
        #expect(traits(Self.number | Self.signed).capitalization == .none)
    }

    @Test func correctionIsOffWhereAndroidsKeyboardsSuggestNothing() {
        #expect(traits(Self.text).corrects)
        #expect(!traits(Self.text | Self.noSuggestions).corrects)
        #expect(!traits(Self.text | Self.password).corrects)
        #expect(!traits(Self.text | Self.visiblePassword).corrects)
        #expect(!traits(Self.text | Self.uri).corrects)
        #expect(!traits(Self.text | Self.email).corrects)
        #expect(!traits(Self.number).corrects)
        #expect(!traits(0).corrects)
    }

    @Test func theReturnKeyNamesTheEditorsAction() {
        #expect(traits(Self.text, Self.go).returnKey == .go)
        #expect(traits(Self.text, Self.search).returnKey == .search)
        #expect(traits(Self.text, Self.send).returnKey == .send)
        #expect(traits(Self.text, Self.next).returnKey == .next)
        #expect(traits(Self.text, Self.done).returnKey == .done)
        #expect(traits(Self.text, Self.previous).returnKey == .standard)
        #expect(traits(Self.text, 0).returnKey == .standard)
        #expect(traits(Self.text | Self.multiLine, Self.done | Self.noEnterAction).returnKey == .standard)
    }

    @Test func onlyThePadsWithoutAReturnKeyNeedABar() {
        #expect(traits(Self.number).needsActionBar)
        #expect(traits(Self.number | Self.decimal).needsActionBar)
        #expect(traits(Self.phone).needsActionBar)
        #expect(!traits(Self.number | Self.signed).needsActionBar)
        #expect(!traits(Self.text).needsActionBar)
    }

    @Test func theCursorAtTheStartBeginsASentence() {
        #expect(traits(Self.text).cursorAtStart)
        var later = editor(Self.text)
        later.selectionStart = 3
        later.selectionEnd = 3
        #expect(!KeyboardTraits(editor: later).cursorAtStart)
        // A field without a known selection (-1) starts as at its start.
        later.selectionStart = -1
        later.selectionEnd = -1
        #expect(KeyboardTraits(editor: later).cursorAtStart)
    }

    // MARK: - Requests → what the view does

    @Test func showActsOnlyForTheLiveEditor() {
        var keyboard = SoftKeyboard()
        #expect(keyboard.handle(.show(serial: 1)) == nil)
        #expect(keyboard.handle(.start(serial: 2, editor: editor(Self.text))) == nil)
        #expect(keyboard.handle(.show(serial: 1)) == nil)
        #expect(keyboard.handle(.show(serial: 2)) == .show(traits(Self.text)))
        #expect(keyboard.isShown)
    }

    @Test func aShowWhileShownChangesNothingSoATapOnTheFocusedFieldIsHarmless() {
        var keyboard = shown()
        #expect(keyboard.handle(.show(serial: 5)) == nil)
        #expect(keyboard.isShown)
    }

    @Test func aShowAfterTheUserHidTheKeyboardShowsItAgain() {
        var keyboard = shown()
        keyboard.userHid()
        #expect(!keyboard.isShown)
        #expect(keyboard.handle(.show(serial: 5)) == .show(traits(Self.text)))
    }

    @Test func aRestartWhileShownSwapsTheTraitsAndTheSerialWithoutAHide() {
        var keyboard = shown()
        #expect(keyboard.handle(.start(serial: 6, editor: editor(Self.number))) == .update(traits(Self.number)))
        #expect(keyboard.isShown)
        #expect(keyboard.insert("1") == [TextRecord(op: .commit, arg0: 1, arg1: 0, serial: 6, text: "1")])
    }

    @Test func hideResignsWhateverItsSerial() {
        var keyboard = shown()
        #expect(keyboard.handle(.hide(serial: 0)) == .hide)
        #expect(!keyboard.isShown)
        #expect(keyboard.handle(.hide(serial: 5)) == nil)
        // The editor stays: a later show of it works.
        #expect(keyboard.handle(.show(serial: 5)) == .show(traits(Self.text)))
    }

    @Test func stopEndsOnlyTheLiveEditor() {
        var keyboard = shown()
        #expect(keyboard.handle(.stop(serial: 4)) == nil)
        #expect(keyboard.isShown)
        #expect(keyboard.handle(.stop(serial: 5)) == .release)
        #expect(!keyboard.isShown)
        #expect(keyboard.handle(.show(serial: 5)) == nil)
        #expect(keyboard.insert("a").isEmpty)
    }

    /// 2b: a restart of the served view (a tap on it, its window's focus) is stop and start, and Android's IME stays up
    /// through it with the new editor; a show after it changes nothing.
    @Test func aRestartKeepsTheKeyboardUpWithTheNewEditor() {
        var keyboard = shown()
        #expect(keyboard.handle(.stop(serial: 5)) == .release)
        #expect(keyboard.handle(.start(serial: 6, editor: editor(Self.number))) == .show(traits(Self.number)))
        #expect(keyboard.isShown)
        let expired1 = keyboard.releaseExpired()
        #expect(!expired1)
        #expect(keyboard.handle(.show(serial: 6)) == nil)
        #expect(keyboard.insert("7") == [TextRecord(op: .commit, arg0: 1, arg1: 0, serial: 6, text: "7")])
    }

    /// A stop that no start follows (the focus went to a view without an editor) hides the keyboard once the release
    /// expires, and a later start does not bring it back.
    @Test func aStopWithoutAStartHidesWhenTheReleaseExpires() {
        var keyboard = shown()
        #expect(keyboard.handle(.stop(serial: 5)) == .release)
        let expired2 = keyboard.releaseExpired()
        #expect(expired2)
        let expired3 = keyboard.releaseExpired()
        #expect(!expired3)
        #expect(keyboard.handle(.start(serial: 6, editor: editor(Self.text))) == nil)
        #expect(!keyboard.isShown)
    }

    @Test func aHideDuringTheReleaseHidesAtOnce() {
        var keyboard = shown()
        #expect(keyboard.handle(.stop(serial: 5)) == .release)
        #expect(keyboard.handle(.hide(serial: 0)) == .hide)
        let expired4 = keyboard.releaseExpired()
        #expect(!expired4)
        #expect(keyboard.handle(.start(serial: 6, editor: editor(Self.text))) == nil)
    }

    @Test func aStopWhileHiddenForgetsTheEditorQuietly() {
        var keyboard = SoftKeyboard()
        _ = keyboard.handle(.start(serial: 2, editor: editor(Self.text)))
        #expect(keyboard.handle(.stop(serial: 2)) == nil)
        #expect(keyboard.handle(.show(serial: 2)) == nil)
    }

    // MARK: - UIKit's keyboard → text records

    @Test func typedTextIsCommittedForTheLiveSerial() {
        let keyboard = shown()
        #expect(keyboard.insert("héllo 🙂") == [TextRecord(op: .commit, arg0: 1, arg1: 0, serial: 5, text: "héllo 🙂")])
    }

    @Test func nothingIsTypedWithoutAShownKeyboard() {
        var keyboard = SoftKeyboard()
        _ = keyboard.handle(.start(serial: 2, editor: editor(Self.text)))
        #expect(keyboard.insert("a").isEmpty)
        #expect(keyboard.deleteBackward().isEmpty)
        #expect(keyboard.returnKey().isEmpty)
    }

    @Test func longTextIsSplitAtCharactersWithinTheRecordsBytes() {
        let keyboard = shown()
        // One byte first, so the two-byte characters reach the limit exactly: a record of 1024 bytes and not one more.
        let long = "a" + String(repeating: "é", count: 600) + String(repeating: "👍🏽", count: 200)
        let records = keyboard.insert(long)
        #expect(records.count > 1)
        #expect(records.allSatisfy { $0.op == .commit && $0.arg0 == 1 && $0.serial == 5 })
        #expect(records.allSatisfy { $0.text.utf8.count <= SoftKeyboard.recordBytes })
        #expect(records.first?.text.utf8.count == SoftKeyboard.recordBytes - 1)
        #expect(records.map(\.text).joined() == long)
    }

    @Test func deleteBackwardDeletesOneBeforeTheCursor() {
        #expect(shown().deleteBackward() == [TextRecord(op: .deleteSurrounding, arg0: 1, arg1: 0, serial: 5, text: "")])
    }

    @Test func aFieldWithoutAnEditorGetsKeys() {
        let keyboard = shown(0)
        #expect(keyboard.deleteBackward() == [TextRecord(op: .sendKey, arg0: 67, arg1: 0, serial: 5, text: "")])
        #expect(keyboard.returnKey() == [TextRecord(op: .sendKey, arg0: 66, arg1: 0, serial: 5, text: "")])
        #expect(keyboard.insert("a\nb") == [
            TextRecord(op: .commit, arg0: 1, arg1: 0, serial: 5, text: "a"),
            TextRecord(op: .sendKey, arg0: 66, arg1: 0, serial: 5, text: ""),
            TextRecord(op: .commit, arg0: 1, arg1: 0, serial: 5, text: "b"),
        ])
    }

    @Test func returnPerformsTheEditorsAction() {
        #expect(shown(Self.text, Self.search).returnKey() == [
            TextRecord(op: .performAction, arg0: Self.search, arg1: 0, serial: 5, text: "")])
        #expect(shown(Self.text, Self.previous).returnKey() == [
            TextRecord(op: .performAction, arg0: Self.previous, arg1: 0, serial: 5, text: "")])
    }

    @Test func returnWithoutAnActionIsANewLine() {
        let newline = [TextRecord(op: .commit, arg0: 1, arg1: 0, serial: 5, text: "\n")]
        #expect(shown(Self.text, 0).returnKey() == newline)
        #expect(shown(Self.text, 1).returnKey() == newline)
        #expect(shown(Self.text | Self.multiLine, Self.done | Self.noEnterAction).returnKey() == newline)
    }

    @Test func uikitsNewLineIsTheReturnKey() {
        #expect(shown(Self.text, Self.go).insert("ab\ncd") == [
            TextRecord(op: .commit, arg0: 1, arg1: 0, serial: 5, text: "ab"),
            TextRecord(op: .performAction, arg0: Self.go, arg1: 0, serial: 5, text: ""),
            TextRecord(op: .commit, arg0: 1, arg1: 0, serial: 5, text: "cd"),
        ])
        #expect(shown(Self.text | Self.multiLine, Self.noEnterAction).insert("a\nb") == [
            TextRecord(op: .commit, arg0: 1, arg1: 0, serial: 5, text: "a\nb")])
    }
}
