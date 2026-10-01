import Testing

@testable import APKSession

/// ADR-0032, decision 5 and test 4: the orientation a session keeps, from the launcher activity's
/// `android:screenOrientation` (ActivityInfo's constants, which aapt2 writes for the attribute's names).
@Suite("Session orientation")
struct SessionOrientationTests {
    @Test(arguments: [
        (0, "landscape"), (6, "sensorLandscape"), (8, "reverseLandscape"), (11, "userLandscape"),
    ])
    func holdsBothLandscapeSidesForALandscapeValue(value: Int, name: String) {
        #expect(SessionOrientation(screenOrientation: value) == .landscape, "\(name)")
    }

    @Test(arguments: [
        (1, "portrait"), (7, "sensorPortrait"), (9, "reversePortrait"), (12, "userPortrait"),
        // Android gives nosensor the natural orientation (ROTATION_0 in DisplayRotation), a phone's portrait.
        (5, "nosensor"),
    ])
    func holdsPortraitForAPortraitValue(value: Int, name: String) {
        #expect(SessionOrientation(screenOrientation: value) == .portrait, "\(name)")
    }

    @Test(arguments: [
        (-1, "unspecified"), (2, "user"), (3, "behind"), (4, "sensor"), (10, "fullSensor"),
        (13, "fullUser"), (14, "locked"), (15, "a value from a later Android"), (-2, "no value Android knows"),
    ])
    func keepsTheOrientationAtStartOtherwise(value: Int, name: String) {
        #expect(SessionOrientation(screenOrientation: value) == .atStart, "\(name)")
    }

    @Test func keepsTheOrientationAtStartWithoutAValue() {
        #expect(SessionOrientation(screenOrientation: nil) == .atStart)
    }

    /// The guest starts with the window's final size: game mode waits for a layout in the orientation it
    /// holds, since iOS turns the window only after the request.
    @Test func aWindowFitsTheOrientationItIsLaidOutIn() {
        #expect(SessionOrientation.landscape.fits(width: 2868, height: 1320))
        #expect(!SessionOrientation.landscape.fits(width: 1320, height: 2868))
        #expect(SessionOrientation.portrait.fits(width: 1320, height: 2868))
        #expect(!SessionOrientation.portrait.fits(width: 2868, height: 1320))
        // The orientation at start is the one the window has; iOS refused to turn it.
        #expect(SessionOrientation.atStart.fits(width: 2868, height: 1320))
        #expect(SessionOrientation.atStart.fits(width: 1320, height: 2868))
    }

    // ADR-0038, decisions 1.2 and 1.3: a session turns where Android turns the activity without creating it anew.

    /// `orientation` and `screenSize` of ActivityInfo's CONFIG_ constants.
    static let turnFlags = 0x0080 | 0x0400

    @Test(arguments: [
        (-1, "unspecified"), (2, "user"), (3, "behind"), (4, "sensor"), (10, "fullSensor"), (13, "fullUser"),
        (15, "a value from a later Android"),
    ])
    func turnsWithBothFlags(value: Int, name: String) {
        #expect(SessionOrientation(screenOrientation: value, configChanges: Self.turnFlags, targetSdk: 35) == .turns, "\(name)")
    }

    @Test func turnsWithBothFlagsWithoutAValue() {
        #expect(SessionOrientation(screenOrientation: nil, configChanges: Self.turnFlags | 0x0020, targetSdk: 35) == .turns)
    }

    /// Below targetSdk 13 a turn changes no screen size for the app (PackageParser adds CONFIG_SCREEN_SIZE to it).
    @Test func needsOnlyOrientationBelowTargetSdk13() {
        #expect(SessionOrientation(screenOrientation: nil, configChanges: 0x0080, targetSdk: 12) == .turns)
        #expect(SessionOrientation(screenOrientation: nil, configChanges: 0x0080, targetSdk: 13) == .atStart)
    }

    /// Android creates the activity anew at a turn; Minato keeps the orientation at start instead.
    @Test(arguments: [nil, 0x0080, 0x0400, 0x0020] as [Int?])
    func keepsTheOrientationAtStartWithoutBothFlags(flags: Int?) {
        #expect(SessionOrientation(screenOrientation: -1, configChanges: flags, targetSdk: 35) == .atStart)
    }

    @Test func aFixedOrLockedValueDoesNotTurnWithTheFlags() {
        #expect(SessionOrientation(screenOrientation: 6, configChanges: Self.turnFlags, targetSdk: 35) == .landscape)
        #expect(SessionOrientation(screenOrientation: 1, configChanges: Self.turnFlags, targetSdk: 35) == .portrait)
        #expect(SessionOrientation(screenOrientation: 5, configChanges: Self.turnFlags, targetSdk: 35) == .portrait)
        #expect(SessionOrientation(screenOrientation: 14, configChanges: Self.turnFlags, targetSdk: 35) == .atStart)
    }

    @Test func aTurningSessionAllowsPortraitAndBothLandscapeSides() {
        #expect(SessionOrientation.turns.orientations == [.portrait, .landscape])
        #expect(SessionOrientation.landscape.orientations == .landscape)
        #expect(SessionOrientation.portrait.orientations == .portrait)
    }

    @Test func anyLaidOutWindowFitsATurningSession() {
        #expect(SessionOrientation.turns.fits(width: 2868, height: 1320))
        #expect(SessionOrientation.turns.fits(width: 1320, height: 2868))
    }

    @Test(arguments: [SessionOrientation.landscape, .portrait, .atStart, .turns])
    func aWindowWithoutASizeFitsNone(orientation: SessionOrientation) {
        #expect(!orientation.fits(width: 0, height: 0))
        #expect(!orientation.fits(width: 1320, height: 0))
    }
}
