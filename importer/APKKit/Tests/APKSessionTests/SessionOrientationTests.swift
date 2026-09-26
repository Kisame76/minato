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
}
