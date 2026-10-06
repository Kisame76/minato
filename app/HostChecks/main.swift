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

let view = SessionLayersView(frame: CGRect(x: 0, y: 0, width: 430, height: 932))
view.layoutIfNeeded()
let window = view.hostView.metalLayer
window.contentsScale = 3
let center = CGPoint(x: 215, y: 466)
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
check(takesTouches(view.hitTest(CGPoint(x: 215, y: 10), with: nil)), "and the top edge's, for the menu's swipe")
setHiddenAsTheHideRule(window, false)

// Without a surface the window's layer is alone, with the view's black, as before the SurfaceViews.
check(view.layer.sublayers?.count == 1 && view.layer.sublayers?.first === window, "without a surface: the window alone")
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
let full = PixelRect(left: 0, top: 0, right: 1290, bottom: 2796)
let media = surfaces.add(frame: full, z: -2, opaque: true) { registry.register($0) }
let sides = view.layer.sublayers ?? []
check(sides.count == 3 && sides[1] === window, "the first surface brings the containers, below and above the window")
let below = sides.first, above = sides.last
check(below?.sublayers?.first === registry.layers[media], "a media surface lies below the window")
check(registry.layers[media]?.frame == CGRect(x: 0, y: 0, width: 430, height: 932),
      "its frame in points: the window's pixels over its contentsScale")
check(registry.layers[media]?.contentsScale == 3 && registry.layers[media]?.isOpaque == true,
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
check(registry.layers[overlay]?.frame == CGRect(x: 10, y: 20, width: 100, height: 200), "and to its new frame")

surfaces.remove(media)
surfaces.remove(second)
check(below?.sublayers?.isEmpty ?? true, "removed surfaces leave their container")
check(isBlack(window.backgroundColor) && view.layer.backgroundColor == nil,
      "with no surface below, the window has its black again")

registry.refuses = true
check(surfaces.add(frame: full, z: -2, opaque: true) { registry.register($0) } == 0
      && (below?.sublayers?.isEmpty ?? true), "a surface without a window is no layer")

print(failures == 0 ? "session window check: fine" : "session window check: \(failures) failed")
exit(failures == 0 ? 0 : 1)
