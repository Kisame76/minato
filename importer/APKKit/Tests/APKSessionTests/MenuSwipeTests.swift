import Testing

@testable import APKSession

/// ADR-0032 decision 9, amended 2026-11-01: a three-finger swipe down anywhere opens the menu.
struct MenuSwipeTests {
    /// A pan's changes, down by `step` points each time, through `MenuSwipe`: how often it opens the menu.
    private func opens(touches: Int, velocityX: Double = 0, velocityY: Double = 400, steps: Int = 10,
                       step: Double = 20) -> Int {
        guard MenuSwipe.begins(touches: touches, velocityX: velocityX, velocityY: velocityY) else { return 0 }
        var swipe = MenuSwipe()
        swipe.began()
        return (1...steps).filter { swipe.moved(distance: Double($0) * step) }.count
    }

    @Test func aThreeTouchPanDownOpensTheMenuOnce() {
        #expect(opens(touches: 3) == 1)
    }

    @Test func aTwoTouchPanNeverOpensIt() {
        #expect(opens(touches: 2) == 0)
        #expect(opens(touches: 1) == 0)
    }

    @Test func onlyDownMoreThanSidewaysIsTheMenus() {
        #expect(opens(touches: 3, velocityX: 500, velocityY: 400) == 0)
        #expect(opens(touches: 3, velocityY: -400) == 0)
    }

    @Test func itOpensPastTheDistanceOnlyAndAgainAfterANewStart() {
        var swipe = MenuSwipe()
        swipe.began()
        let atTheDistance = swipe.moved(distance: MenuSwipe.distance)
        let past = swipe.moved(distance: MenuSwipe.distance + 1)
        let further = swipe.moved(distance: MenuSwipe.distance + 100)
        swipe.began()
        let again = swipe.moved(distance: MenuSwipe.distance + 1)
        #expect(!atTheDistance && past && !further && again)
    }
}
