import XCTest

/// Report a Problem (ReportSheet): by default the report names no app, and naming it is the user's own switch, after
/// which the text shows the name. The library must hold an app named by MINATO_REPORT_APP ("Activity Test",
/// activity-test's label, by default), or the round says so. tools/tab-swipe-check.sh runs it with
/// TESTS=ReportSheetUITests.
@MainActor
final class ReportSheetUITests: XCTestCase {
    private let minato = XCUIApplication(bundleIdentifier: "io.github.kisame76.minato")

    func testTheReportNamesTheAppOnlyByTheUsersSwitch() {
        let name = ProcessInfo.processInfo.environment["MINATO_REPORT_APP"] ?? "Activity Test"
        minato.launch()
        let welcome = minato.buttons["Continue"]
        if welcome.waitForExistence(timeout: 3) { welcome.tap() }
        minato.tabBars.buttons["Library"].tap()
        let tile = minato.buttons.matching(NSPredicate(format: "label CONTAINS %@", name)).firstMatch
        guard tile.waitForExistence(timeout: 10) else {
            print("MINATO-UITEST precondition not met: no app \(name) in the library (import it first)")
            XCTFail("precondition not met: no app \(name) in the library")
            return
        }
        tile.press(forDuration: 1.2)
        let reportButton = minato.buttons["Report a Problem"]
        XCTAssertTrue(reportButton.waitForExistence(timeout: 5), "no Report a Problem in the tile's menu")
        reportButton.tap()

        let nameSwitch = minato.switches["Include the App's Name"]
        XCTAssertTrue(nameSwitch.waitForExistence(timeout: 5), "no switch to name the app")
        XCTAssertEqual(nameSwitch.value as? String, "0", "naming the app is on by default")
        let report = minato.textViews["Report"]
        XCTAssertTrue(report.waitForExistence(timeout: 5), "no report text")
        let before = report.value as? String ?? ""
        print("MINATO-UITEST report without the name:\n\(before)")
        XCTAssertFalse(before.contains(name), "the report names the app without the user's switch")
        XCTAssertFalse(before.contains("Package:"), "the report names the package without the user's switch")

        // iOS's switch cell holds the control itself since iOS 17; a tap on the cell's label does not turn it.
        let control = nameSwitch.switches.firstMatch
        (control.exists ? control : nameSwitch).tap()
        let after = report.value as? String ?? ""
        print("MINATO-UITEST report with the name:\n\(after)")
        XCTAssertTrue(after.contains("App: \(name)"), "the switch does not add the app's name")
        XCTAssertTrue(after.contains("Package: "), "the switch does not add the package")
        minato.buttons["Cancel"].tap()
    }
}
