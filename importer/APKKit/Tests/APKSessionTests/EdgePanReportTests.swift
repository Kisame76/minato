import Testing

@testable import APKSession

/// The Debug log of pans near the top edge (ADR-0032, decision 9): where each starts, whether it is the menu's
/// swipe, and whether it reached the menu.
@Suite("EdgePanReport")
struct EdgePanReportTests {
    let topEdge = 40.0

    @Test func aPanWithin3TopEdgesIsLoggedInPortrait() {
        #expect(EdgePanReport.isNearTop(startY: 0, topEdge: topEdge))
        #expect(EdgePanReport.isNearTop(startY: 119, topEdge: topEdge))
        #expect(!EdgePanReport.isNearTop(startY: 120, topEdge: topEdge))
        #expect(!EdgePanReport.isNearTop(startY: 600, topEdge: topEdge))
    }

    /// The view spans the screen, so its top is the screen's in landscape too.
    @Test func theBandIsTheSameInLandscape() {
        let start = EdgePanReport.start(x: 700, y: 30, width: 932, height: 430, velocityX: -40, velocityY: 900,
                                        topEdge: topEdge)
        #expect(start?.verdict == .accepted)
        #expect(start?.line == "edge pan 700 30 in 932x430 velocity -40 900 accepted")
        #expect(EdgePanReport.start(x: 700, y: 130, width: 932, height: 430, velocityX: 0, velocityY: 900,
                                    topEdge: topEdge) == nil)
    }

    @Test func aDownwardPanAtTheTopIsAccepted() {
        let start = EdgePanReport.start(x: 215.4, y: 12.6, width: 430, height: 932, velocityX: 20, velocityY: 800,
                                        topEdge: topEdge)
        #expect(start?.verdict == .accepted)
        #expect(start?.line == "edge pan 215 13 in 430x932 velocity 20 800 accepted")
    }

    @Test func aPanBelowTheEdgeIsRefusedForItsStart() {
        let start = EdgePanReport.start(x: 60, y: 55, width: 430, height: 932, velocityX: 0, velocityY: 800,
                                        topEdge: topEdge)
        #expect(start?.verdict == .refusedStart)
        #expect(start?.line == "edge pan 60 55 in 430x932 velocity 0 800 refused: start")
    }

    @Test func aSidewaysPanIsRefusedForItsDirection() {
        let start = EdgePanReport.start(x: 60, y: 10, width: 430, height: 932, velocityX: 900, velocityY: 300,
                                        topEdge: topEdge)
        #expect(start?.verdict == .refusedDirection)
        #expect(start?.line == "edge pan 60 10 in 430x932 velocity 900 300 refused: direction")
    }

    /// The start goes first: a pan below the edge that also goes sideways is refused for its start.
    @Test func theStartIsCheckedFirst() {
        let start = EdgePanReport.start(x: 60, y: 80, width: 430, height: 932, velocityX: 900, velocityY: 0,
                                        topEdge: topEdge)
        #expect(start?.verdict == .refusedStart)
    }

    @Test func theEndSaysWhetherTheMenuOpened() {
        #expect(EdgePanReport.end(distance: 84.7, menu: true) == "edge pan end 85 menu yes")
        #expect(EdgePanReport.end(distance: 31, menu: false) == "edge pan end 31 menu no")
    }
}
