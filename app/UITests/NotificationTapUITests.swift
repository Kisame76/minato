import XCTest

/// SpringBoard's part of a notification's tap, for tools/java-start-check.sh's NOTIFY_REAL rounds: the runner opens
/// Notification Center and taps the notification as the user does, and never launches Minato itself, so that iOS
/// launches or resumes Minato as for the user's tap. The check reads what Minato did from its session events.
@MainActor
final class NotificationTapUITests: XCTestCase {
    private var springboard: XCUIApplication { XCUIApplication(bundleIdentifier: "com.apple.springboard") }
    private var minato: XCUIApplication { XCUIApplication(bundleIdentifier: "io.github.kisame76.minato") }

    /// The Home Screen comes to the front, Minato goes to the background.
    func testGoHome() {
        continueAfterFailure = false
        XCUIDevice.shared.press(.home)
        let minato = minato
        XCTAssertTrue(waitUntil(10) { [.runningBackground, .runningBackgroundSuspended].contains(minato.state) },
                      "Minato is not in the background: state \(minato.state.rawValue)")
    }

    /// The notification with the text TEST_RUNNER_MINATO_NOTIFICATION (default "Tap me", notify-tap-test's title),
    /// its banner or its entry in Notification Center, is tapped; Minato must be in front within 10 s.
    func testTapNotification() {
        continueAfterFailure = false
        let springboard = springboard, minato = minato
        let environment = ProcessInfo.processInfo.environment
        let text = environment["MINATO_NOTIFICATION"] ?? "Tap me"
        // The notification's cell is a button whose label holds its title and text.
        let notification = springboard.buttons.matching(NSPredicate(format: "label CONTAINS %@", text)).firstMatch
        // The banner of a notification that iOS delivers in the next TEST_RUNNER_MINATO_WAIT seconds, if it comes.
        _ = notification.waitForExistence(timeout: environment["MINATO_WAIT"].flatMap { Double($0) } ?? 0)
        // A drag down from the top edge's left part (its right part is Control Center's). A full-screen app may
        // defer the edge (preferredScreenEdgesDeferringSystemGestures): then its first drag goes to the app.
        var dragged = false
        for _ in 0..<3 where !notification.waitForExistence(timeout: 2) {
            dragged = true
            let top = springboard.coordinate(withNormalizedOffset: CGVector(dx: 0.3, dy: 0.0))
            top.press(forDuration: 0.1, thenDragTo: springboard.coordinate(withNormalizedOffset: CGVector(dx: 0.3,
                                                                                                         dy: 0.6)))
        }
        XCTAssertTrue(notification.waitForExistence(timeout: 5), "no notification \"\(text)\" in Notification Center")
        // The check's output says which of the two the user's tap was.
        print("MINATO-UITEST tapped the \(dragged ? "entry in Notification Center" : "banner")")
        notification.tap()
        // A stack of Minato's notifications opens at the first tap; its newest is tapped then.
        if !waitUntil(3, { minato.state == .runningForeground }) && notification.exists {
            notification.tap()
        }
        // A tap in Notification Center that iOS treats as the Lock Screen's (the Simulator's) waits for the unlock,
        // the user's swipe up from the bottom edge, and opens the app then.
        if !waitUntil(3, { minato.state == .runningForeground }) {
            swipeUpFromBottom()
        }
        XCTAssertTrue(minato.wait(for: .runningForeground, timeout: 10), "Minato is not in front after the tap")
    }

    /// The Lock Screen goes, as the user's swipe up from the bottom edge unlocks a device without a passcode (the
    /// Simulator's after its boot): a notification tapped on the Lock Screen opens nothing until then.
    func testUnlock() {
        XCUIDevice.shared.press(.home)
        swipeUpFromBottom()
        sleep(1)
    }

    private func swipeUpFromBottom() {
        let springboard = springboard
        let bottom = springboard.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.995))
        bottom.press(forDuration: 0.05, thenDragTo: springboard.coordinate(withNormalizedOffset: CGVector(dx: 0.5,
                                                                                                         dy: 0.2)))
    }

    /// UIKit's keyboard over Minato's session: its delete key is tapped once (for an empty field, where Android's
    /// keyboard still deletes; tools/java-start-check.sh's ime-surrounding-test-delete round).
    func testKeyboardDelete() {
        continueAfterFailure = false
        let keyboard = minato.keyboards.firstMatch
        XCTAssertTrue(keyboard.waitForExistence(timeout: 10), "no keyboard over Minato")
        let delete = keyboard.keys.matching(NSPredicate(format: "identifier ==[c] 'delete' OR label IN %@",
                                                        ["delete", "Delete", "Löschen"])).firstMatch
        XCTAssertTrue(delete.waitForExistence(timeout: 5), "the keyboard has no delete key")
        delete.tap()
        print("MINATO-UITEST tapped the keyboard's delete key")
    }

    /// iOS's alert for Minato's request, such as the notifications prompt of a Simulator that never answered it, is
    /// answered Allow within TEST_RUNNER_MINATO_WAIT seconds, 90 by default (its button that allows, in English or
    /// German).
    func testAllowAlert() {
        continueAfterFailure = false
        let alert = springboard.alerts.firstMatch
        let seconds = ProcessInfo.processInfo.environment["MINATO_WAIT"].flatMap { Double($0) } ?? 90
        XCTAssertTrue(alert.waitForExistence(timeout: seconds), "no alert within \(Int(seconds)) s")
        let allow = alert.buttons.matching(NSPredicate(format: "label IN %@", ["Allow", "Erlauben"])).firstMatch
        XCTAssertTrue(allow.exists, "the alert has no Allow button")
        allow.tap()
        print("MINATO-UITEST answered an alert Allow")
    }

    /// An alert of SpringBoard's that is still up goes, by its first button (a prompt's "Don't Allow"), with a note:
    /// it outlives the app that asked and would hold up the next launch.
    func testDismissAlert() {
        let alert = springboard.alerts.firstMatch
        guard alert.waitForExistence(timeout: 2) else { return }
        alert.buttons.element(boundBy: 0).tap()
        print("MINATO-UITEST an alert of SpringBoard's was still up: dismissed by its first button")
    }

    private func waitUntil(_ seconds: TimeInterval, _ condition: () -> Bool) -> Bool {
        let end = Date().addingTimeInterval(seconds)
        while !condition() {
            if Date() > end { return false }
            RunLoop.current.run(until: Date().addingTimeInterval(0.2))
        }
        return true
    }
}
