import XCTest

/// Library's Select mode (LibraryView): its bottom bar takes the tab bar's place, as in Files. The library must hold
/// an app, or Select is off. tools/tab-swipe-check.sh runs it with TESTS=SelectModeUITests.
@MainActor
final class SelectModeUITests: XCTestCase {
    private let minato = XCUIApplication(bundleIdentifier: "io.github.kisame76.minato")

    private func startSelecting() {
        minato.launch()
        let welcome = minato.buttons["Continue"]
        if welcome.waitForExistence(timeout: 3) { welcome.tap() }
        minato.tabBars.buttons["Library"].tap()
        minato.buttons["More"].tap()
        minato.buttons["Select"].tap()
        XCTAssertTrue(minato.buttons["Select All"].waitForExistence(timeout: 5), "no Select All in Select mode")
    }

    /// The user's regression: the bottom bar's keys sat behind the tab bar. Each must be hittable, clear of a tab bar
    /// that is still shown.
    func testSelectBarAboveTheTabBar() {
        startSelecting()
        let bar = minato.tabBars.firstMatch
        let barFrame: CGRect? = bar.exists && bar.isHittable ? bar.frame : nil
        for name in ["Select All", "Favorite", "Delete"] {
            let button = minato.buttons[name]
            let covered = barFrame.map { $0.intersects(button.frame) } ?? false
            print("MINATO-UITEST \(name): hittable \(button.isHittable), frame \(button.frame), "
                  + "tab bar \(String(describing: barFrame))")
            XCTAssertTrue(button.isHittable && !covered, "\(name) is behind the tab bar")
        }
        minato.buttons["Select All"].tap()
        XCTAssertTrue(minato.staticTexts.matching(NSPredicate(format: "label ENDSWITH 'Selected'")).firstMatch
            .waitForExistence(timeout: 5), "Select All selected nothing")
    }
}
