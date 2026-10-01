import Testing

@testable import APKSession

/// ADR-0038, decision 1.1: the library stays in portrait; game mode allows what Info.plist does until its
/// session holds its own.
@Suite("Interface orientations")
struct InterfaceOrientationsTests {
    @Test(arguments: [
        LaunchMode.library(nil), .library(.appNotFound("org.example.gone")),
    ])
    func theLibraryIsPortraitOnly(mode: LaunchMode) {
        #expect(mode.orientations == .portrait)
    }

    @Test func gameModeAllowsPortraitAndBothLandscapeSidesUntilItsSessionHolds() throws {
        let mode = LaunchMode.gameMode(try LaunchRequestTests.entry())

        #expect(mode.orientations == [.portrait, .landscapeLeft, .landscapeRight])
    }

    @Test func landscapeIsBothSides() {
        #expect(InterfaceOrientations.landscape == [.landscapeLeft, .landscapeRight])
        #expect(!InterfaceOrientations.landscape.contains(.portrait))
    }
}
