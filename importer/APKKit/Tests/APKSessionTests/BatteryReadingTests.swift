import Testing

@testable import APKSession

/// The battery: UIDevice's battery as apkrt_battery_set takes it.
@Suite("BatteryReading")
struct BatteryReadingTests {
    @Test func theLevelIsWholePercent() {
        #expect(BatteryReading(level: 1, state: 2).percent == 100)
        #expect(BatteryReading(level: 0.5, state: 1).percent == 50)
        #expect(BatteryReading(level: 0.005, state: 1).percent == 1)
        #expect(BatteryReading(level: 0, state: 1).percent == 0)
    }

    @Test func anUnknownLevelIsMinusOne() {
        #expect(BatteryReading(level: -1, state: 0).percent == -1)
        #expect(BatteryReading(level: .nan, state: 0).percent == -1)
    }

    @Test func aLevelOutsideZeroToOneIsClamped() {
        #expect(BatteryReading(level: 1.2, state: 3).percent == 100)
    }

    @Test func theStateIsUIKitsRawValue() {
        #expect(BatteryReading(level: 0.4, state: 2).state == 2)
        #expect(BatteryReading(level: 0.4, state: 7).state == 0)
    }
}
