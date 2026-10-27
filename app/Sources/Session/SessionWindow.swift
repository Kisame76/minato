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
/// (ADR-0043 c4 (d), Grafik's finding), so SessionLayersView takes them.
final class MetalLayerHostView: UIView {
    override class var layerClass: AnyClass { MinatoMetalLayer.self }

    /// Runs after every layout, with the window's size in pixels.
    var onLayout: (CGSize) -> Void = { _ in }

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

    // A hidden view is laid out all the same, so a turn's size comes while the hide rule hides the layer.
    override func layoutSubviews() {
        super.layoutSubviews()
        onLayout(sizeInPixels)
    }
}

/// The session's view (ADR-0043 c4 (d)): the window's view, with the SurfaceViews' layers beside its layer as siblings,
/// below it for a negative z and above it for a positive one, so graphics/host's hide rule, which hides the window's
/// layer, leaves them alone; a session without a SurfaceView keeps the window's layer alone, as before. It takes the
/// touches, as it is never hidden: the swipe down from the top edge that opens the menu (ADR-0032, decision 9), and
/// every touch, which goes to the guest (ADR-0036, decision 3); its points are the window's view's.
final class SessionLayersView: UIView {
    /// Where the menu's swipe starts: within this many points of the top edge.
    static let topEdge: CGFloat = 40
    /// How far down it goes before the menu opens, in points.
    static let swipeDistance: CGFloat = 60

    let hostView = MetalLayerHostView(frame: .zero)
    private(set) lazy var surfaces = SurfaceLayers(backdrop: layer, windowLayer: hostView.metalLayer)
    /// Runs once per swipe that opens the menu.
    var onTopEdgeSwipe: () -> Void = {}
    /// Runs for every touch callback, with the touches that changed and the window's pixels per point.
    var onTouches: (TouchPhase, [TouchChange], Double) -> Void = { _, _, _ in }
    private var swipeOpenedMenu = false

    override init(frame: CGRect) {
        super.init(frame: frame)
        addSubview(hostView)
        isMultipleTouchEnabled = true
        let swipe = UIPanGestureRecognizer(target: self, action: #selector(swiped(_:)))
        swipe.maximumNumberOfTouches = 1
        // The guest gets the swipe's touch at once, and a cancel once it is the menu's (ADR-0036,
        // decision 3).
        swipe.cancelsTouchesInView = true
        swipe.delaysTouchesBegan = false
        addGestureRecognizer(swipe)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not used")
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        hostView.frame = bounds
        surfaces.laidOut()
    }

    /// Only a pan that starts at the top edge and goes down is the menu's swipe; the view spans the
    /// screen, so its top is the screen's in every orientation. A Debug build logs each pan near the top.
    override func gestureRecognizerShouldBegin(_ recognizer: UIGestureRecognizer) -> Bool {
        guard let pan = recognizer as? UIPanGestureRecognizer else { return true }
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

    /// The session's view was laid out: the containers span it, as the window's layer does.
    func laidOut() {
        guard below.superlayer != nil else { return }
        Self.withoutAnimation {
            below.frame = backdrop.bounds
            above.frame = backdrop.bounds
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
        let hasBelow = entries.values.contains { SurfaceOrder.isBelowWindow(z: $0.key.z) }
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

struct MetalLayerView: UIViewRepresentable {
    /// The view's layers, once, as the view is made: the window's, and the SurfaceViews' beside it.
    var onLayer: (CAMetalLayer, SurfaceLayers) -> Void
    /// A swipe down from the top edge (ADR-0032, decision 9).
    var onTopEdgeSwipe: () -> Void
    /// A layout, with the window's size in pixels.
    var onLayout: (CGSize) -> Void
    /// A touch callback, with the window's pixels per point (ADR-0036, decision 3).
    var onTouches: (TouchPhase, [TouchChange], Double) -> Void

    func makeUIView(context: Context) -> SessionLayersView {
        let view = SessionLayersView(frame: .zero)
        updateUIView(view, context: context)
        onLayer(view.hostView.metalLayer, view.surfaces)
        return view
    }

    func updateUIView(_ view: SessionLayersView, context: Context) {
        view.onTopEdgeSwipe = onTopEdgeSwipe
        view.hostView.onLayout = onLayout
        view.onTouches = onTouches
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
