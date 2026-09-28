import Testing

@testable import APKSession

/// ADR-0036, decision 3: a motion event as `apkrt_input_post` takes it, flat.
@Suite("MotionRecord")
struct MotionRecordTests {
    static func pointer(_ id: Int32, _ x: Float) -> MotionEvent.Pointer {
        MotionEvent.Pointer(id: id, x: x, y: x + 1, pressure: 0.5, touchMajor: 20)
    }

    static func place(_ x: Float) -> MotionEvent.Place {
        MotionEvent.Place(x: x, y: x + 1, pressure: 0.5, touchMajor: 20)
    }

    @Test func theActionsHaveAndroidsCodes() {
        let codes = [MotionEvent.Action.down, .up, .move, .cancel, .pointerDown(1), .pointerUp(2)].map {
            MotionEvent(action: $0, downTime: 1, eventTime: 2, pointers: [Self.pointer(0, 10)]).record.action
        }
        // AMOTION_EVENT_ACTION_POINTER_DOWN 5 and _UP 6, with the pointer's index << 8.
        #expect(codes == [0, 1, 2, 3, 5 | 1 << 8, 6 | 2 << 8])
    }

    @Test func aTapIsOneSampleOfOnePointer() {
        let record = MotionEvent(action: .down, downTime: 100, eventTime: 100, pointers: [Self.pointer(0, 10)]).record

        #expect(record.downTime == 100)
        #expect(record.pointerIDs == [0])
        #expect(record.sampleTimes == [100])
        #expect(record.places == [Self.place(10)])
    }

    @Test func theHistoryComesFirstThenTheEventsOwnSampleByPointer() {
        let event = MotionEvent(
            action: .move, downTime: 100, eventTime: 300, pointers: [Self.pointer(0, 30), Self.pointer(2, 31)],
            history: [
                .init(time: 200, pointers: [Self.pointer(0, 10), Self.pointer(2, 11)]),
                .init(time: 250, pointers: [Self.pointer(0, 20), Self.pointer(2, 21)]),
            ])
        let record = event.record

        #expect(record.pointerIDs == [0, 2])
        #expect(record.sampleTimes == [200, 250, 300])
        #expect(record.places == [10, 11, 20, 21, 30, 31].map(Self.place))
    }
}
