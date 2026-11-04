import APKLibrary
import Testing

/// Swiping between the tabs (the user's wish, the navigation design): pages side by side that follow the finger, no
/// loop at the edges.
@Suite("Tab swipe")
struct TabSwipeTests {
    let swipe = TabSwipe(count: 3)

    @Test func aSwipeToTheLeftGoesToTheNextTabAndOneToTheRightToTheOneBefore() {
        #expect(swipe.target(from: 0, translation: -40) == 1)
        #expect(swipe.target(from: 1, translation: -40) == 2)
        #expect(swipe.target(from: 2, translation: 40) == 1)
        #expect(swipe.target(from: 1, translation: 40) == 0)
    }

    @Test func theEdgesStop() {
        #expect(swipe.target(from: 0, translation: 40) == nil)
        #expect(swipe.target(from: 2, translation: -40) == nil)
        #expect(swipe.target(from: 1, translation: 0) == nil)
    }

    @Test func onlyAMostlyHorizontalMoveIsASwipe() {
        #expect(TabSwipe.isHorizontal(dx: 30, dy: 5))
        #expect(TabSwipe.isHorizontal(dx: -30, dy: -10))
        #expect(!TabSwipe.isHorizontal(dx: 10, dy: 30))
        #expect(!TabSwipe.isHorizontal(dx: 20, dy: 20))
        #expect(!TabSwipe.isHorizontal(dx: 0, dy: 0))
    }

    @Test func theProgressIsTheShareOfTheWidthAndStaysWithinOne() {
        #expect(TabSwipe.progress(translation: -100, width: 400) == 0.25)
        #expect(TabSwipe.progress(translation: 200, width: 400) == 0.5)
        #expect(TabSwipe.progress(translation: -900, width: 400) == 1)
        #expect(TabSwipe.progress(translation: 50, width: 0) == 0)
    }

    /// Released past half the way, or flicked on towards the next page, it goes on; otherwise it springs back, also
    /// when flicked back.
    @Test func aReleasedSwipeGoesOnPastHalfwayOrWhenFlicked() {
        #expect(TabSwipe.finishes(progress: 0.6, velocity: 0, translation: -240))
        #expect(!TabSwipe.finishes(progress: 0.3, velocity: 0, translation: -120))
        #expect(TabSwipe.finishes(progress: 0.2, velocity: -800, translation: -80))
        #expect(TabSwipe.finishes(progress: 0.2, velocity: 800, translation: 80))
        #expect(!TabSwipe.finishes(progress: 0.6, velocity: 800, translation: -240))
    }
}
