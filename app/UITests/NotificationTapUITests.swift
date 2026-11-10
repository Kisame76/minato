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

    /// UIKit's keyboard over Minato's session: its keys h, i and Return tapped, as Android 15's reference types them
    /// (ime-surrounding-test-return, which hides the keyboard at Return as Ren'Py's name field does). Its keys, not
    /// typeText: the session's text view is no element XCTest can give the focus.
    func testKeyboardReturn() {
        continueAfterFailure = false
        let keyboard = minato.keyboards.firstMatch
        XCTAssertTrue(keyboard.waitForExistence(timeout: 10), "no keyboard over Minato")
        for letter in ["h", "i"] {
            let key = keyboard.keys[letter]
            XCTAssertTrue(key.waitForExistence(timeout: 5), "the keyboard has no key \(letter)")
            key.tap()
        }
        let labels = ["return", "Return", "Zeilenumbruch", "Eingabe"]
        let predicate = NSPredicate(format: "identifier ==[c] 'Return' OR label IN %@", labels)
        let button = keyboard.buttons.matching(predicate).firstMatch
        let enter = button.waitForExistence(timeout: 2) ? button : keyboard.keys.matching(predicate).firstMatch
        XCTAssertTrue(enter.exists, "the keyboard has no return key")
        enter.tap()
        print("MINATO-UITEST tapped h, i and Return on the keyboard")
    }

    /// The bar's key that hides the keyboard (KeyboardBarView, "Hide Keyboard"), tapped once it is there.
    func testKeyboardHide() {
        continueAfterFailure = false
        let hide = minato.buttons["Hide Keyboard"]
        XCTAssertTrue(hide.waitForExistence(timeout: 10), "no Hide Keyboard key over Minato's keyboard")
        hide.tap()
        print("MINATO-UITEST tapped the bar's Hide Keyboard key")
    }

    /// A tap outside the guest's text field while UIKit's keyboard is up: at a fifth of Minato's window's height, in
    /// its middle (ime-outside-test, whose fields lie lower).
    func testTapOutsideField() {
        continueAfterFailure = false
        XCTAssertTrue(minato.wait(for: .runningForeground, timeout: 10), "Minato is not in front")
        minato.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.2)).tap()
        print("MINATO-UITEST tapped outside the field")
    }

    /// Two swipes up from the bottom edge of Minato's window, as the user leaves a full-screen app for the Home Screen.
    /// Game mode defers every edge (defersSystemGestures, GameModeView): iOS gives the first swipe to the app and
    /// shows the home indicator, and only a second swipe right after it goes to the system (device-regressions entry
    /// 3, the landscape edge's "popup"; java-start-check's edge-swipe round). Notes say where each swipe went.
    func testSwipeUpFromBottomTwice() {
        continueAfterFailure = false
        let minato = minato
        XCTAssertTrue(minato.wait(for: .runningForeground, timeout: 10), "Minato is not in front")
        // A window in landscape: the device turns too, as the user holds it (the Simulator's stays upright).
        if minato.frame.width > minato.frame.height, XCUIDevice.shared.orientation != .landscapeLeft {
            XCUIDevice.shared.orientation = .landscapeLeft
            sleep(2)
        }
        print("MINATO-UITEST window \(Int(minato.frame.width))x\(Int(minato.frame.height)) points, device "
              + "orientation \(XCUIDevice.shared.orientation.rawValue)")
        defer { XCUIDevice.shared.orientation = .portrait }
        swipeUpFromBottom(of: minato)
        if waitUntil(2, { minato.state != .runningForeground }) {
            print("MINATO-UITEST the first swipe: Minato left the front")
            return
        }
        print("MINATO-UITEST the first swipe: Minato in front")
        swipeUpFromBottom(of: minato)
        let left = waitUntil(5) { minato.state != .runningForeground }
        print("MINATO-UITEST the second swipe: Minato \(left ? "left the front" : "in front")")
    }

    /// A swipe up from the very bottom edge of `app`'s window, across its lower half.
    private func swipeUpFromBottom(of app: XCUIApplication) {
        let bottom = app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.999))
        bottom.press(forDuration: 0.05, thenDragTo: app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.4)))
    }

    /// iOS's alert for Minato's request, such as the notifications prompt of a Simulator that never answered it, is
    /// answered Allow within TEST_RUNNER_MINATO_WAIT seconds, 90 by default (its button that allows, in English or
    /// German). TEST_RUNNER_MINATO_ARG "localnetwork": only iOS's local network alert ("… Find and Connect to Devices
    /// on Your Local Network"), which the iPhone shows at a guest's first connection into the local network.
    func testAllowAlert() {
        answerAlert(allow: true)
    }

    /// The same alert answered Don't Allow, as the user's refusal.
    func testDenyAlert() {
        answerAlert(allow: false)
    }

    private func answerAlert(allow: Bool) {
        continueAfterFailure = false
        let environment = ProcessInfo.processInfo.environment
        let alert = environment["MINATO_ARG"] == "localnetwork"
            ? springboard.alerts.matching(NSPredicate(format: "label CONTAINS[c] %@ OR label CONTAINS[c] %@",
                                                      "Local Network", "lokalen Netzwerk")).firstMatch
            : springboard.alerts.firstMatch
        let seconds = environment["MINATO_WAIT"].flatMap { Double($0) } ?? 90
        XCTAssertTrue(alert.waitForExistence(timeout: seconds), "no alert within \(Int(seconds)) s")
        let labels = allow ? ["Allow", "Erlauben"] : ["Don’t Allow", "Don't Allow", "Nicht erlauben"]
        let button = alert.buttons.matching(NSPredicate(format: "label IN %@", labels)).firstMatch
        XCTAssertTrue(button.exists, "the alert has no \(allow ? "Allow" : "Don't Allow") button")
        button.tap()
        print("MINATO-UITEST answered an alert \(allow ? "Allow" : "Don't Allow")")
    }

    /// Minato's own Local Network switch in Settings (Settings > Minato > Local Network, which iOS lists once a guest
    /// connected into the local network; the Simulator has no such switch): TEST_RUNNER_MINATO_ARG "read" prints its
    /// state, "on" or "off" sets it and prints the state before and after. It opens Minato's page through Settings >
    /// Apps and touches nothing else; Settings goes again at the end. The check reads the state first and sets it back
    /// at its end.
    func testLocalNetworkSetting() {
        continueAfterFailure = false
        let settings = XCUIApplication(bundleIdentifier: "com.apple.Preferences")
        settings.launch()
        defer { settings.terminate() }
        // Settings > Apps lists every app, with a search field of its own (Settings' main search does not index them);
        // its rows are buttons.
        let row = { (label: String) in
            settings.buttons.matching(NSPredicate(format: "label ==[c] %@", label)).firstMatch
        }
        let apps = row("Apps")
        for _ in 0..<6 where !apps.waitForExistence(timeout: 1) { settings.swipeUp() }
        XCTAssertTrue(apps.exists, "Settings has no Apps row")
        apps.tap()
        let search = settings.searchFields.firstMatch
        if search.waitForExistence(timeout: 5) {
            search.tap()
            search.typeText("Minato")
        }
        let page = row("Minato")
        XCTAssertTrue(page.waitForExistence(timeout: 10), "Settings' Apps list has no Minato")
        page.tap()
        let labels = ["Local Network", "Lokales Netzwerk"]
        let toggle = settings.switches.matching(NSPredicate(format: "label IN %@", labels)).firstMatch
        XCTAssertTrue(toggle.waitForExistence(timeout: 10), "Minato's settings page has no Local Network switch")
        let isOn = { (toggle.value as? String) == "1" }
        print("MINATO-UITEST local network was \(isOn() ? "on" : "off")")
        let want = ProcessInfo.processInfo.environment["MINATO_ARG"]
        if want == "on" || want == "off", isOn() != (want == "on") {
            toggle.tap()
            XCTAssertTrue(waitUntil(5) { isOn() == (want == "on") }, "the switch did not turn \(want ?? "")")
        }
        print("MINATO-UITEST local network is \(isOn() ? "on" : "off")")
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
