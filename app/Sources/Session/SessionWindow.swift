import APKSession
import SwiftUI
import UIKit
import os

private let log = Logger(subsystem: "io.github.kisame76.minato", category: "session")

/// Holds game mode's orientation for the whole session (ADR-0032, decision 5): iOS asks the app delegate
/// which orientations the window may take.
@MainActor
final class AppDelegate: NSObject, UIApplicationDelegate {
    /// Info.plist's three until the launch mode sets its own (ADR-0038, decision 1.1): a benchmark run keeps them.
    static var orientations: UIInterfaceOrientationMask = .allButUpsideDown

    func application(_ application: UIApplication, supportedInterfaceOrientationsFor window: UIWindow?) -> UIInterfaceOrientationMask {
        Self.orientations
    }

    /// Fixes the window to `orientation` and says which one that is, landscape or portrait: the
    /// orientation at start is the one the scene has now. `refused` runs when iOS does not turn the window.
    static func hold(_ orientation: SessionOrientation, in scene: UIWindowScene,
                     refused: @escaping @MainActor @Sendable () -> Void) -> SessionOrientation {
        let (mask, held): (UIInterfaceOrientationMask, SessionOrientation) = switch orientation {
        case .landscape: (.landscape, .landscape)
        case .portrait: (.portrait, .portrait)
        // A session that turns (ADR-0038) allows all three; game mode passes it from step d on.
        case .turns: (.allButUpsideDown, .turns)
        case .atStart:
            switch scene.effectiveGeometry.interfaceOrientation {
            case .landscapeLeft: (.landscapeLeft, .landscape)
            case .landscapeRight: (.landscapeRight, .landscape)
            default: (.portrait, .portrait)
            }
        }
        orientations = mask
        // UIKit does not say on which thread the handler runs.
        scene.requestGeometryUpdate(.iOS(interfaceOrientations: mask)) { @Sendable error in
            log.error("iOS did not turn the window: \(error.localizedDescription, privacy: .public)")
            Task { @MainActor in
                // The orientation it has then stays, also after a return from the background.
                orientations = switch scene.effectiveGeometry.interfaceOrientation {
                case .landscapeLeft: .landscapeLeft
                case .landscapeRight: .landscapeRight
                default: .portrait
                }
                refused()
            }
        }
        scene.keyWindow?.rootViewController?.setNeedsUpdateOfSupportedInterfaceOrientations()
        return held
    }
}

/// The host side of the app's ANativeWindow (ADR-0032, decision 5): a view whose layer is a
/// CAMetalLayer, black until the guest's first frame. ADR-0028 hands the layer to the guest in step c). The layer is
/// graphics/host's MinatoMetalLayer in every build, which records the parity benchmark's drawables (ADR-0037, decision
/// 2). It takes no touches: graphics/host's hide rule hides its layer, and so the view, which hit-testing then skips
/// (ADR-0043 c4 (d), the graphics host's finding), so SessionLayersView takes them.
final class MetalLayerHostView: UIView {
    override class var layerClass: AnyClass { MinatoMetalLayer.self }

    var metalLayer: CAMetalLayer {
        // layerClass makes it one.
        layer as! CAMetalLayer
    }

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .black
        isOpaque = true
        // A background color clears the layer's opacity in UIKit, so the layer is set after it (ADR-0030 V5):
        // opaque as Android's window from NativeActivity's RGB_565, until the guest's format has alpha.
        metalLayer.isOpaque = true
        isUserInteractionEnabled = false
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not used")
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        if let screen = window?.windowScene?.screen {
            metalLayer.contentsScale = screen.nativeScale
        }
    }

    /// The window's size in pixels: the layer's bounds times `contentsScale` (ADR-0032, decision 5, as
    /// ADR-0030 changed it; the graphics front libs write the drawable size).
    var sizeInPixels: CGSize {
        CGSize(width: bounds.width * metalLayer.contentsScale, height: bounds.height * metalLayer.contentsScale)
    }
}

/// The session's view (ADR-0043 c4 (d)): the window's view, with the SurfaceViews' layers beside its layer as siblings,
/// below it for a negative z and above it for a positive one, so graphics/host's hide rule, which hides the window's
/// layer, leaves them alone; a session without a SurfaceView keeps the window's layer alone, as before. It takes the
/// touches, as it is never hidden: the swipes down that open the menu, with three fingers anywhere or from the top
/// edge (ADR-0032, decision 9), and every touch, which goes to the guest (ADR-0036, decision 3); its points are the
/// window's view's.
final class SessionLayersView: UIView {
    /// Where the menu's swipe starts: within this many points of the top edge.
    static let topEdge: CGFloat = 40
    /// How far down it goes before the menu opens, in points.
    static let swipeDistance: CGFloat = 60

    let hostView = MetalLayerHostView(frame: .zero)
    /// Runs after a layout in a new size, with the view's size in pixels, the window's (ADR-0041, decision 12). The
    /// session's view is never hidden, so a turn's size comes while the hide rule hides the window's layer.
    var onLayout: (CGSize) -> Void = { _ in }
    /// In size compat (ADR-0041, decision 12 b), where the window's view shows the guest's window; nil while it spans
    /// the view.
    var letterbox: Letterbox? {
        didSet { if letterbox != oldValue { setNeedsLayout() } }
    }
    /// The size last reported, in pixels.
    private var reportedPixels: CGSize?
    /// Runs after a layout that moved or scaled the window's view (a new size, a letterbox), for the WebViews over it,
    /// which graphics/host places in the view's points (ADR-0056 c2).
    var onWindowMoved: () -> Void = {}
    /// The window's view's frame at the last layout.
    private var placedFrame: CGRect?
    /// The guest's soft keyboard (ADR-0068 step 2c): first responder while the guest's editor wants UIKit's keyboard.
    let textView = SoftKeyboardView(frame: .zero)
    private(set) lazy var surfaces = SurfaceLayers(backdrop: layer, windowLayer: hostView.metalLayer)
    /// Runs once per swipe that opens the menu.
    var onTopEdgeSwipe: () -> Void = {}
    /// Runs for every touch callback, with the touches that changed and the window's pixels per point.
    var onTouches: (TouchPhase, [TouchChange], Double) -> Void = { _, _, _ in }
    /// Runs for the presses of a hardware keyboard's keys (ADR-0068 step 7c2): down or up, or cancelled, with the
    /// keys that Android has a code for; the presses it does not know go on to UIKit.
    var onKeys: (KeyPhase, [HardwareKeyPress]) -> Void = { _, _ in }
    /// Runs whenever UIKit's keyboard moves, with the part of the window's view it covers in pixels, 0 once it is
    /// hidden (ADR-0068 step 2d).
    var onKeyboardCover: (Int32) -> Void = { _ in }
    private var swipeOpenedMenu = false
    /// The three-finger swipe's state (ADR-0032 decision 9, amended 2026-11-01).
    private var menuSwipe = MenuSwipe()
    private let threeFingers = UIPanGestureRecognizer()
    /// The three-finger pan's delegate, which the recognizer holds weakly.
    private let alongside = AlongsideOthers()
    private var keyboardObservers: [any NSObjectProtocol] = []

    override init(frame: CGRect) {
        super.init(frame: frame)
        addSubview(hostView)
        addSubview(textView)
        isMultipleTouchEnabled = true
        let swipe = UIPanGestureRecognizer(target: self, action: #selector(swiped(_:)))
        swipe.maximumNumberOfTouches = 1
        // The guest gets the swipe's touch at once, and a cancel once it is the menu's (ADR-0036,
        // decision 3).
        swipe.cancelsTouchesInView = true
        swipe.delaysTouchesBegan = false
        addGestureRecognizer(swipe)
        // The menu's swipe with three fingers anywhere, which iOS leaves to the app, unlike an edge: the guest gets
        // the touches at once and a cancel once the swipe is the menu's, as above.
        threeFingers.addTarget(self, action: #selector(threeFingersSwiped(_:)))
        threeFingers.minimumNumberOfTouches = MenuSwipe.fingers
        threeFingers.cancelsTouchesInView = true
        threeFingers.delaysTouchesBegan = false
        // Over an app's WKWebView, WebKit's recognizers start on the first finger and are exclusive by default.
        threeFingers.delegate = alongside
        addGestureRecognizer(threeFingers)
    }

    /// iOS's three-finger editing gestures (undo, redo, copy, paste) are off in game mode: three fingers are the
    /// menu's.
    override var editingInteractionConfiguration: UIEditingInteractionConfiguration { .none }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not used")
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        placeWindowView()
        surfaces.laidOut()
        if hostView.frame != placedFrame {
            let first = placedFrame == nil
            placedFrame = hostView.frame
            if !first { onWindowMoved() }
        }
        let scale = hostView.metalLayer.contentsScale
        let pixels = CGSize(width: bounds.width * scale, height: bounds.height * scale)
        guard pixels != reportedPixels else { return }
        reportedPixels = pixels
        onLayout(pixels)
    }

    /// The window's view spans the view, or in size compat has the guest's size, shown at the letterbox's scale in
    /// its frame.
    private func placeWindowView() {
        guard let letterbox else {
            hostView.transform = .identity
            hostView.frame = bounds
            return
        }
        let points = hostView.metalLayer.contentsScale
        let scale = CGFloat(letterbox.scale)
        let (width, height) = (CGFloat(letterbox.size.width), CGFloat(letterbox.size.height))
        hostView.transform = CGAffineTransform(scaleX: scale, y: scale)
        hostView.bounds = CGRect(x: 0, y: 0, width: width / points, height: height / points)
        hostView.center = CGPoint(x: (CGFloat(letterbox.frame.left) + width * scale / 2) / points,
                                  y: (CGFloat(letterbox.frame.top) + height * scale / 2) / points)
    }

    // The keys of a hardware keyboard go to the first responder: the session's view, while it is in a window, or its
    // text view while the guest's editor has UIKit's keyboard, which hands them here.
    override var canBecomeFirstResponder: Bool { true }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window != nil { becomeFirstResponder() }
        observeKeyboard(window != nil)
    }

    /// UIKit's keyboard notifications while the view is in a window (ADR-0068 step 2d): willShow and willChangeFrame
    /// give the keyboard's frame, also while it moves, and willHide 0.
    private func observeKeyboard(_ on: Bool) {
        for observer in keyboardObservers { NotificationCenter.default.removeObserver(observer) }
        keyboardObservers = []
        guard on else { return }
        let center = NotificationCenter.default
        for name in [UIResponder.keyboardWillShowNotification, UIResponder.keyboardWillChangeFrameNotification,
                     UIResponder.keyboardWillHideNotification] {
            keyboardObservers.append(center.addObserver(forName: name, object: nil, queue: .main) { [weak self] note in
                let hides = note.name == UIResponder.keyboardWillHideNotification
                let frame = hides ? nil : note.userInfo?[UIResponder.keyboardFrameEndUserInfoKey] as? CGRect
                MainActor.assumeIsolated { self?.keyboardMoved(frame) }
            })
        }
    }

    /// `frame` is the keyboard's in the screen's points, nil once it hides.
    private func keyboardMoved(_ frame: CGRect?) {
        guard let screen = window?.windowScene?.screen else { return }
        let view = hostView.convert(hostView.bounds, to: screen.coordinateSpace)
        onKeyboardCover(KeyboardCover.pixels(keyboard: frame ?? .zero, view: view, screen: screen.bounds,
                                             scale: hostView.metalLayer.contentsScale / CGFloat(letterbox?.scale ?? 1)))
    }

    override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        let rest = handKeys(.down, presses)
        if !rest.isEmpty { super.pressesBegan(rest, with: event) }
    }

    override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        let rest = handKeys(.up, presses)
        if !rest.isEmpty { super.pressesEnded(rest, with: event) }
    }

    override func pressesCancelled(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        let rest = handKeys(.cancelled, presses)
        if !rest.isEmpty { super.pressesCancelled(rest, with: event) }
    }

    /// Hands the presses that have a hardware key and an Android code for it to the guest, in the order of their
    /// times; returns the others, which are UIKit's (the Globe key, the system's shortcuts).
    private func handKeys(_ phase: KeyPhase, _ presses: Set<UIPress>) -> Set<UIPress> {
        var rest = presses
        let keys = presses.compactMap { press -> HardwareKeyPress? in
            guard let key = press.key,
                  HardwareKeyboard.androidKeyCode(forHIDUsage: key.keyCode.rawValue) != nil else { return nil }
            rest.remove(press)
            return HardwareKeyPress(usage: key.keyCode.rawValue, timestamp: press.timestamp)
        }.sorted { $0.timestamp < $1.timestamp }
        if !keys.isEmpty { onKeys(phase, keys) }
        return rest
    }

    /// Only a pan that starts at the top edge and goes down is the menu's swipe; the view spans the
    /// screen, so its top is the screen's in every orientation. A Debug build logs each pan near the top.
    override func gestureRecognizerShouldBegin(_ recognizer: UIGestureRecognizer) -> Bool {
        guard let pan = recognizer as? UIPanGestureRecognizer else { return true }
        if pan === threeFingers {
            let velocity = pan.velocity(in: self)
            return MenuSwipe.begins(touches: pan.numberOfTouches, velocityX: velocity.x, velocityY: velocity.y)
        }
        let location = pan.location(in: self)
        let translation = pan.translation(in: self)
        let start = CGPoint(x: location.x - translation.x, y: location.y - translation.y)
        let velocity = pan.velocity(in: self)
        #if DEBUG
        if let report = EdgePanReport.start(x: start.x, y: start.y, width: bounds.width, height: bounds.height,
                                            velocityX: velocity.x, velocityY: velocity.y, topEdge: Self.topEdge) {
            note(report.line)
        }
        #endif
        return EdgePanReport.verdict(startY: start.y, velocityX: velocity.x, velocityY: velocity.y,
                                     topEdge: Self.topEdge) == .accepted
    }

    // Every callback of its own, none handed on: the view is the guest's whole screen.
    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        hand(.began, touches, event)
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        hand(.moved, touches, event)
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        hand(.ended, touches, event)
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        hand(.cancelled, touches, event)
    }

    /// Each touch with its coalesced samples, oldest first; a touch is known by its object while it lasts.
    private func hand(_ phase: TouchPhase, _ touches: Set<UITouch>, _ event: UIEvent?) {
        let changes = touches.map { touch in
            let coalesced = event?.coalescedTouches(for: touch) ?? []
            return TouchChange(touch: UInt(bitPattern: ObjectIdentifier(touch)),
                               samples: (coalesced.isEmpty ? [touch] : coalesced).map(sample))
        }
        onTouches(phase, changes, hostView.metalLayer.contentsScale)
    }

    private func sample(_ touch: UITouch) -> TouchSample {
        let place = touch.preciseLocation(in: self)
        return TouchSample(x: place.x, y: place.y, force: touch.force, maximumForce: touch.maximumPossibleForce,
                           majorRadius: touch.majorRadius, timestamp: touch.timestamp)
    }

    @objc private func swiped(_ pan: UIPanGestureRecognizer) {
        switch pan.state {
        case .began:
            swipeOpenedMenu = false
        case .changed where !swipeOpenedMenu && pan.translation(in: self).y > Self.swipeDistance:
            swipeOpenedMenu = true
            onTopEdgeSwipe()
        #if DEBUG
        case .ended, .cancelled, .failed:
            note(EdgePanReport.end(distance: pan.translation(in: self).y, menu: swipeOpenedMenu))
        #endif
        default:
            break
        }
    }

    @objc private func threeFingersSwiped(_ pan: UIPanGestureRecognizer) {
        switch pan.state {
        case .began:
            menuSwipe.began()
        case .changed:
            if menuSwipe.moved(distance: pan.translation(in: self).y) { onTopEdgeSwipe() }
        default:
            break
        }
    }

    #if DEBUG
    /// The swipe log of ADR-0032's decision 9: what tools/session-check.sh prints after its rounds by hand.
    private func note(_ line: String) {
        log.debug("\(line, privacy: .public)")
        SessionCheck.event(line)
    }
    #endif
}

/// The SurfaceViews' layers (ADR-0043 c4 (d)): each a MinatoMetalLayer, whose presents are paced as the window's
/// (graphics/host's layer.m), in one of two containers beside the window's layer, the one below it for a negative z
/// and the one above it for a positive one, by z, then in make order (`SurfaceOrder`); frames come in the window's
/// pixels. The containers go in with the first surface. While a surface lies below the window, the window's layer
/// has no background of its own, which would cover the surface wherever the window is transparent (its SurfaceView's
/// hole), and the black is behind the surfaces instead. Used on the main thread, for the bridge.
final class SurfaceLayers {
    private struct Entry {
        let layer: MinatoMetalLayer
        var key: SurfaceOrder.Key
    }

    /// The session view's layer, which holds the window's layer and the containers.
    private let backdrop: CALayer
    private let windowLayer: CAMetalLayer
    private let below = CALayer()
    private let above = CALayer()
    private var entries: [UInt64: Entry] = [:]
    private var made: UInt64 = 0
    private var surfaceBelow = false
    /// The window layer's own background, the view's black, while a surface lies below.
    private var windowBackground: CGColor?
    /// Runs with each new surface's layer before the guest gets its window: a benchmark records it.
    var onAdd: (CAMetalLayer) -> Void = { _ in }
    /// Whether a WebView lies below the window (ADR-0056 c2), whose background then goes as for a surface below it.
    var webViewsBelow = false {
        didSet { updateBackgrounds() }
    }

    init(backdrop: CALayer, windowLayer: CAMetalLayer) {
        self.backdrop = backdrop
        self.windowLayer = windowLayer
    }

    /// A new surface's layer at `frame`, below the window for a negative z, with the window that `register` gives
    /// it, or 0 without one.
    func add(frame: PixelRect, z: Int32, opaque: Bool, register: (CAMetalLayer) -> UInt64) -> UInt64 {
        let layer = MinatoMetalLayer()
        layer.contentsScale = windowLayer.contentsScale
        layer.isOpaque = opaque
        Self.withoutAnimation { layer.frame = points(frame) }
        onAdd(layer)
        let window = register(layer)
        guard window != 0 else { return 0 }
        if below.superlayer == nil {
            Self.withoutAnimation {
                backdrop.insertSublayer(below, below: windowLayer)
                backdrop.insertSublayer(above, above: windowLayer)
            }
            laidOut()
        }
        made += 1
        entries[window] = Entry(layer: layer, key: SurfaceOrder.Key(z: z, made: made))
        insert(window)
        return window
    }

    func place(_ window: UInt64, frame: PixelRect, z: Int32) {
        guard var entry = entries[window] else { return }
        Self.withoutAnimation { entry.layer.frame = points(frame) }
        guard entry.key.z != z else { return }
        entry.key.z = z
        entries[window] = entry
        insert(window)
    }

    func remove(_ window: UInt64) {
        guard let entry = entries.removeValue(forKey: window) else { return }
        Self.withoutAnimation { entry.layer.removeFromSuperlayer() }
        updateBackgrounds()
    }

    /// The session's view was laid out: the containers lie where the window's layer lies, at its scale (ADR-0041,
    /// decision 12 b), so that the surfaces' frames stay the window's pixels.
    func laidOut() {
        guard below.superlayer != nil else { return }
        Self.withoutAnimation {
            for container in [below, above] {
                container.setAffineTransform(windowLayer.affineTransform())
                container.bounds = windowLayer.bounds
                container.position = windowLayer.position
            }
        }
    }

    /// The layer goes to its side at its place by z, then in make order.
    private func insert(_ window: UInt64) {
        guard let entry = entries[window] else { return }
        let side = SurfaceOrder.isBelowWindow(z: entry.key.z) ? below : above
        let others = (side.sublayers ?? []).filter { $0 !== entry.layer }
        let keys = others.compactMap { layer in entries.values.first { $0.layer === layer }?.key }
        Self.withoutAnimation {
            entry.layer.removeFromSuperlayer()
            side.insertSublayer(entry.layer, at: UInt32(SurfaceOrder.index(of: entry.key, among: keys)))
        }
        updateBackgrounds()
    }

    private func updateBackgrounds() {
        let hasBelow = webViewsBelow || entries.values.contains { SurfaceOrder.isBelowWindow(z: $0.key.z) }
        guard hasBelow != surfaceBelow else { return }
        surfaceBelow = hasBelow
        // The layers' own property, not the view's backgroundColor, which would also clear the layer's opacity.
        Self.withoutAnimation {
            if hasBelow {
                windowBackground = windowLayer.backgroundColor
                backdrop.backgroundColor = windowBackground
                windowLayer.backgroundColor = nil
            } else {
                windowLayer.backgroundColor = windowBackground
                backdrop.backgroundColor = nil
            }
        }
    }

    /// `frame` in points, as the window's layer has them.
    private func points(_ frame: PixelRect) -> CGRect {
        let scale = max(windowLayer.contentsScale, 1)
        return CGRect(x: CGFloat(frame.left) / scale, y: CGFloat(frame.top) / scale,
                      width: CGFloat(frame.right - frame.left) / scale,
                      height: CGFloat(frame.bottom - frame.top) / scale)
    }

    private static func withoutAnimation(_ body: () -> Void) {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        body()
        CATransaction.commit()
    }
}

/// The host's view of the guest's input method (ADR-0068 step 2c): an invisible UIKeyInput in the session's view that
/// becomes first responder when the guest shows its keyboard, so UIKit shows its own with the editor's traits, and
/// hands what it types on. A hardware keyboard's presses stay keys, as on Android, where they reach the focused
/// view as KeyEvents: the view gives them to the session's view. Above the keyboard sits a bar (KeyboardBarView) with
/// the served field's text once the guest has sent it (keyboard plan b), and the editor's action for the pads, which
/// have no return key; a tap outside the field puts the keyboard away (OutsideTap).
final class SoftKeyboardView: UIView, UIKeyInput {
    /// As the session's view's: three fingers are the menu's, also while the guest's editor has the keyboard.
    override var editingInteractionConfiguration: UIEditingInteractionConfiguration { .none }

    /// Typed text, the delete key, and the return key or the bar's button.
    var onInsert: (String) -> Void = { _ in }
    var onDelete: () -> Void = {}
    var onReturn: () -> Void = {}
    /// UIKit took the keyboard away without the guest asking (a tap outside the field, an iPad's dismiss key, another
    /// first responder).
    var onDismissed: () -> Void = {}
    /// A tap in the bar's text, at this offset in UTF-16 units of its text.
    var onSelect: (Int) -> Void = { _ in }

    var keyboardType: UIKeyboardType = .default
    var returnKeyType: UIReturnKeyType = .default
    var autocapitalizationType: UITextAutocapitalizationType = .none
    var autocorrectionType: UITextAutocorrectionType = .no
    var spellCheckingType: UITextSpellCheckingType = .no
    // Android's keyboards type what the key shows.
    var smartQuotesType: UITextSmartQuotesType = .no
    var smartDashesType: UITextSmartDashesType = .no
    var smartInsertDeleteType: UITextSmartInsertDeleteType = .no
    var isSecureTextEntry = false
    var hasText = true

    /// While the guest's editor wants the keyboard: UIKit may give the view the focus back (after a sheet).
    private var active = false
    private var resigning = false
    private var bar: KeyboardBarView?
    private var barAction: ReturnKeyKind?
    private var textBar: TextBar?

    override var canBecomeFirstResponder: Bool { active }
    override var inputAccessoryView: UIView? { bar }

    func show(_ traits: KeyboardTraits) {
        apply(traits)
        active = true
        // Still up after a release (a tap on the focused field): the new editor's traits only.
        if isFirstResponder { reloadInputViews() } else { becomeFirstResponder() }
    }

    /// New traits while shown (the guest restarted the input).
    func update(_ traits: KeyboardTraits) {
        apply(traits)
        reloadInputViews()
    }

    /// The guest hid its keyboard: the session's view takes the hardware keys again.
    func hide() {
        active = false
        resigning = true
        _ = resignFirstResponder()
        resigning = false
        superview?.becomeFirstResponder()
    }

    override func resignFirstResponder() -> Bool {
        let resigned = super.resignFirstResponder()
        if resigned, !resigning { onDismissed() }
        return resigned
    }

    func insertText(_ text: String) {
        hasText = true
        onInsert(text)
    }

    func deleteBackward() {
        onDelete()
    }

    override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        guard let superview else { return super.pressesBegan(presses, with: event) }
        superview.pressesBegan(presses, with: event)
    }

    override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        guard let superview else { return super.pressesEnded(presses, with: event) }
        superview.pressesEnded(presses, with: event)
    }

    override func pressesCancelled(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        guard let superview else { return super.pressesCancelled(presses, with: event) }
        superview.pressesCancelled(presses, with: event)
    }

    private func apply(_ traits: KeyboardTraits) {
        keyboardType = switch traits.kind {
        case .standard: .default
        case .ascii: .asciiCapable
        case .url: .URL
        case .email: .emailAddress
        case .numberPad: .numberPad
        case .decimalPad: .decimalPad
        case .numbersAndPunctuation: .numbersAndPunctuation
        case .phonePad: .phonePad
        }
        returnKeyType = switch traits.returnKey {
        case .standard: .default
        case .go: .go
        case .search: .search
        case .send: .send
        case .next: .next
        case .done: .done
        }
        autocapitalizationType = switch traits.capitalization {
        case .none: .none
        case .words: .words
        case .sentences: .sentences
        case .characters: .allCharacters
        }
        autocorrectionType = traits.corrects ? .default : .no
        spellCheckingType = traits.corrects ? .default : .no
        isSecureTextEntry = traits.secure
        hasText = !traits.cursorAtStart
        barAction = traits.barAction
        // A new editor: its text comes with its next extract.
        textBar = nil
        bar = nil
        rebuildBar()
    }

    /// The served field's text for the bar, nil before the guest has sent it or once its editor has gone.
    func showText(_ textBar: TextBar?) {
        let height = bar?.frame.height
        let had = bar != nil
        self.textBar = textBar
        rebuildBar()
        if isFirstResponder, had != (bar != nil) || height != bar?.frame.height { reloadInputViews() }
    }

    /// The bar while it has something to show: the field's text, the pads' action, or both.
    private func rebuildBar() {
        guard textBar != nil || barAction != nil else {
            bar = nil
            return
        }
        if bar == nil {
            let made = KeyboardBarView(action: barAction) { [weak self] in self?.onReturn() }
            made.onSelect = { [weak self] in self?.onSelect($0) }
            bar = made
        }
        bar?.show(textBar)
    }
}

struct MetalLayerView: UIViewRepresentable {
    /// The view's layers, once, as the view is made: the window's, and the SurfaceViews' beside it.
    var onLayer: (CAMetalLayer, SurfaceLayers) -> Void
    /// A swipe down from the top edge (ADR-0032, decision 9).
    var onTopEdgeSwipe: () -> Void
    /// A layout, with the window's size in pixels.
    var onLayout: (CGSize) -> Void
    /// A touch callback, with the window's pixels per point (ADR-0036, decision 3).
    var onTouches: (TouchPhase, [TouchChange], Double) -> Void
    /// A hardware keyboard's presses (ADR-0068 step 7c2).
    var onKeys: (KeyPhase, [HardwareKeyPress]) -> Void = { _, _ in }
    /// The soft keyboard's view, once, as the view is made (ADR-0068 step 2c).
    var onTextView: (SoftKeyboardView) -> Void = { _ in }
    /// The part of the session's view UIKit's keyboard covers, in pixels (ADR-0068 step 2d).
    var onKeyboardCover: (Int32) -> Void = { _ in }
    /// The session's view, once, as it is made.
    var onView: (SessionLayersView) -> Void = { _ in }

    func makeUIView(context: Context) -> SessionLayersView {
        let view = SessionLayersView(frame: .zero)
        updateUIView(view, context: context)
        onView(view)
        onLayer(view.hostView.metalLayer, view.surfaces)
        onTextView(view.textView)
        return view
    }

    func updateUIView(_ view: SessionLayersView, context: Context) {
        view.onTopEdgeSwipe = onTopEdgeSwipe
        view.onLayout = onLayout
        view.onTouches = onTouches
        view.onKeys = onKeys
        view.onKeyboardCover = onKeyboardCover
    }
}

extension UIInterfaceOrientationMask {
    init(_ orientations: InterfaceOrientations) {
        self = []
        if orientations.contains(.portrait) { insert(.portrait) }
        if orientations.contains(.landscapeLeft) { insert(.landscapeLeft) }
        if orientations.contains(.landscapeRight) { insert(.landscapeRight) }
    }
}

/// Lets the menu's three-finger pan begin while another recognizer, such as one of a WKWebView, has begun already.
private final class AlongsideOthers: NSObject, UIGestureRecognizerDelegate {
    func gestureRecognizer(_ recognizer: UIGestureRecognizer,
                           shouldRecognizeSimultaneouslyWith other: UIGestureRecognizer) -> Bool {
        true
    }
}
