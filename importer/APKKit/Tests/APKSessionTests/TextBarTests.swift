import Testing

@testable import APKSession

/// ADR-0068's keyboard plan b): the guest's "extract" and "field" texts of APKRT_REQUEST_IME become the bar above
/// UIKit's keyboard, which shows the served field's text with its cursor and composing region, the hint while it is
/// empty, bullets for a password, and moves the guest's cursor with setSelection where the user taps, in the field's
/// own offsets. Only the live serial's texts count.
@Suite struct TextBarTests {
    static let text: Int32 = 0x1

    private func shown(serial: Int64 = 5) -> SoftKeyboard {
        var keyboard = SoftKeyboard()
        _ = keyboard.handle(.start(serial: serial, editor: EditorSpec(
            inputType: Self.text, imeOptions: 0, capsMode: 0, selectionStart: 0, selectionEnd: 0, hint: "")))
        _ = keyboard.handle(.show(serial: serial))
        return keyboard
    }

    private func extract(_ serial: Int64 = 5, start: Int = 0, selection: (Int, Int) = (0, 0),
                         composing: (Int, Int) = (-1, -1), flags: Int = 1, hint: String = "",
                         text: String) -> ImeRequest? {
        ImeRequest(text: "extract\t\(serial)\t\(start)\t\(selection.0)\t\(selection.1)\t\(composing.0)\t"
                       + "\(composing.1)\t\(flags)\t\(hint)\t\(text)", serial: serial)
    }

    // MARK: - The texts

    @Test func anExtractKeepsItsOffsetsAndATextWithTabsAndNewLines() throws {
        let request = try #require(extract(start: 12, selection: (3, 5), composing: (1, 3), flags: 5, hint: "Name",
                                           text: "a\tb\nc d"))
        #expect(request == .extract(serial: 5, ExtractedField(
            startOffset: 12, selectionStart: 3, selectionEnd: 5, composingStart: 1, composingEnd: 3, editable: true,
            password: false, multiLine: true, hint: "Name", text: "a\tb\nc d")))
    }

    @Test func anExtractNamesItsOwnSerial() {
        #expect(ImeRequest(text: "extract\t6\t0\t0\t0\t-1\t-1\t1\t\tx", serial: 5) == nil)
    }

    @Test func anExtractWithoutAllItsNumbersIsNoRequest() {
        #expect(ImeRequest(text: "extract\t5\t0\t0\t0\t-1\t1\t\tx", serial: 5) == nil)
        #expect(ImeRequest(text: "extract\t5\t0\ta\t0\t-1\t-1\t1\t\tx", serial: 5) == nil)
    }

    @Test func anExtractPastTheCapIsNoRequest() {
        #expect(extract(text: String(repeating: "x", count: ExtractedField.textLimit)) != nil)
        #expect(extract(text: String(repeating: "x", count: ExtractedField.textLimit + 1)) == nil)
        // The cap counts UTF-16 units, as Java's characters: 2049 emoji are 4098.
        #expect(extract(text: String(repeating: "😀", count: ExtractedField.textLimit / 2 + 1)) == nil)
    }

    @Test func aFieldIsTheServedViewsRectangleInPixels() throws {
        let request = try #require(ImeRequest(text: "field\t5\t10\t20\t300\t80", serial: 5))
        #expect(request == .field(serial: 5, FieldRect(left: 10, top: 20, right: 300, bottom: 80)))
        #expect(ImeRequest(text: "field\t4\t10\t20\t300\t80", serial: 5) == nil)
        #expect(ImeRequest(text: "field\t5\t10\t20\t300", serial: 5) == nil)
    }

    @Test func theLiveSerialsFieldIsKeptUntilItsEditorEnds() throws {
        var keyboard = shown()
        _ = keyboard.handle(try #require(ImeRequest(text: "field\t5\t10\t20\t300\t80", serial: 5)))
        #expect(keyboard.field == FieldRect(left: 10, top: 20, right: 300, bottom: 80))
        _ = keyboard.handle(try #require(ImeRequest(text: "field\t4\t0\t0\t9\t9", serial: 4)))
        #expect(keyboard.field == FieldRect(left: 10, top: 20, right: 300, bottom: 80))
        _ = keyboard.handle(.stop(serial: 5))
        #expect(keyboard.field == nil)
    }

    // MARK: - The keyboard's bar

    @Test func theLiveSerialsExtractFillsTheBarAndAStaleOneIsDropped() throws {
        var keyboard = shown()
        #expect(keyboard.bar == nil)
        _ = keyboard.handle(try #require(extract(text: "hello")))
        #expect(keyboard.bar?.display == "hello")
        _ = keyboard.handle(try #require(extract(4, text: "old")))
        #expect(keyboard.bar?.display == "hello")
    }

    @Test func aNewEditorOrItsEndEmptiesTheBar() throws {
        var keyboard = shown()
        _ = keyboard.handle(try #require(extract(text: "hello")))
        _ = keyboard.handle(.start(serial: 6, editor: EditorSpec(
            inputType: Self.text, imeOptions: 0, capsMode: 0, selectionStart: 0, selectionEnd: 0, hint: "")))
        #expect(keyboard.bar == nil)
        _ = keyboard.handle(try #require(extract(6, text: "next")))
        _ = keyboard.handle(.stop(serial: 6))
        #expect(keyboard.bar == nil)
    }

    @Test func anEmptyFieldShowsItsHint() throws {
        var keyboard = shown()
        _ = keyboard.handle(try #require(extract(hint: "Message", text: "")))
        #expect(keyboard.bar?.display == "Message")
        #expect(keyboard.bar?.isHint == true)
        #expect(keyboard.bar?.cursor == 0)
    }

    @Test func aPasswordShowsBulletsWhateverItsTextIs() throws {
        var keyboard = shown()
        _ = keyboard.handle(try #require(extract(selection: (2, 2), flags: 3, text: "ab😀")))
        #expect(keyboard.bar?.display == "••••")
        #expect(keyboard.bar?.cursor == 2)
    }

    @Test func theCursorAndTheComposingRegionStayWithinTheText() throws {
        var keyboard = shown()
        _ = keyboard.handle(try #require(extract(selection: (9, 2), composing: (1, 40), text: "abc")))
        #expect(keyboard.bar?.selection == 2..<3)
        #expect(keyboard.bar?.cursor == 2)
        #expect(keyboard.bar?.composing == 1..<3)
        _ = keyboard.handle(try #require(extract(selection: (1, 1), text: "abc")))
        #expect(keyboard.bar?.selection == 1..<1)
        #expect(keyboard.bar?.composing == nil)
    }

    @Test func aTapInTheBarMovesTheCursorInTheFieldsOwnOffsets() throws {
        var keyboard = shown()
        _ = keyboard.handle(try #require(extract(start: 100, selection: (0, 0), text: "abcdef")))
        #expect(keyboard.select(at: 4) == [TextRecord(op: .setSelection, arg0: 104, arg1: 104, serial: 5, text: "")])
        // Past the text's end: at its end.
        #expect(keyboard.select(at: 99) == [TextRecord(op: .setSelection, arg0: 106, arg1: 106, serial: 5, text: "")])
    }

    @Test func aTapWithoutAnExtractMovesNothingAndOneOnTheHintGoesToTheStart() throws {
        var keyboard = shown()
        #expect(keyboard.select(at: 1).isEmpty)
        _ = keyboard.handle(try #require(extract(hint: "Message", text: "")))
        #expect(keyboard.select(at: 3) == [TextRecord(op: .setSelection, arg0: 0, arg1: 0, serial: 5, text: "")])
    }

    @Test func aReadOnlyFieldsBarTakesNoCursor() throws {
        var keyboard = shown()
        _ = keyboard.handle(try #require(extract(flags: 0, text: "abc")))
        #expect(keyboard.bar?.display == "abc")
        #expect(keyboard.select(at: 1).isEmpty)
    }

    @Test func theBarDescribesItselfWithoutItsText() throws {
        var keyboard = shown()
        _ = keyboard.handle(try #require(extract(text: "secret words")))
        let bar = try #require(keyboard.bar)
        #expect(!String(describing: bar).contains("secret"))
        let request = try #require(extract(text: "secret words"))
        #expect(!String(describing: request).contains("secret"))
    }

    @Test func theSessionChecksLineOfAnImeRequestNamesOnlyItsLength() {
        let request = SessionRequest.ime(serial: 5, text: "extract\t5\t0\t6\t6\t-1\t-1\t1\t\tsecret")
        #expect(request.checkLine == "ime(serial: 5, 31 units)")
        #expect(SessionRequest.finish.checkLine == "finish")
    }
}
