/// A motion event as `apkrt_input_post` takes it (ADR-0036, decision 3; `struct apkrt_motion_event` in
/// guest_interface.h): the arrays flat, the samples oldest first with the event's own last.
public struct MotionRecord: Sendable, Equatable {
    /// AMOTION_EVENT_ACTION_*, with the pointer's index << 8 for POINTER_DOWN and POINTER_UP.
    public var action: Int32
    public var downTime: Int64
    /// In the order of the pointers: ascending.
    public var pointerIDs: [Int32]
    /// The history's times, then the event's.
    public var sampleTimes: [Int64]
    /// sampleTimes.count × pointerIDs.count places, by sample, then pointer index.
    public var places: [MotionEvent.Place]
}

extension MotionEvent {
    /// A pointer's place in one sample (`struct apkrt_pointer_coords`).
    public struct Place: Sendable, Equatable {
        public var x: Float
        public var y: Float
        public var pressure: Float
        public var touchMajor: Float

        public init(x: Float, y: Float, pressure: Float, touchMajor: Float) {
            self.x = x
            self.y = y
            self.pressure = pressure
            self.touchMajor = touchMajor
        }

        init(_ pointer: Pointer) {
            self.init(x: pointer.x, y: pointer.y, pressure: pointer.pressure, touchMajor: pointer.touchMajor)
        }
    }

    /// The event for the input channel. Of more samples than the channel holds, the runtime drops the oldest.
    public var record: MotionRecord {
        let samples = history.map { ($0.time, $0.pointers) } + [(eventTime, pointers)]
        return MotionRecord(
            action: action.code, downTime: downTime, pointerIDs: pointers.map(\.id),
            sampleTimes: samples.map(\.0), places: samples.flatMap { $0.1.map(Place.init) })
    }
}

extension MotionEvent.Action {
    /// AMOTION_EVENT_ACTION_* (android/input.h).
    var code: Int32 {
        switch self {
        case .down: 0
        case .up: 1
        case .move: 2
        case .cancel: 3
        case .pointerDown(let index): 5 | Int32(index) << 8
        case .pointerUp(let index): 6 | Int32(index) << 8
        }
    }
}
