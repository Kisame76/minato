import XCTest

/// The swipe between Home, Library and Settings (RootView's SwipeTabBarController), as the user swipes: fast and
/// slow, back and forth, let go before half the way, and right after a tab was tapped. After each swipe the page must
/// be whole: the selected tab's page in the window's place, and not black (the user's bug of 2026-11-06: sometimes
/// a black page after a swipe: no page in the window). tools/tab-swipe-check.sh runs it. Minato starts in the light
/// appearance (its Debug build's MINATO_APPEARANCE), where a page is never black, also on an iPhone in dark mode, where
/// an empty page is black by itself; so a black page counts everywhere.
@MainActor
final class TabSwipeUITests: XCTestCase {
    private let minato: XCUIApplication = {
        let minato = XCUIApplication(bundleIdentifier: "io.github.kisame76.minato")
        minato.launchEnvironment["MINATO_APPEARANCE"] = "light"
        return minato
    }()
    private let tabs = ["Home", "Library", "Settings"]
    private var broken = 0

    func testSwipeBetweenTabs() {
        minato.launch()
        let welcome = minato.buttons["Continue"]
        if welcome.waitForExistence(timeout: 3) { welcome.tap() }
        XCTAssertTrue(minato.tabBars.firstMatch.waitForExistence(timeout: 10), "no tab bar")
        select("Home")

        // Fast, back and forth.
        for round in 0..<3 {
            swipe(left: true, velocity: .fast, "fast left \(round) a")
            swipe(left: true, velocity: .fast, "fast left \(round) b")
            swipe(left: false, velocity: .fast, "fast right \(round) a")
            swipe(left: false, velocity: .fast, "fast right \(round) b")
        }
        // Slowly.
        swipe(left: true, velocity: .slow, "slow left a")
        swipe(left: true, velocity: .slow, "slow left b")
        swipe(left: false, velocity: .slow, "slow right a")
        swipe(left: false, velocity: .slow, "slow right b")
        // Let go before half the way: the page springs back.
        select("Library")
        for side in [true, false, true, false] {
            cancelled(left: side, "let go \(side ? "left" : "right")")
        }
        // Right after a tap on a tab, both ways.
        for (tab, left) in [("Library", true), ("Settings", false), ("Home", true), ("Library", false),
                            ("Settings", false), ("Home", true)] {
            select(tab)
            swipe(left: left, velocity: .fast, "after the tap on \(tab), \(left ? "left" : "right")")
        }
        // A tap on the tab that a swipe just left, then on the one it came to.
        swipe(left: true, velocity: .fast, "before the taps")
        for tab in ["Home", "Library", "Settings", "Library", "Home"] {
            select(tab)
            check("the tap on \(tab)", expected: tab)
        }
        XCTAssertEqual(broken, 0, "\(broken) broken pages")
    }

    /// What quick fingers do and XCUITest cannot: Minato's own DEBUG swipes (MINATO_TAB_SWIPE_STRESS) start a swipe
    /// while the last one still slides, tap a tab while a page slides and flick, the finger lifted before UIKit has
    /// started the pages; then the page must be whole, and the bar's taps and the swipes work again.
    func testSwipeWhileAPageSlides() {
        for stress in ["back", "tap", "flick-left", "flick-right"] {
            minato.terminate()
            minato.launchEnvironment["MINATO_TAB_SWIPE_STRESS"] = stress
            minato.launch()
            let welcome = minato.buttons["Continue"]
            if welcome.waitForExistence(timeout: 3) { welcome.tap() }
            XCTAssertTrue(minato.tabBars.firstMatch.waitForExistence(timeout: 10), "no tab bar")
            // The case runs 2 s after the first appearance and settles within a second.
            Thread.sleep(forTimeInterval: 4)
            check("stress \(stress)", expected: selectedTab ?? "none")
            // The bar's taps and the swipes work again.
            select("Home")
            check("stress \(stress), then the tap on Home", expected: "Home")
            swipe(left: true, velocity: .fast, "stress \(stress), then a swipe left")
        }
        XCTAssertEqual(broken, 0, "\(broken) broken pages")
    }

    // MARK: - Gestures

    private var selectedTab: String? {
        tabs.first { minato.tabBars.buttons[$0].isSelected }
    }

    private func select(_ tab: String) {
        minato.tabBars.buttons[tab].tap()
    }

    /// A swipe across the middle of the page, to the left (to the next tab) or to the right.
    private func swipe(left: Bool, velocity: XCUIGestureVelocity, _ name: String) {
        let before = selectedTab ?? "?"
        let start = minato.coordinate(withNormalizedOffset: CGVector(dx: left ? 0.85 : 0.15, dy: 0.55))
        let end = minato.coordinate(withNormalizedOffset: CGVector(dx: left ? 0.1 : 0.9, dy: 0.55))
        start.press(forDuration: 0.02, thenDragTo: end, withVelocity: velocity, thenHoldForDuration: 0)
        let index = tabs.firstIndex(of: before) ?? 0
        let next = left ? min(index + 1, tabs.count - 1) : max(index - 1, 0)
        check(name, expected: tabs[next])
    }

    /// A slow swipe a third of the way, held, then let go: the page goes back to where it was.
    private func cancelled(left: Bool, _ name: String) {
        let before = selectedTab ?? "?"
        let index = tabs.firstIndex(of: before) ?? 0
        let target = left ? index + 1 : index - 1
        guard tabs.indices.contains(target) else { return }
        let start = minato.coordinate(withNormalizedOffset: CGVector(dx: left ? 0.7 : 0.3, dy: 0.55))
        let end = minato.coordinate(withNormalizedOffset: CGVector(dx: left ? 0.45 : 0.55, dy: 0.55))
        start.press(forDuration: 0.02, thenDragTo: end, withVelocity: .slow, thenHoldForDuration: 0.4)
        check(name, expected: before)
    }

    // MARK: - The page

    /// Right after the gesture and once it has settled: the tab bar selects `expected`, and its page fills the
    /// window and is not black.
    private func check(_ name: String, expected: String) {
        let early = darkShare()
        // The page's springs and the bar's selection settle within a second.
        Thread.sleep(forTimeInterval: 1.0)
        let settled = darkShare()
        let selected = selectedTab ?? "none"
        let window = minato.windows.firstMatch.frame
        let bars = minato.navigationBars.allElementsBoundByIndex.map(\.frame).filter { $0.intersects(window) }
        let placed = bars.contains { abs($0.minX - window.minX) < 1 && abs($0.width - window.width) < 1 }
        let line = "\(name): tab \(selected), page placed \(placed) (\(bars.map { Int($0.minX) })), black "
            + String(format: "%.2f then %.2f", early, settled)
        print("MINATO-UITEST \(line)")
        if selected != expected || !placed || settled > 0.5 {
            broken += 1
            let attachment = XCTAttachment(screenshot: XCUIScreen.main.screenshot())
            attachment.name = name
            attachment.lifetime = .keepAlways
            add(attachment)
            XCTFail("broken page after \(line); expected tab \(expected)")
        }
    }

    /// The share of near-black pixels in the page's middle, between its navigation bar and the tab bar.
    private func darkShare() -> Double {
        guard let image = XCUIScreen.main.screenshot().image.cgImage else { return 0 }
        let width = 60, height = 130
        var pixels = [UInt8](repeating: 0, count: width * height * 4)
        let drawn = pixels.withUnsafeMutableBytes { buffer -> Bool in
            guard let context = CGContext(data: buffer.baseAddress, width: width, height: height, bitsPerComponent: 8,
                                          bytesPerRow: width * 4, space: CGColorSpaceCreateDeviceRGB(),
                                          bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else {
                return false
            }
            context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
            return true
        }
        guard drawn else { return 0 }
        var dark = 0, all = 0
        for row in (height * 25 / 100)..<(height * 80 / 100) {
            for column in 0..<width {
                let at = (row * width + column) * 4
                if max(pixels[at], pixels[at + 1], pixels[at + 2]) < 24 { dark += 1 }
                all += 1
            }
        }
        return Double(dark) / Double(all)
    }
}
