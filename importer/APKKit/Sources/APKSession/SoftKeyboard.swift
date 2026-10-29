/// The editor the guest serves, from the "start" text of APKRT_REQUEST_IME (ADR-0068 step 2b): the EditorInfo that
/// the focused view's onCreateInputConnection filled.
public struct EditorSpec: Sendable, Equatable {
    public var inputType: Int32
    public var imeOptions: Int32
    public var capsMode: Int32
    public var selectionStart: Int32
    public var selectionEnd: Int32
    public var hint: String

    public init(inputType: Int32, imeOptions: Int32, capsMode: Int32, selectionStart: Int32, selectionEnd: Int32,
                hint: String) {
        self.inputType = inputType
        self.imeOptions = imeOptions
        self.capsMode = capsMode
        self.selectionStart = selectionStart
        self.selectionEnd = selectionEnd
        self.hint = hint
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

    /// The request of `text`, or nil for any text but the four: "start\t<inputType>\t<imeOptions>\t<capsMode>\t
    /// <selStart>\t<selEnd>\t<fieldId>\t<hint>" in decimal (the hint last, so it may hold tabs), "show", "hide" and
    /// "stop".
    public init?(text: String, serial: Int64) {
        switch text {
        case "show": self = .show(serial: serial)
        case "hide": self = .hide(serial: serial)
        case "stop": self = .stop(serial: serial)
        default:
            let fields = text.split(separator: "\t", maxSplits: 7, omittingEmptySubsequences: false)
            guard fields.count == 8, fields[0] == "start" else { return nil }
            let numbers = fields[1...6].compactMap { Int32($0) }
            guard numbers.count == 6 else { return nil }
            self = .start(serial: serial, editor: EditorSpec(
                inputType: numbers[0], imeOptions: numbers[1], capsMode: numbers[2], selectionStart: numbers[3],
                selectionEnd: numbers[4], hint: String(fields[7])))
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
    /// The pads that have no return key on an iPhone get a bar with the editor's action above them.
    public var needsActionBar: Bool
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
        needsActionBar = [.numberPad, .decimalPad, .phonePad].contains(kind)
        cursorAtStart = editor.selectionStart <= 0 && editor.selectionEnd <= 0
    }
}

/// What a text record asks of the connection (guest_interface.h's APKRT_TEXT_*; the app maps each to its constant).
public enum TextOp: Sendable, Equatable {
    case commit, deleteSurrounding, performAction, sendKey
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
    static let keyEnter: Int32 = 66, keyDel: Int32 = 67

    private var serial: Int64 = 0
    private var editor: EditorSpec?
    public private(set) var isShown = false
    /// A stop released the shown keyboard and no start has come since.
    private var released = false

    public init() {}

    public mutating func handle(_ request: ImeRequest) -> KeyboardCommand? {
        switch request {
        case .start(let serial, let editor):
            self.serial = serial
            self.editor = editor
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
            guard resign() != nil else { return nil }
            released = true
            return .release
        }
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

    /// insertText: the text as commits, each new line the return key unless the editor takes it as text.
    public func insert(_ text: String) -> [TextRecord] {
        guard let editor, isShown else { return [] }
        if newLineIsText(editor) { return commits(text) }
        var records: [TextRecord] = []
        for (index, line) in text.split(separator: "\n", omittingEmptySubsequences: false).enumerated() {
            if index > 0 { records += returnKey() }
            records += commits(String(line))
        }
        return records
    }

    /// deleteBackward: one before the cursor (the guest widens it to a whole surrogate pair), or the DEL key for a
    /// view without an editor (TYPE_NULL), as Android's keyboard sends it.
    public func deleteBackward() -> [TextRecord] {
        guard let editor, isShown else { return [] }
        return editor.inputType == 0 ? [record(.sendKey, Self.keyDel)] : [record(.deleteSurrounding, 1)]
    }

    /// The return key, or the action bar's button: the editor's action, else a new line (LatinIME's
    /// sendKeyCodePoint), or the ENTER key for a view without an editor.
    public func returnKey() -> [TextRecord] {
        guard let editor, isShown else { return [] }
        if editor.inputType == 0 { return [record(.sendKey, Self.keyEnter)] }
        if newLineIsText(editor) { return [record(.commit, 1, text: "\n")] }
        return [record(.performAction, Self.action(editor))]
    }

    /// The editor's IME action, 0 (unspecified) or 1 (none) when the return key is to be a new line.
    static func action(_ editor: EditorSpec) -> Int32 {
        editor.imeOptions & 0x4000_0000 != 0 ? 0 : editor.imeOptions & 0xff
    }

    private func newLineIsText(_ editor: EditorSpec) -> Bool {
        editor.inputType != 0 && Self.action(editor) <= 1
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
