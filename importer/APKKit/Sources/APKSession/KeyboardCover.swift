import CoreGraphics

/// The part of the session's view that UIKit's keyboard covers (ADR-0068 step 2d), from the keyboard's top edge to the
/// view's bottom, which the guest makes every window's ime() inset of (APKRT_EVENT_IME_INSETS).
public enum KeyboardCover {
    /// The cover in the session's pixels, `scale` per point; `keyboard`, `view` and `screen` in the screen's points, as
    /// UIKit's keyboard notifications give the keyboard's frame. 0 when the keyboard is hidden, beside or below the
    /// view, or floats (an iPad's undocked keyboard does not reach the screen's bottom), as an Android IME that floats
    /// gives no ime() inset.
    public static func pixels(keyboard: CGRect, view: CGRect, screen: CGRect, scale: Double) -> Int32 {
        guard !keyboard.isEmpty, keyboard.maxY >= screen.maxY - 0.5, keyboard.minX < view.maxX,
              keyboard.maxX > view.minX else { return 0 }
        let points = min(max(view.maxY - keyboard.minY, 0), view.height)
        return Int32((points * scale).rounded())
    }
}
