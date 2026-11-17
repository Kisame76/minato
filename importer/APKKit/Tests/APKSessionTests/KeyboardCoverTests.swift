import CoreGraphics
import Testing

@testable import APKSession

/// ADR-0068 step 2d: the part of the session's view that UIKit's keyboard covers, in the session's pixels, which the
/// guest makes every window's ime() inset of.
@Suite("KeyboardCover")
struct KeyboardCoverTests {
    /// An iPhone's screen in points, 3 pixels each.
    static let screen = CGRect(x: 0, y: 0, width: 430, height: 932)

    private func pixels(_ keyboard: CGRect, view: CGRect = screen, scale: Double = 3) -> Int32 {
        KeyboardCover.pixels(keyboard: keyboard, view: view, screen: Self.screen, scale: scale)
    }

    @Test func theDockedKeyboardCoversFromItsTopToTheViewsBottom() {
        #expect(pixels(CGRect(x: 0, y: 596, width: 430, height: 336)) == 1008)
        // A view that ends above the screen's bottom, as a letterboxed one: only its part below the keyboard's top.
        #expect(pixels(CGRect(x: 0, y: 596, width: 430, height: 336), view: CGRect(x: 0, y: 100, width: 430,
                                                                                    height: 600), scale: 2) == 208)
    }

    /// A keyboard and a view that it does not cover: the keyboard's frame, the view's, and why.
    @Test(arguments: [
        (CGRect(x: 0, y: 596, width: 430, height: 336), CGRect(x: 0, y: 0, width: 430, height: 500),
         "a keyboard below the view"),
        // The keyboard's frame at willHide, and an empty one, as UIKit gives for a hardware keyboard's.
        (CGRect(x: 0, y: 932, width: 430, height: 336), KeyboardCoverTests.screen, "a hidden keyboard"),
        (.zero, KeyboardCoverTests.screen, "a hardware keyboard's empty frame"),
        // An iPad's floating or undocked keyboard does not reach the screen's bottom: the content stays where it is,
        // as an Android IME that floats gives no ime() inset.
        (CGRect(x: 60, y: 400, width: 320, height: 250), KeyboardCoverTests.screen, "a keyboard that floats"),
        // A view beside the keyboard, as a window in Split View's other half.
        (CGRect(x: 0, y: 596, width: 200, height: 336), CGRect(x: 230, y: 0, width: 200, height: 932),
         "a keyboard beside the view"),
    ] as [(CGRect, CGRect, String)])
    func aKeyboardThatMissesTheViewCoversNothing(keyboard: CGRect, view: CGRect, kind: String) {
        #expect(pixels(keyboard, view: view) == 0, "\(kind)")
    }

    /// Never more than the view's height, and whole pixels.
    @Test func theCoverIsAtMostTheViewAndRounded() {
        #expect(pixels(CGRect(x: 0, y: 0, width: 430, height: 932), view: CGRect(x: 0, y: 500, width: 430,
                                                                                  height: 432)) == 1296)
        #expect(pixels(CGRect(x: 0, y: 596.4, width: 430, height: 335.6)) == 1007)
    }
}
