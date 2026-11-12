import XCTest

/// A new user's first run, as a release candidate ships it (Release: no Debug hooks), by the user's own taps: the
/// welcome screen, Continue, the empty library, "+" with a test APK from Files until its tile is Ready, the start into
/// game mode, the menu by the swipe down from the top edge (the three-finger swipe is not something XCUITest can do),
/// Quit back to the library with no "quit unexpectedly" alert, and Settings. Minato must be freshly installed, and
/// Files' On My iPhone must hold the folder TEST_RUNNER_MINATO_FOLDER with TEST_RUNNER_MINATO_APK (java-test.apk)
/// in it: tools/first-run-check.sh does both. Each step prints its time as "MINATO-UITEST time <step> <seconds>".
@MainActor
final class FirstRunUITests: XCTestCase {
    private let minato = XCUIApplication(bundleIdentifier: "io.github.kisame76.minato")
    private let environment = ProcessInfo.processInfo.environment

    func testFirstRun() {
        continueAfterFailure = false
        let folder = environment["MINATO_FOLDER"] ?? "Minato First Run"
        let apk = environment["MINATO_APK"] ?? "java-test.apk"
        let name = (apk as NSString).deletingPathExtension

        // 1. The welcome screen of the first launch.
        var started = Date()
        minato.launch()
        XCTAssertTrue(minato.staticTexts["Welcome to Minato"].waitForExistence(timeout: 20), "no welcome screen")
        time("welcome", since: started)

        // 2. Continue: the empty library with its "+".
        minato.buttons["Continue"].tap()
        XCTAssertTrue(minato.tabBars.firstMatch.waitForExistence(timeout: 10), "no tab bar after Continue")
        let add = minato.buttons["Add APK"]
        XCTAssertTrue(add.waitForExistence(timeout: 10), "no + in the library")
        XCTAssertFalse(tile(name).exists, "the new library already holds \(name)")

        // 3. "+", Files, the folder, the APK, Open: the tile appears and becomes Ready.
        add.tap()
        openFolder(folder)
        let file = minato.descendants(matching: .any).matching(NSPredicate(format: "label BEGINSWITH %@", name))
            .firstMatch
        XCTAssertTrue(file.waitForExistence(timeout: 15), "no \(apk) in the folder \(folder)")
        file.tap()
        let open = button(["Open", "Öffnen"])
        XCTAssertTrue(open.waitForExistence(timeout: 10), "no Open in the picker")
        started = Date()
        open.tap()
        XCTAssertTrue(tile(name).waitForExistence(timeout: 30), "no tile for \(name) after the import")
        let ready = minato.descendants(matching: .any).matching(NSPredicate(format: "label == 'Ready'")).firstMatch
        XCTAssertTrue(ready.waitForExistence(timeout: 120), "\(name) did not become Ready")
        time("import", since: started)
        XCTAssertFalse(alertExists(), "an alert after the import: \(alertText())")

        // 4. The start: Minato restarts itself in game mode (no tab bar), and nothing says it could not start.
        started = Date()
        tile(name).tap()
        let inGameMode = waitUntil(60) {
            self.minato.state == .runningForeground && !self.minato.tabBars.firstMatch.exists
        }
        XCTAssertTrue(inGameMode, "no game mode within 60 s")
        time("start", since: started)
        XCTAssertFalse(alertExists(), "an alert at the start: \(alertText())")
        sleep(5)  // the app's first screen; XCUITest cannot see into the guest's window

        // 5. The menu by the swipe down from the top edge (iOS gives the app only every other one: up to 3 tries).
        let quit = minato.buttons["Quit"]
        for _ in 0..<3 where !quit.exists {
            let top = minato.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.01))
            let middle = minato.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.4))
            top.press(forDuration: 0.05, thenDragTo: middle)
            _ = quit.waitForExistence(timeout: 3)
        }
        XCTAssertTrue(quit.exists, "no menu with Quit after three swipes down from the top edge")

        // 6. Quit: back in the library, with no "quit unexpectedly".
        started = Date()
        quit.tap()
        XCTAssertTrue(minato.tabBars.firstMatch.waitForExistence(timeout: 60), "no library after Quit")
        time("quit", since: started)
        let unexpected = minato.staticTexts.matching(NSPredicate(format: "label CONTAINS 'quit unexpectedly'"))
            .firstMatch
        XCTAssertFalse(unexpected.waitForExistence(timeout: 5), "\"quit unexpectedly\" after a plain Quit")

        // 7. Settings, as a new user sees it.
        minato.tabBars.buttons["Settings"].tap()
        XCTAssertTrue(minato.staticTexts["Version"].waitForExistence(timeout: 10), "no Version row in Settings")
        print("MINATO-UITEST first run done")
    }

    private func tile(_ name: String) -> XCUIElement {
        minato.buttons.matching(NSPredicate(format: "label CONTAINS %@", name)).firstMatch
    }

    private func button(_ labels: [String]) -> XCUIElement {
        minato.buttons.matching(NSPredicate(format: "label IN %@", labels)).firstMatch
    }

    /// The picker shows its last place: the folder itself, On My iPhone, or Browse; its words follow the system's
    /// language (English or German here).
    private func openFolder(_ folder: String) {
        let row = minato.descendants(matching: .any).matching(NSPredicate(format: "label == %@", folder)).firstMatch
        if row.waitForExistence(timeout: 10) { row.tap(); return }
        let browse = button(["Browse", "Durchsuchen"])
        if browse.exists { browse.tap() }
        let onPhone = minato.descendants(matching: .any)
            .matching(NSPredicate(format: "label IN %@", ["On My iPhone", "Auf meinem iPhone"])).firstMatch
        XCTAssertTrue(onPhone.waitForExistence(timeout: 10), "no On My iPhone in the picker")
        onPhone.tap()
        XCTAssertTrue(row.waitForExistence(timeout: 10), "no folder \(folder) in On My iPhone")
        row.tap()
    }

    private func alertExists() -> Bool { minato.alerts.firstMatch.exists }

    private func alertText() -> String {
        minato.alerts.firstMatch.staticTexts.allElementsBoundByIndex.map(\.label).joined(separator: " ")
    }

    private func time(_ step: String, since start: Date) {
        print("MINATO-UITEST time \(step) \(String(format: "%.1f", Date().timeIntervalSince(start)))")
    }

    private func waitUntil(_ seconds: TimeInterval, _ condition: () -> Bool) -> Bool {
        let end = Date().addingTimeInterval(seconds)
        while Date() < end {
            if condition() { return true }
            RunLoop.current.run(until: Date().addingTimeInterval(0.5))
        }
        return condition()
    }
}
