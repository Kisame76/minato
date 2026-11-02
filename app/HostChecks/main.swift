import APKSession
import QuartzCore
import UIKit

// tools/session-window-check.sh's checks of app/Sources/Session/SessionWindow.swift, with UIKit under Mac Catalyst
// (ADR-0043 c4 (d)): which view takes the guest's touches while graphics/host's hide rule hides the window's layer, and
// where the SurfaceViews' layers lie.

var failures = 0

@MainActor func check(_ condition: Bool, _ what: String) {
    print("\(condition ? "ok  " : "FAIL") \(what)")
    if !condition { failures += 1 }
}

/// Hides or shows `layer` as the hide rule does (graphics/host/window.m's set_hidden).
func setHiddenAsTheHideRule(_ layer: CALayer, _ hidden: Bool) {
    CATransaction.begin()
    CATransaction.setDisableActions(true)
    layer.isHidden = hidden
    CATransaction.commit()
}

/// Whether `view` takes the guest's touches and the menu's swipe: multiple touch and a pan for one touch.
@MainActor func takesTouches(_ view: UIView?) -> Bool {
    guard let view else { return false }
    let pans = (view.gestureRecognizers ?? []).compactMap { $0 as? UIPanGestureRecognizer }
    return view.isMultipleTouchEnabled && view.isUserInteractionEnabled
        && pans.contains { $0.maximumNumberOfTouches == 1 }
}

/// The display the view is laid out at: MINATO_DISPLAY, <width>x<height>@<scale> in pixels (tools/check-display.sh).
let display: (width: Int32, height: Int32, scale: CGFloat) = {
    let text = ProcessInfo.processInfo.environment["MINATO_DISPLAY"] ?? "1290x2796@3"
    let parts = text.split(whereSeparator: { $0 == "x" || $0 == "@" }).compactMap { Int32($0) }
    precondition(parts.count == 3, "MINATO_DISPLAY is <width>x<height>@<scale>, not \(text)")
    return (parts[0], parts[1], CGFloat(parts[2]))
}()
let points = CGSize(width: CGFloat(display.width) / display.scale, height: CGFloat(display.height) / display.scale)

let view = SessionLayersView(frame: CGRect(origin: .zero, size: points))
view.layoutIfNeeded()
let window = view.hostView.metalLayer
window.contentsScale = display.scale
let center = CGPoint(x: points.width / 2, y: points.height / 2)
/// UIKit's black, the window view's background, in whatever color space it comes.
func isBlack(_ color: CGColor?) -> Bool {
    guard let color, let parts = color.components, color.alpha == 1 else { return false }
    return parts.dropLast().allSatisfy { $0 == 0 }
}

// The touches, as the window's layer shows and as the hide rule hides it: a window over a full-screen SurfaceView whose
// buffer has no visible pixel.
check(takesTouches(view.hitTest(center, with: nil)), "the view hit while the window's layer shows takes the touches")
setHiddenAsTheHideRule(window, true)
check(takesTouches(view.hitTest(center, with: nil)),
      "the view hit while the hide rule hides the window's layer takes the touches")
check(takesTouches(view.hitTest(CGPoint(x: center.x, y: 10), with: nil)), "and the top edge's, for the menu's swipe")
setHiddenAsTheHideRule(window, false)

/// The view's layers beside the soft keyboard view's, which takes no part in the window's and the surfaces' order.
@MainActor func sideLayers() -> [CALayer] {
    (view.layer.sublayers ?? []).filter { $0 !== view.textView.layer }
}

// Without a surface the window's layer is alone, with the view's black, as before the SurfaceViews.
check(sideLayers().count == 1 && sideLayers().first === window, "without a surface: the window alone")
check(isBlack(window.backgroundColor) && view.layer.backgroundColor == nil, "without a surface: the window's black")

final class Registry {
    var layers: [UInt64: CAMetalLayer] = [:]
    var next: UInt64 = 1
    var refuses = false

    func register(_ layer: CAMetalLayer) -> UInt64 {
        guard !refuses else { return 0 }
        defer { next += 1 }
        layers[next] = layer
        return next
    }
}

let registry = Registry()
let surfaces = view.surfaces
let full = PixelRect(left: 0, top: 0, right: display.width, bottom: display.height)
let media = surfaces.add(frame: full, z: -2, opaque: true) { registry.register($0) }
let sides = sideLayers()
check(sides.count == 3 && sides[1] === window, "the first surface brings the containers, below and above the window")
let below = sides.first, above = sides.last
check(below?.sublayers?.first === registry.layers[media], "a media surface lies below the window")
check(registry.layers[media]?.frame == CGRect(origin: .zero, size: points),
      "its frame in points: the window's pixels over its contentsScale")
check(registry.layers[media]?.contentsScale == display.scale && registry.layers[media]?.isOpaque == true,
      "its contentsScale is the window's, and an opaque format makes it opaque")
check(window.backgroundColor == nil && isBlack(view.layer.backgroundColor),
      "with a surface below, the black is behind it, and the window has none of its own")
check(takesTouches(view.hitTest(center, with: nil)), "the containers take no touches")

let overlay = surfaces.add(frame: full, z: -1, opaque: true) { registry.register($0) }
let second = surfaces.add(frame: full, z: -2, opaque: true) { registry.register($0) }
/// The layers of `windows`, as identities, to compare with a container's sublayers.
@MainActor func identities(_ windows: [UInt64]) -> [ObjectIdentifier] {
    windows.map { ObjectIdentifier(registry.layers[$0]!) }
}
check(below?.sublayers?.map { ObjectIdentifier($0) } == identities([media, second, overlay]),
      "below the window: by z, then in make order")
let top = surfaces.add(frame: PixelRect(left: 0, top: 0, right: 600, bottom: 300), z: 1, opaque: false) {
    registry.register($0)
}
check(above?.sublayers?.first === registry.layers[top] && registry.layers[top]?.isOpaque == false,
      "an on-top surface lies above the window; a format with alpha leaves it not opaque")

surfaces.place(overlay, frame: PixelRect(left: 30, top: 60, right: 330, bottom: 660), z: 2)
check(above?.sublayers?.map { ObjectIdentifier($0) } == identities([top, overlay]),
      "a place with another z moves the layer to its side, by z")
check(registry.layers[overlay]?.frame == CGRect(x: 30, y: 60, width: 300, height: 600).applying(
    CGAffineTransform(scaleX: 1 / display.scale, y: 1 / display.scale)), "and to its new frame")

surfaces.remove(media)
surfaces.remove(second)
check(below?.sublayers?.isEmpty ?? true, "removed surfaces leave their container")
check(isBlack(window.backgroundColor) && view.layer.backgroundColor == nil,
      "with no surface below, the window has its black again")

registry.refuses = true
check(surfaces.add(frame: full, z: -2, opaque: true) { registry.register($0) } == 0
      && (below?.sublayers?.isEmpty ?? true), "a surface without a window is no layer")

// ADR-0041, decision 12: the view reports its size in pixels, once per size, whatever the window's view shows.
var reported: [CGSize] = []
view.onLayout = { reported.append($0) }
view.frame = CGRect(origin: .zero, size: CGSize(width: points.height, height: points.width))
view.layoutIfNeeded()
view.setNeedsLayout()
view.layoutIfNeeded()
check(reported == [CGSize(width: CGFloat(display.height), height: CGFloat(display.width))],
      "a new size is reported once, in pixels")
view.frame = CGRect(origin: .zero, size: points)
view.layoutIfNeeded()

/// Whether two rectangles agree to a hundredth of a point.
func near(_ a: CGRect?, _ b: CGRect) -> Bool {
    guard let a else { return false }
    return [a.minX - b.minX, a.minY - b.minY, a.width - b.width, a.height - b.height].allSatisfy { abs($0) < 0.01 }
}

// Decision 12 b: in size compat the window's view shows the guest's size in the letterbox's frame, at its scale, and
// the containers follow it; a letterbox is no new size.
let half = Letterbox(frame: PixelRect(left: display.width / 4, top: 0, right: display.width / 4 + display.width / 2,
                                      bottom: display.height / 2), scale: 0.5,
                     size: PixelSize(width: display.width, height: display.height))
reported = []
var moved = 0
view.onWindowMoved = { moved += 1 }
view.letterbox = half
view.layoutIfNeeded()
view.setNeedsLayout()
view.layoutIfNeeded()
check(moved == 1, "the window's view moving is told once, for the WebViews over it")
// From the frame's corner, at the guest's size times the scale, which the frame's pixels round.
let boxed = CGRect(x: CGFloat(display.width / 4) / display.scale, y: 0,
                   width: CGFloat(display.width) / 2 / display.scale,
                   height: CGFloat(display.height) / 2 / display.scale)
check(near(view.hostView.frame, boxed), "in size compat the window's view lies in the letterbox's frame, in points")
check(view.hostView.bounds.size == points, "at the guest's size, which the letterbox's scale shows smaller")
check(near(below?.frame, boxed) && near(above?.frame, boxed), "the containers lie where the window's view lies")
check(reported.isEmpty, "a letterbox is no new size")
view.letterbox = nil
view.layoutIfNeeded()
check(view.hostView.frame == CGRect(origin: .zero, size: points) && below?.frame == view.bounds,
      "without, the window's view and the containers span the view again")
view.frame = CGRect(origin: .zero, size: CGSize(width: points.height, height: points.width))
view.layoutIfNeeded()
check(moved == 3, "the letterbox's end and a new size move the window's view too")
view.frame = CGRect(origin: .zero, size: points)
view.layoutIfNeeded()

// ADR-0032 decision 9, amended 2026-11-01: a three-finger swipe down anywhere opens the menu, with Apple's own pan
// recognizer; the guest gets the touches at once and a cancel once the swipe is the menu's; iOS's three-finger
// editing gestures are off, beside the guest's editor too.
let pans = (view.gestureRecognizers ?? []).compactMap { $0 as? UIPanGestureRecognizer }
let threeFingers = pans.filter { $0.minimumNumberOfTouches == MenuSwipe.fingers }
check(threeFingers.count == 1 && threeFingers.allSatisfy { $0.cancelsTouchesInView && !$0.delaysTouchesBegan },
      "one pan recognizer takes three fingers, and the guest has the touches until the menu takes them")
check(pans.contains { $0.maximumNumberOfTouches == 1 }, "the top edge's one-finger swipe stays")
check(view.editingInteractionConfiguration == .none && view.textView.editingInteractionConfiguration == .none,
      "iOS's three-finger editing gestures are off")

print(failures == 0 ? "session window check: fine" : "session window check: \(failures) failed")
exit(failures == 0 ? 0 : 1)
