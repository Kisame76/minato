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

    /// A swipe in from the left edge of Minato's window, above the keyboard: the guest's edge back (ADR-0041 decision
    /// 10), which hides the keyboard as Android 15's BACK does (the keyboard glass round of tools/java-start-check.sh).
    func testSwipeFromLeftEdge() {
        continueAfterFailure = false
        XCTAssertTrue(minato.wait(for: .runningForeground, timeout: 10), "Minato is not in front")
        let start = minato.coordinate(withNormalizedOffset: CGVector(dx: 0, dy: 0.35))
            .withOffset(CGVector(dx: 2, dy: 0))
        let end = minato.coordinate(withNormalizedOffset: CGVector(dx: 0.7, dy: 0.35))
        start.press(forDuration: 0.05, thenDragTo: end)
        print("MINATO-UITEST swiped in from the left edge")
    }

    /// The device turns as the user turns it: TEST_RUNNER_MINATO_ARG "landscape" (left) or "portrait", with the app in
    /// front (the keyboard glass round of tools/java-start-check.sh, the keyboard up through the turn).
    func testTurn() {
        continueAfterFailure = false
        XCTAssertTrue(minato.wait(for: .runningForeground, timeout: 10), "Minato is not in front")
        let landscape = ProcessInfo.processInfo.environment["MINATO_ARG"] == "landscape"
        XCUIDevice.shared.orientation = landscape ? .landscapeLeft : .portrait
        print("MINATO-UITEST turned the device to \(landscape ? "landscape" : "portrait")")
    }

    /// Two swipes up from the bottom edge of Minato's window, as the user leaves a full-screen app for the Home Screen.
    /// Game mode defers every edge (defersSystemGestures, GameModeView): iOS gives the first swipe to the app and
    /// shows the home indicator, and only a second swipe right after it goes to the system (device-regressions entry
    /// 3, the landscape edge's "popup"; java-start-check's edge-swipe round). Notes say where each swipe went.
    /// TEST_RUNNER_MINATO_ARG "control": the runner first launches Minato as the control of entry 3 (EdgeControl, a
    /// bare UIKit view controller with the same deferral, no session), notes the touches it got, and ends it after.
    func testSwipeUpFromBottomTwice() {
        continueAfterFailure = false
        let minato = minato
        let control = ProcessInfo.processInfo.environment["MINATO_ARG"] == "control"
        if control {
            minato.launchEnvironment["MINATO_EDGE_CONTROL"] = "1"
            minato.launch()
        }
        defer { if control { minato.terminate() } }
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
        if control { print("MINATO-UITEST \(minato.otherElements["edge-control"].label)") }
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

    /// Minato's own Local Network switch in Settings (the Simulator has no such switch): TEST_RUNNER_MINATO_ARG "read"
    /// prints its state, "on" or "off" sets it and prints the state before and after. Two ways to it, the first that
    /// finds a switch wins: Settings > Apps > Minato (its page), then Settings > Privacy & Security > Local Network
    /// (a list of apps, each with a switch, long, so it is scrolled). When neither shows it, the failure says what the
    /// runner saw as counts and kinds of elements only, never a name. Settings goes again at the end. The check reads
    /// the state first and sets it back at its end.
    func testLocalNetworkSetting() {
        continueAfterFailure = false
        let settings = XCUIApplication(bundleIdentifier: "com.apple.Preferences")
        var seen: [String] = []
        var found = switchOnAppsPage(settings, &seen)
        if found == nil {
            settings.terminate()
            found = switchInPrivacyList(settings, &seen)
        }
        defer { settings.terminate() }
        guard let toggle = found else {
            XCTFail("Minato's Local Network switch was not found; saw: " + seen.joined(separator: "; "))
            return
        }
        let isOn = { (toggle.value as? String) == "1" }
        print("MINATO-UITEST local network was \(isOn() ? "on" : "off")")
        let want = ProcessInfo.processInfo.environment["MINATO_ARG"]
        if want == "on" || want == "off", isOn() != (want == "on") {
            toggle.tap()
            XCTAssertTrue(waitUntil(5) { isOn() == (want == "on") }, "the switch did not turn \(want ?? "")")
        }
        print("MINATO-UITEST local network is \(isOn() ? "on" : "off")")
    }

    /// Counts and kinds of what a Settings screen shows: no label, no value, no name.
    private func census(_ app: XCUIApplication) -> String {
        "buttons \(app.buttons.count), switches \(app.switches.count), cells \(app.cells.count), static texts "
            + "\(app.staticTexts.count), tables \(app.tables.count), search fields \(app.searchFields.count)"
    }

    private func settingsRow(_ app: XCUIApplication, _ labels: [String]) -> XCUIElement {
        app.descendants(matching: .any).matching(NSPredicate(format: "label IN[c] %@", labels)).firstMatch
    }

    /// Scrolls up to `tries` times until the element is on screen.
    private func reveal(_ element: XCUIElement, in app: XCUIApplication, tries: Int = 10) -> Bool {
        for _ in 0..<tries {
            if element.waitForExistence(timeout: 1) && element.isHittable { return true }
            app.swipeUp()
        }
        return element.exists
    }

    private func appSwitch(_ app: XCUIApplication, _ names: [String]) -> XCUIElement {
        app.switches.matching(NSPredicate(format: "label IN[c] %@", names)).firstMatch
    }

    private func switchOnAppsPage(_ settings: XCUIApplication, _ seen: inout [String]) -> XCUIElement? {
        settings.launch()
        let apps = settingsRow(settings, ["Apps"])
        guard reveal(apps, in: settings) else {
            seen.append("Settings has no Apps row (\(census(settings)))")
            return nil
        }
        apps.tap()
        let search = settings.searchFields.firstMatch
        if search.waitForExistence(timeout: 5) {
            search.tap()
            search.typeText("Minato")
        }
        let page = settingsRow(settings, ["Minato"])
        guard page.waitForExistence(timeout: 10) else {
            seen.append("the Apps list has no Minato row (\(census(settings)))")
            return nil
        }
        page.tap()
        // The row's label differs by language and iOS version, and the page can be long: match by a part of the name.
        let toggle = settings.switches.matching(NSPredicate(
            format: "label CONTAINS[c] 'Local' OR label CONTAINS[c] 'Netzwerk'")).firstMatch
        guard reveal(toggle, in: settings) else {
            seen.append("Minato's page has no Local Network switch (\(census(settings)))")
            return nil
        }
        return toggle
    }

    private func switchInPrivacyList(_ settings: XCUIApplication, _ seen: inout [String]) -> XCUIElement? {
        settings.launch()
        let privacy = settingsRow(settings, ["Privacy & Security", "Datenschutz & Sicherheit"])
        guard reveal(privacy, in: settings) else {
            seen.append("Settings has no Privacy & Security row (\(census(settings)))")
            return nil
        }
        privacy.tap()
        let local = settingsRow(settings, ["Local Network", "Lokales Netzwerk"])
        guard reveal(local, in: settings) else {
            seen.append("Privacy & Security has no Local Network row (\(census(settings)))")
            return nil
        }
        local.tap()
        let toggle = appSwitch(settings, ["Minato"])
        guard reveal(toggle, in: settings, tries: 15) else {
            seen.append("the Local Network list has no Minato switch (\(census(settings)))")
            return nil
        }
        return toggle
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
