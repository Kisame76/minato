/// The served field's text as the guest extracted it (ADR-0068, keyboard plan b): the "extract" text of
/// APKRT_REQUEST_IME, from InputConnection.getExtractedText and the field's updates. `text` is a window of the field's
/// own text, at most `textLimit` UTF-16 units around the selection, which starts at `startOffset` in the field; the
/// selection and the composing region are in the window, in UTF-16 units, the composing start -1 for none. A
/// password's text is bullets. It is never logged: its description names only its length.
public struct ExtractedField: Sendable, Equatable, CustomStringConvertible {
    /// The most UTF-16 units of `text`, as the guest caps it (Java's characters).
    public static let textLimit = 4096

    public var startOffset: Int32
    public var selectionStart: Int32
    public var selectionEnd: Int32
    public var composingStart: Int32
    public var composingEnd: Int32
    public var editable: Bool
    public var password: Bool
    public var multiLine: Bool
    public var hint: String
    public var text: String

    public init(startOffset: Int32, selectionStart: Int32, selectionEnd: Int32, composingStart: Int32,
                composingEnd: Int32, editable: Bool, password: Bool, multiLine: Bool, hint: String, text: String) {
        self.startOffset = startOffset
        self.selectionStart = selectionStart
        self.selectionEnd = selectionEnd
        self.composingStart = composingStart
        self.composingEnd = composingEnd
        self.editable = editable
        self.password = password
        self.multiLine = multiLine
        self.hint = hint
        self.text = text
    }

    /// The fields after "extract": "<serial>\t<startOffset>\t<selStart>\t<selEnd>\t<composingStart>\t<composingEnd>\t
    /// <flags>\t<hint>\t<text>", the flags 1 editable, 2 password, 4 multi-line; nil when the serial is not `serial`,
    /// a number is missing or the text passes the cap.
    init?(fields: [Substring], serial: Int64) {
        guard fields.count == 9, Int64(fields[0]) == serial else { return nil }
        let numbers = fields[1...6].compactMap { Int32($0) }
        guard numbers.count == 6 else { return nil }
        let text = String(fields[8])
        guard text.utf16.count <= Self.textLimit else { return nil }
        self.init(startOffset: numbers[0], selectionStart: numbers[1], selectionEnd: numbers[2],
                  composingStart: numbers[3], composingEnd: numbers[4], editable: numbers[5] & 1 != 0,
                  password: numbers[5] & 2 != 0, multiLine: numbers[5] & 4 != 0, hint: String(fields[7]), text: text)
    }

    public var description: String { "extracted text of \(text.utf16.count) units" }
}

/// What the bar above UIKit's keyboard shows (ADR-0068, keyboard plan b): the field's text, bullets for a password,
/// or its hint while it is empty; the selection, whose end is the cursor, and the composing region, in UTF-16 units of
/// `display`, kept within it. Its description names only its length.
public struct TextBar: Sendable, Equatable, CustomStringConvertible {
    public var display: String
    /// `display` is the hint, shown dimmed.
    public var isHint: Bool
    public var selection: Range<Int>
    public var cursor: Int
    public var composing: Range<Int>?
    public var multiLine: Bool
    /// A tap moves the cursor.
    public var editable: Bool

    init(_ field: ExtractedField) {
        let length = field.text.utf16.count
        multiLine = field.multiLine
        editable = field.editable
        guard length > 0 else {
            display = field.hint
            isHint = !field.hint.isEmpty
            selection = 0..<0
            cursor = 0
            composing = nil
            return
        }
        display = field.password ? String(repeating: "\u{2022}", count: length) : field.text
        isHint = false
        let clamp = { (offset: Int32) in min(max(Int(offset), 0), length) }
        let start = clamp(field.selectionStart), end = clamp(field.selectionEnd)
        selection = min(start, end)..<max(start, end)
        cursor = end
        if field.composingStart >= 0, field.composingEnd >= 0 {
            let from = clamp(field.composingStart), to = clamp(field.composingEnd)
            composing = from < to ? from..<to : (to < from ? to..<from : nil)
        } else {
            composing = nil
        }
    }

    public var description: String { "text bar of \(display.utf16.count) units" }
}

extension SessionRequest {
    /// The request as the Debug build's session check logs it: an IME request names only its text's length, since
    /// its "extract" holds what the user types.
    public var checkLine: String {
        if case .ime(let serial, let text) = self { return "ime(serial: \(serial), \(text.utf16.count) units)" }
        return "\(self)"
    }
}
