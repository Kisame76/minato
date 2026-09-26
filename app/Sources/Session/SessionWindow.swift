import APKSession
import SwiftUI
import UIKit
import os

private let log = Logger(subsystem: "io.github.kisame76.minato", category: "session")

/// Holds game mode's orientation for the whole session (ADR-0032, decision 5): iOS asks the app delegate
/// which orientations the window may take.
@MainActor
final class AppDelegate: NSObject, UIApplicationDelegate {
    /// The library's are Info.plist's three.
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
/// CAMetalLayer, black until the guest's first frame. ADR-0028 hands the layer to the guest in step c).
/// It also takes the swipe down from the top edge that opens the menu (decision 9); the guest's own
/// touch comes with M3 step 6.
final class MetalLayerHostView: UIView {
    override class var layerClass: AnyClass { CAMetalLayer.self }

    /// Where the menu's swipe starts: within this many points of the top edge.
    static let topEdge: CGFloat = 40
    /// How far down it goes before the menu opens, in points.
    static let swipeDistance: CGFloat = 60

    /// Runs once per swipe that opens the menu.
    var onTopEdgeSwipe: () -> Void = {}
    /// Runs after every layout, with the window's size in pixels.
    var onLayout: (CGSize) -> Void = { _ in }
    private var swipeOpenedMenu = false

    var metalLayer: CAMetalLayer {
        // layerClass makes it one.
        layer as! CAMetalLayer
    }

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .black
        isOpaque = true
        let swipe = UIPanGestureRecognizer(target: self, action: #selector(swiped(_:)))
        swipe.maximumNumberOfTouches = 1
        addGestureRecognizer(swipe)
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

    override func layoutSubviews() {
        super.layoutSubviews()
        onLayout(sizeInPixels)
    }

    /// Only a pan that starts at the top edge and goes down is the menu's swipe; the view spans the
    /// screen, so its top is the screen's in every orientation.
    override func gestureRecognizerShouldBegin(_ recognizer: UIGestureRecognizer) -> Bool {
        guard let pan = recognizer as? UIPanGestureRecognizer else { return true }
        let start = pan.location(in: self).y - pan.translation(in: self).y
        let velocity = pan.velocity(in: self)
        return start < Self.topEdge && velocity.y > abs(velocity.x)
    }

    @objc private func swiped(_ pan: UIPanGestureRecognizer) {
        switch pan.state {
        case .began:
            swipeOpenedMenu = false
        case .changed where !swipeOpenedMenu && pan.translation(in: self).y > Self.swipeDistance:
            swipeOpenedMenu = true
            onTopEdgeSwipe()
        default:
            break
        }
    }
}

struct MetalLayerView: UIViewRepresentable {
    /// A swipe down from the top edge (ADR-0032, decision 9).
    var onTopEdgeSwipe: () -> Void
    /// A layout, with the window's size in pixels.
    var onLayout: (CGSize) -> Void

    func makeUIView(context: Context) -> MetalLayerHostView {
        let view = MetalLayerHostView(frame: .zero)
        updateUIView(view, context: context)
        return view
    }

    func updateUIView(_ view: MetalLayerHostView, context: Context) {
        view.onTopEdgeSwipe = onTopEdgeSwipe
        view.onLayout = onLayout
    }
}
