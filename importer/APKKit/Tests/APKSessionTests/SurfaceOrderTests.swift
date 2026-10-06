import Testing

@testable import APKSession

/// ADR-0043 c4 (d): where a SurfaceView's layer lies among the others on its side of the window (apkrt.h's surface
/// handler: below the window's layer for a negative z and above it for a positive one, by z, then in make order).
@Suite("SurfaceOrder")
struct SurfaceOrderTests {
    typealias Key = SurfaceOrder.Key

    @Test func aSideIsBelowTheWindowForANegativeZ() {
        #expect(SurfaceOrder.isBelowWindow(z: -2))
        #expect(SurfaceOrder.isBelowWindow(z: -1))
        #expect(!SurfaceOrder.isBelowWindow(z: 1))
        #expect(!SurfaceOrder.isBelowWindow(z: 2))
    }

    @Test func aNewLayerGoesByZThenAfterTheOnesMadeBefore() {
        let side = [Key(z: -2, made: 1), Key(z: -2, made: 3), Key(z: -1, made: 2)]
        #expect(SurfaceOrder.index(of: Key(z: -2, made: 4), among: side) == 2)
        #expect(SurfaceOrder.index(of: Key(z: -1, made: 4), among: side) == 3)
        #expect(SurfaceOrder.index(of: Key(z: -3, made: 4), among: side) == 0)
        #expect(SurfaceOrder.index(of: Key(z: 1, made: 1), among: []) == 0)
    }

    /// A place with another z puts the layer back in its make order among the ones of that z.
    @Test func aPlacedLayerKeepsItsMakeOrder() {
        let side = [Key(z: -2, made: 1), Key(z: -2, made: 3)]
        #expect(SurfaceOrder.index(of: Key(z: -2, made: 2), among: side) == 1)
        #expect(Key(z: -2, made: 3) < Key(z: -1, made: 1))
    }
}
