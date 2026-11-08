import APKSession
import UIKit

/// The bar above UIKit's keyboard (ADR-0068, keyboard plan b): the served field's text as the guest extracted it, with
/// its cursor, selection and composing region, the hint dimmed while it is empty and bullets for a password, as
/// Android's extract mode shows a field the keyboard covers; a tap in it moves the guest's cursor there. Typing still
/// goes through SoftKeyboardView, so the guest owns the text and the bar redraws from its next extract. The pads, which
/// have no return key, have the editor's action at the bar's right; a field whose keyboard a tap outside keeps up has
/// Apple's key that hides the keyboard at the far right (KeyboardTraits.hideKey). The text sits on glass of its own,
/// since UIKit's keyboard leaves an accessory view's background clear and the app shows through. Its text is never
/// logged.
final class KeyboardBarView: UIInputView {
    /// A tap in the text, at this offset in UTF-16 units of the bar's text.
    var onSelect: (Int) -> Void = { _ in }

    private let surface = UIVisualEffectView(effect: UIGlassEffect())
    private let text = UITextView()
    private let caret = UIView()
    private var bar: TextBar?

    static func height(multiLine: Bool) -> CGFloat { multiLine ? 88 : 52 }
    /// The caret's and the selection's color: iOS's blue, as a text field's, since the keyboard's window tints its
    /// accessory views white, which the glass hides.
    private static let accent = UIColor.systemBlue

    init(action: ReturnKeyKind?, hideKey: Bool, onReturn: @escaping () -> Void, onHide: @escaping () -> Void) {
        super.init(frame: CGRect(x: 0, y: 0, width: 320, height: Self.height(multiLine: false)),
                   inputViewStyle: .keyboard)
        autoresizingMask = .flexibleHeight
        text.isEditable = false
        text.isSelectable = false
        text.backgroundColor = .clear
        text.font = .preferredFont(forTextStyle: .body)
        text.adjustsFontForContentSizeCategory = true
        text.textContainerInset = UIEdgeInsets(top: 6, left: 10, bottom: 6, right: 10)
        text.addGestureRecognizer(UITapGestureRecognizer(target: self, action: #selector(tapped(_:))))
        text.translatesAutoresizingMaskIntoConstraints = false
        surface.layer.cornerRadius = 18
        surface.layer.cornerCurve = .continuous
        surface.clipsToBounds = true
        surface.translatesAutoresizingMaskIntoConstraints = false
        addSubview(surface)
        surface.contentView.addSubview(text)
        caret.backgroundColor = Self.accent
        caret.isUserInteractionEnabled = false
        text.addSubview(caret)

        var constraints = [
            surface.topAnchor.constraint(equalTo: topAnchor, constant: 4),
            surface.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -4),
            surface.leadingAnchor.constraint(equalTo: safeAreaLayoutGuide.leadingAnchor, constant: 8),
            text.topAnchor.constraint(equalTo: surface.contentView.topAnchor),
            text.bottomAnchor.constraint(equalTo: surface.contentView.bottomAnchor),
            text.leadingAnchor.constraint(equalTo: surface.contentView.leadingAnchor),
            text.trailingAnchor.constraint(equalTo: surface.contentView.trailingAnchor),
        ]
        // From the right: the hide key, then the editor's action, then the text's glass.
        var trailing = safeAreaLayoutGuide.trailingAnchor
        if hideKey {
            let image = UIImage(systemName: "keyboard.chevron.compact.down")
            let button = UIButton(configuration: .glass(), primaryAction: UIAction(image: image) { _ in onHide() })
            button.accessibilityLabel = "Hide Keyboard"
            constraints += place(button, before: trailing)
            trailing = button.leadingAnchor
        }
        if let action {
            let button = UIButton(configuration: .prominentGlass(), primaryAction: UIAction(title: Self.title(action)) {
                _ in onReturn()
            })
            constraints += place(button, before: trailing)
            trailing = button.leadingAnchor
        }
        constraints.append(surface.trailingAnchor.constraint(equalTo: trailing, constant: -8))
        NSLayoutConstraint.activate(constraints)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError("not from a nib") }

    /// A key of the bar, centered, 8 points before `trailing`.
    private func place(_ button: UIButton, before trailing: NSLayoutXAxisAnchor) -> [NSLayoutConstraint] {
        button.translatesAutoresizingMaskIntoConstraints = false
        addSubview(button)
        return [
            button.centerYAnchor.constraint(equalTo: centerYAnchor),
            button.trailingAnchor.constraint(equalTo: trailing, constant: -8),
        ]
    }

    /// The field's text as the guest last sent it, or none (the pads' action alone).
    func show(_ bar: TextBar?) {
        self.bar = bar
        frame.size.height = Self.height(multiLine: bar?.multiLine ?? false)
        // Without a text, the keys alone: no empty glass beside them.
        surface.isHidden = bar == nil
        guard let bar else {
            text.attributedText = nil
            caret.isHidden = true
            return
        }
        let font = text.font ?? .preferredFont(forTextStyle: .body)
        let shown = NSMutableAttributedString(string: bar.display, attributes: [
            .font: font, .foregroundColor: bar.isHint ? UIColor.placeholderText : UIColor.label,
        ])
        if !bar.isHint {
            if !bar.selection.isEmpty {
                shown.addAttribute(.backgroundColor, value: Self.accent.withAlphaComponent(0.25),
                                   range: NSRange(bar.selection))
            }
            if let composing = bar.composing {
                shown.addAttribute(.underlineStyle, value: NSUnderlineStyle.single.rawValue,
                                   range: NSRange(composing))
            }
        }
        text.attributedText = shown
        text.layoutIfNeeded()
        placeCaret()
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        placeCaret()
    }

    /// The caret at the cursor while nothing is selected, in view.
    private func placeCaret() {
        guard let bar, bar.selection.isEmpty, bar.editable else {
            caret.isHidden = true
            return
        }
        let offset = bar.isHint ? 0 : bar.cursor
        guard let position = text.position(from: text.beginningOfDocument, offset: offset) else {
            caret.isHidden = true
            return
        }
        var rect = text.caretRect(for: position)
        rect.size.width = 2
        caret.frame = rect
        caret.isHidden = false
        text.scrollRectToVisible(rect.insetBy(dx: 0, dy: -4), animated: false)
    }

    @objc private func tapped(_ gesture: UITapGestureRecognizer) {
        guard let bar, bar.editable, let position = text.closestPosition(to: gesture.location(in: text)) else { return }
        onSelect(text.offset(from: text.beginningOfDocument, to: position))
    }

    static func title(_ action: ReturnKeyKind) -> String {
        switch action {
        case .standard: "Return"
        case .go: "Go"
        case .search: "Search"
        case .send: "Send"
        case .next: "Next"
        case .done: "Done"
        }
    }
}
