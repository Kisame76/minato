/// The editor the guest serves, from the "start" text of APKRT_REQUEST_IME (ADR-0068 step 2b): the EditorInfo that
/// the focused view's onCreateInputConnection filled.
public struct EditorSpec: Sendable, Equatable {
    public var inputType: Int32
    public var imeOptions: Int32
    public var capsMode: Int32
    public var selectionStart: Int32
    public var selectionEnd: Int32
    public var hint: String
    /// EditorInfo's actionId, which an actionLabel's Enter performs.
    public var actionId: Int32
    /// EditorInfo's actionLabel is set.
    public var hasActionLabel: Bool

    public init(inputType: Int32, imeOptions: Int32, capsMode: Int32, selectionStart: Int32, selectionEnd: Int32,
                hint: String, actionId: Int32 = 0, hasActionLabel: Bool = false) {
        self.inputType = inputType
        self.imeOptions = imeOptions
        self.capsMode = capsMode
        self.selectionStart = selectionStart
        self.selectionEnd = selectionEnd
        self.hint = hint
        self.actionId = actionId
        self.hasActionLabel = hasActionLabel
    }
}

/// What the guest's InputMethodManager tells the host, with the serial of the connection it means (the request's
/// argument).
public enum ImeRequest: Sendable, Equatable {
    /// A served view got a connection, `serial` being new for it (a restart too).
    case start(serial: Int64, editor: EditorSpec)
    /// showSoftInput: also a tap on a field that has the focus already.
    case show(serial: Int64)
    /// hideSoftInputFromWindow, with the live serial or 0 without a connection.
    case hide(serial: Int64)
    /// The connection ended.
    case stop(serial: Int64)
    /// The served view's visible rectangle, in the window's pixels (keyboard plan a).
    case field(serial: Int64, FieldRect)
    /// The served field's text (keyboard plan b).
    case extract(serial: Int64, ExtractedField)

    /// The request of `text`, or nil for any text but these: "start\t<inputType>\t<imeOptions>\t<capsMode>\t
    /// <selStart>\t<selEnd>\t<fieldId>\t<actionId>\t<actionLabel 1 or 0>\t<hint>" in decimal (the hint last, so it may
    /// hold tabs), "show", "hide", "stop", "field\t<serial>\t<l>\t<t>\t<r>\t<b>" and "extract\t<serial>\t..."
    /// (ExtractedField), the last two naming the request's own serial.
    public init?(text: String, serial: Int64) {
        switch text {
        case "show": self = .show(serial: serial)
        case "hide": self = .hide(serial: serial)
        case "stop": self = .stop(serial: serial)
        default:
            if text.hasPrefix("extract\t") {
                let fields = text.split(separator: "\t", maxSplits: 9, omittingEmptySubsequences: false)
                guard let field = ExtractedField(fields: Array(fields.dropFirst()), serial: serial) else { return nil }
                self = .extract(serial: serial, field)
                return
            }
            if text.hasPrefix("field\t") {
                let fields = text.split(separator: "\t", omittingEmptySubsequences: false)
                let numbers = fields.dropFirst().compactMap { Int64($0) }
                guard fields.count == 6, numbers.count == 5, numbers[0] == serial else { return nil }
                self = .field(serial: serial, FieldRect(left: Double(numbers[1]), top: Double(numbers[2]),
                                                        right: Double(numbers[3]), bottom: Double(numbers[4])))
                return
            }
            let fields = text.split(separator: "\t", maxSplits: 9, omittingEmptySubsequences: false)
            guard fields.count == 10, fields[0] == "start" else { return nil }
            let numbers = fields[1...8].compactMap { Int32($0) }
            guard numbers.count == 8 else { return nil }
            self = .start(serial: serial, editor: EditorSpec(
                inputType: numbers[0], imeOptions: numbers[1], capsMode: numbers[2], selectionStart: numbers[3],
                selectionEnd: numbers[4], hint: String(fields[9]), actionId: numbers[6],
                hasActionLabel: numbers[7] != 0))
        }
    }
}

/// The keyboard UIKit shows for an editor (UIKeyboardType).
public enum KeyboardKind: Sendable, Equatable {
    case standard, ascii, url, email, numberPad, decimalPad, numbersAndPunctuation, phonePad
}

/// The return key's label (UIReturnKeyType).
public enum ReturnKeyKind: Sendable, Equatable {
    case standard, go, search, send, next, done
}

/// UITextAutocapitalizationType.
public enum Capitalization: Sendable, Equatable {
    case none, words, sentences, characters
}

/// UIKit's text input traits for an editor, as Android's keyboards read its EditorInfo: the input type's class and
/// variation choose the keyboard, its flags the capitals and suggestions, imeOptions' action the return key.
public struct KeyboardTraits: Sendable, Equatable {
    public var kind: KeyboardKind
    public var returnKey: ReturnKeyKind
    public var capitalization: Capitalization
    public var secure: Bool
    /// Autocorrection and spell checking, off where Android's keyboards suggest nothing.
    public var corrects: Bool
    /// The editor's action on the bar above the keyboard, for the pads that have no return key on an iPhone; nil for
    /// the others, whose bar has only the key that puts the keyboard away.
    public var barAction: ReturnKeyKind?
    /// The cursor starts at the text's start, where UIKit's keyboard begins a sentence (UIKeyInput's hasText false).
    public var cursorAtStart: Bool

    public init(editor: EditorSpec) {
        let type = editor.inputType
        let variation = type & SoftKeyboard.variationMask
        let isText = type & SoftKeyboard.classMask == SoftKeyboard.classText
        switch type & SoftKeyboard.classMask {
        case SoftKeyboard.classText:
            switch variation {
            case 0x10: kind = .url
            case 0x20, 0xd0: kind = .email
            case 0x90: kind = .ascii
            default: kind = .standard
            }
            secure = variation == 0x80 || variation == 0xe0
        case 0x2:
            let signed = type & 0x1000 != 0, decimal = type & 0x2000 != 0
            kind = signed ? .numbersAndPunctuation : decimal ? .decimalPad : .numberPad
            secure = variation == 0x10
        case 0x3:
            kind = .phonePad
            secure = false
        case 0x4:
            kind = .numbersAndPunctuation
            secure = false
        default:
            kind = .standard
            secure = false
        }
        if !isText {
            capitalization = .none
        } else if type & 0x1000 != 0 {
            capitalization = .characters
        } else if type & 0x2000 != 0 {
            capitalization = .words
        } else if type & 0x4000 != 0 {
            capitalization = .sentences
        } else {
            capitalization = .none
        }
        corrects = isText && ![0x10, 0x20, 0x80, 0x90, 0xd0, 0xe0].contains(variation) && type & 0x80000 == 0
        switch SoftKeyboard.action(editor) {
        case 2: returnKey = .go
        case 3: returnKey = .search
        case 4: returnKey = .send
        case 5: returnKey = .next
        case 6: returnKey = .done
        default: returnKey = .standard
        }
        barAction = [.numberPad, .decimalPad, .phonePad].contains(kind) ? returnKey : nil
        cursorAtStart = editor.selectionStart <= 0 && editor.selectionEnd <= 0
    }
}

/// What a text record asks of the connection (guest_interface.h's APKRT_TEXT_*; the app maps each to its constant).
public enum TextOp: Sendable, Equatable {
    case commit, deleteSurrounding, setSelection, performAction, sendKey
}

/// One text record for the guest's input channel (`struct apkrt_text_event`).
public struct TextRecord: Sendable, Equatable {
    public var op: TextOp
    public var arg0: Int32
    public var arg1: Int32
    public var serial: Int64
    /// At most `SoftKeyboard.recordBytes` bytes of UTF-8.
    public var text: String
}

/// What the session's text view does for a request.
public enum KeyboardCommand: Sendable, Equatable {
    /// Become first responder with these traits.
    case show(KeyboardTraits)
    /// Stay first responder with new traits (a restart while shown).
    case update(KeyboardTraits)
    /// Resign first responder.
    case hide
    /// The editor ended: resign once the release expires (`releaseExpired`) unless a start comes first, since a
    /// restart of the served view is "stop" and "start" (2b) and Android's IME stays up through it.
    case release
}

/// The host's side of the guest's input method (ADR-0068 step 2c): the editor of the last "start", whether UIKit's
/// keyboard is shown for it, and what UIKit's keyboard types as text records for that editor's connection. Show and
/// stop act only for the live serial, so a request about a connection that has since changed does nothing; hide acts
/// on any, as hideSoftInputFromWindow is the window's. A stop releases the keyboard rather than hiding it: a start
/// before the release expires keeps it up with the new editor, as Android's IME stays up while the focus goes from one
/// editor to the next, and hides it only when the focus goes to a view without one. Nothing is typed while the keyboard
/// is hidden or released.
public struct SoftKeyboard: Sendable {
    /// APKRT_TEXT_BYTES.
    public static let recordBytes = 1024
    static let classMask: Int32 = 0xf, classText: Int32 = 0x1, variationMask: Int32 = 0xff0
    static let keyEnter: Int32 = 66, keyDel: Int32 = 67, key0: Int32 = 7
    /// Jelly Bean's API level: an app that targets an older one gets Enter and the delete as keys.
    static let jellyBean = 16

    private var serial: Int64 = 0
    private var editor: EditorSpec?
    /// The live editor's text, from its last "extract" (keyboard plan b).
    private var extracted: ExtractedField?
    /// The live editor's view in the window's pixels, from its last "field" (keyboard plan a).
    public private(set) var field: FieldRect?
    public private(set) var isShown = false
    /// A stop released the shown keyboard and no start has come since.
    private var released = false

    /// The app's targetSdk.
    private let targetSdk: Int

    public init(targetSdk: Int = 35) {
        self.targetSdk = targetSdk
    }

    public mutating func handle(_ request: ImeRequest) -> KeyboardCommand? {
        switch request {
        case .start(let serial, let editor):
            self.serial = serial
            self.editor = editor
            extracted = nil
            field = nil
            if isShown { return .update(KeyboardTraits(editor: editor)) }
            guard released else { return nil }
            released = false
            isShown = true
            return .show(KeyboardTraits(editor: editor))
        case .show(let serial):
            guard let editor, serial == self.serial, !isShown else { return nil }
            isShown = true
            return .show(KeyboardTraits(editor: editor))
        case .hide:
            let wasReleased = released
            released = false
            return resign() ?? (wasReleased ? .hide : nil)
        case .stop(let serial):
            guard editor != nil, serial == self.serial else { return nil }
            editor = nil
            extracted = nil
            field = nil
            guard resign() != nil else { return nil }
            released = true
            return .release
        case .extract(let serial, let field):
            // A stale serial's text is dropped: its connection has gone.
            if editor != nil, serial == self.serial { extracted = field }
            return nil
        case .field(let serial, let rect):
            if editor != nil, serial == self.serial { field = rect }
            return nil
        }
    }

    /// The bar above the keyboard, once the live editor's text has come (keyboard plan b).
    public var bar: TextBar? { extracted.map(TextBar.init) }

    /// Whether the bar shows, with the keyboard's top without the bar at `keyboardTop` in the window's pixels: for a
    /// field that asks for no extract UI (IME_FLAG_NO_EXTRACT_UI, as SDL2's), where Android shows none, only while the
    /// keyboard covers the field, since the field shows its own text otherwise; always for the others.
    public func showsBar(keyboardTop: Double?) -> Bool {
        guard let editor, editor.imeOptions & Self.noExtractUI != 0, let field, let keyboardTop else { return true }
        return field.bottom > keyboardTop
    }

    /// EditorInfo.IME_FLAG_NO_EXTRACT_UI.
    static let noExtractUI: Int32 = 0x1000_0000

    /// A tap in the bar at `offset`, in UTF-16 units of its text: the guest's cursor goes there, in the field's own
    /// offsets, as Android's extract mode moves it (InputMethodService.onExtractedSelectionChanged); nothing for a
    /// field that cannot be edited, or before its text has come.
    public func select(at offset: Int) -> [TextRecord] {
        guard let extracted, extracted.editable, isShown else { return [] }
        let position = extracted.startOffset + Int32(min(max(offset, 0), extracted.text.utf16.count))
        return [record(.setSelection, position, position)]
    }

    /// The release's time is up: whether the keyboard is to go, as no start came since the stop.
    public mutating func releaseExpired() -> Bool {
        defer { released = false }
        return released
    }

    /// UIKit's keyboard went away without the guest asking (the user hid it, or another view took the focus).
    public mutating func userHid() {
        isShown = false
    }

    /// The system's back (ADR-0041 decision 10): the keyboard that is up takes it and goes, as Android's input method
    /// takes the first BACK; whether it did. The guest hears of it as of the user's hide, its field keeps the focus.
    public mutating func takesBack() -> Bool {
        guard isShown else { return false }
        isShown = false
        return true
    }

    /// insertText: the text as commits, each new line the return key unless the editor takes it as text, and each
    /// digit its key on the pads and without an editor (LatinIME's sendKeyCodePoint).
    public func insert(_ text: String) -> [TextRecord] {
        guard let editor, isShown else { return [] }
        if newLineIsText(editor) { return typed(text, editor) }
        var records: [TextRecord] = []
        for (index, line) in text.split(separator: "\n", omittingEmptySubsequences: false).enumerated() {
            if index > 0 { records += returnKey() }
            records += typed(String(line), editor)
        }
        return records
    }

    /// deleteBackward: one before the cursor (the guest widens it to a whole surrogate pair), or the DEL key for a
    /// view without an editor (TYPE_NULL) and an app before Jelly Bean, as LatinIME's backspace sends it.
    public func deleteBackward() -> [TextRecord] {
        guard let editor, isShown else { return [] }
        let key = editor.inputType == 0 || targetSdk < Self.jellyBean
        return key ? [record(.sendKey, Self.keyDel)] : [record(.deleteSurrounding, 1)]
    }

    /// The return key, or the action bar's button, as LatinIME's Enter: the editor's action, else a new line, which
    /// is the ENTER key for an app before Jelly Bean (sendKeyCodePoint).
    public func returnKey() -> [TextRecord] {
        guard let editor, isShown else { return [] }
        if let action = Self.enterAction(editor) { return [record(.performAction, action)] }
        return targetSdk < Self.jellyBean ? [record(.sendKey, Self.keyEnter)] : [record(.commit, 1, text: "\n")]
    }

    /// The action Enter performs (InputTypeUtils.getImeOptionsActionIdFromEditorInfo): none, so a new line, with
    /// IME_FLAG_NO_ENTER_ACTION or IME_ACTION_NONE; actionId with an actionLabel; else imeOptions' action, where
    /// IME_ACTION_UNSPECIFIED (0) is an action too, which a BaseInputConnection makes the ENTER key of.
    static func enterAction(_ editor: EditorSpec) -> Int32? {
        if editor.imeOptions & 0x4000_0000 != 0 { return nil }
        if editor.hasActionLabel { return editor.actionId }
        let action = editor.imeOptions & 0xff
        return action == 1 ? nil : action
    }

    /// The editor's IME action for the return key's label: 0 (unspecified) or 1 (none) give UIKit's plain return.
    static func action(_ editor: EditorSpec) -> Int32 {
        editor.imeOptions & 0x4000_0000 != 0 ? 0 : editor.imeOptions & 0xff
    }

    private func newLineIsText(_ editor: EditorSpec) -> Bool {
        Self.enterAction(editor) == nil && targetSdk >= Self.jellyBean
    }

    /// `text` without new lines: commits, but on the number, phone and date pads and without an editor each digit as
    /// its key, as LatinIME's sendKeyCodePoint sends digits there (its text keyboards compose them into words).
    private func typed(_ text: String, _ editor: EditorSpec) -> [TextRecord] {
        guard editor.inputType & Self.classMask != Self.classText else { return commits(text) }
        var records: [TextRecord] = []
        var rest = ""
        for character in text {
            if let digit = character.wholeNumberValue, character.isASCII {
                records += commits(rest)
                rest = ""
                records.append(record(.sendKey, Self.key0 + Int32(digit)))
            } else {
                rest.append(character)
            }
        }
        return records + commits(rest)
    }

    private func record(_ op: TextOp, _ arg0: Int32 = 0, _ arg1: Int32 = 0, text: String = "") -> TextRecord {
        TextRecord(op: op, arg0: arg0, arg1: arg1, serial: serial, text: text)
    }

    /// Commits of at most `recordBytes` each, split between characters (between scalars only for a character
    /// longer than a record).
    private func commits(_ text: String) -> [TextRecord] {
        var records: [TextRecord] = []
        var chunk = ""
        func add(_ piece: String) {
            if chunk.utf8.count + piece.utf8.count > Self.recordBytes, !chunk.isEmpty {
                records.append(record(.commit, 1, text: chunk))
                chunk = ""
            }
            chunk += piece
        }
        for character in text {
            let piece = String(character)
            if piece.utf8.count <= Self.recordBytes {
                add(piece)
            } else {
                for scalar in piece.unicodeScalars { add(String(scalar)) }
            }
        }
        if !chunk.isEmpty { records.append(record(.commit, 1, text: chunk)) }
        return records
    }

    private mutating func resign() -> KeyboardCommand? {
        guard isShown else { return nil }
        isShown = false
        return .hide
    }
}
