/// One sample of a `UITouch`: its place in the view's points, its force, its major radius in points, and
/// its time in seconds on the uptime clock (`UITouch.timestamp`).
public struct TouchSample: Sendable, Equatable {
    public var x: Double
    public var y: Double
    public var force: Double
    /// 0 on a screen without force.
    public var maximumForce: Double
    public var majorRadius: Double
    public var timestamp: Double

    public init(x: Double, y: Double, force: Double, maximumForce: Double, majorRadius: Double, timestamp: Double) {
        self.x = x
        self.y = y
        self.force = force
        self.maximumForce = maximumForce
        self.majorRadius = majorRadius
        self.timestamp = timestamp
    }
}

/// A touch in one of UIKit's touch callbacks.
public struct TouchChange: Sendable, Equatable {
    /// The touch's identity while it lasts: its object's address.
    public var touch: UInt
    /// `coalescedTouches(for:)`, oldest first; the last is the touch itself.
    public var samples: [TouchSample]

    public init(touch: UInt, samples: [TouchSample]) {
        self.touch = touch
        self.samples = samples
    }
}

/// UIKit's touch callbacks.
public enum TouchPhase: Sendable {
    case began, moved, ended, cancelled
}

/// Where the motion events go: the input channel's queue (ADR-0036, decision 3). The app implements it.
public protocol MotionEventSink: AnyObject {
    /// The events in order; never empty.
    func post(_ events: [MotionEvent])
}

/// An Android motion event from the touchscreen (`AMotionEvent`). Tool, source and device are the same
/// for every event and come with the input channel's record.
public struct MotionEvent: Sendable, Equatable {
    public enum Action: Sendable, Equatable {
        case down
        case up
        case move
        case cancel
        /// With the index of the pointer that went down.
        case pointerDown(Int)
        /// With the index of the pointer that goes up.
        case pointerUp(Int)
    }

    /// A pointer in window pixels.
    public struct Pointer: Sendable, Equatable {
        public var id: Int32
        public var x: Float
        public var y: Float
        public var pressure: Float
        public var touchMajor: Float

        public init(id: Int32, x: Float, y: Float, pressure: Float, touchMajor: Float) {
            self.id = id
            self.x = x
            self.y = y
            self.pressure = pressure
            self.touchMajor = touchMajor
        }
    }

    /// An older sample of a MOVE, with every pointer in the event's order.
    public struct Historical: Sendable, Equatable {
        public var time: Int64
        public var pointers: [Pointer]

        public init(time: Int64, pointers: [Pointer]) {
            self.time = time
            self.pointers = pointers
        }
    }

    public var action: Action
    /// The time of the gesture's DOWN, in ns on the guest's CLOCK_MONOTONIC.
    public var downTime: Int64
    public var eventTime: Int64
    /// In the order of their IDs.
    public var pointers: [Pointer]
    public var history: [Historical]

    public init(action: Action, downTime: Int64, eventTime: Int64, pointers: [Pointer], history: [Historical] = []) {
        self.action = action
        self.downTime = downTime
        self.eventTime = eventTime
        self.pointers = pointers
        self.history = history
    }
}

/// UIKit's touches as Android's motion events (ADR-0036, decision 3), one event per change, in the order
/// of AOSP's TouchInputMapper: a pointer's ID is the lowest free one and stays until it goes up, and the
/// pointers of an event are in the order of their IDs.
///
/// UIKit's times are on the uptime clock, which is the guest's CLOCK_MONOTONIC (F10).
public struct TouchTranslator: Sendable {
    /// Android's MAX_POINTERS: further fingers are ignored.
    public static let maxPointers = 16

    /// The window's pixels per point.
    public var scale: Double

    /// The gesture's pointers by touch. Touches not in it, those beyond `maxPointers` and those of a
    /// cancelled gesture, are ignored until they end.
    private var pointers: [UInt: MotionEvent.Pointer] = [:]
    private var downTime: Int64 = 0

    public init(scale: Double) {
        self.scale = scale
    }

    public mutating func began(_ changes: [TouchChange]) -> [MotionEvent] {
        var events: [MotionEvent] = []
        for change in changes {
            guard let sample = change.samples.last else { continue }
            guard pointers.count < Self.maxPointers else { continue }
            let time = Self.nanoseconds(sample.timestamp)
            if pointers.isEmpty { downTime = time }
            let id = Self.lowestFreeID(in: pointers.values)
            pointers[change.touch] = pointer(id, sample)
            let ordered = orderedPointers()
            let index = ordered.firstIndex { $0.id == id }!
            events.append(MotionEvent(action: ordered.count == 1 ? .down : .pointerDown(index), downTime: downTime,
                                      eventTime: time, pointers: ordered))
        }
        return events
    }

    /// The callback of `phase`.
    public mutating func translate(_ phase: TouchPhase, _ changes: [TouchChange]) -> [MotionEvent] {
        switch phase {
        case .began: began(changes)
        case .moved: moved(changes)
        case .ended: ended(changes)
        case .cancelled: cancelled(changes)
        }
    }

    /// One MOVE with every pointer; the coalesced samples before the last become its history.
    public mutating func moved(_ changes: [TouchChange]) -> [MotionEvent] {
        let known = changes.filter { pointers[$0.touch] != nil && !$0.samples.isEmpty }
        guard !known.isEmpty else { return [] }
        let eventTime = known.map { Self.nanoseconds($0.samples.last!.timestamp) }.max()!
        let times = Set(known.flatMap { $0.samples.dropLast().map { Self.nanoseconds($0.timestamp) } })
        var history: [MotionEvent.Historical] = []
        for time in times.sorted() {
            var then = pointers
            for change in known {
                if let sample = change.samples.last(where: { Self.nanoseconds($0.timestamp) <= time }) {
                    then[change.touch] = pointer(then[change.touch]!.id, sample)
                }
            }
            history.append(.init(time: time, pointers: Self.ordered(then)))
        }
        for change in known {
            pointers[change.touch] = pointer(pointers[change.touch]!.id, change.samples.last!)
        }
        return [MotionEvent(action: .move, downTime: downTime, eventTime: eventTime, pointers: orderedPointers(),
                            history: history)]
    }

    /// One POINTER_UP per finger in the order of the IDs, the last one UP, each at the finger's last place.
    public mutating func ended(_ changes: [TouchChange]) -> [MotionEvent] {
        var events: [MotionEvent] = []
        let known = changes.filter { pointers[$0.touch] != nil && !$0.samples.isEmpty }
        for change in known.sorted(by: { pointers[$0.touch]!.id < pointers[$1.touch]!.id }) {
            let sample = change.samples.last!
            let id = pointers[change.touch]!.id
            pointers[change.touch] = pointer(id, sample)
            let ordered = orderedPointers()
            let index = ordered.firstIndex { $0.id == id }!
            events.append(MotionEvent(action: ordered.count == 1 ? .up : .pointerUp(index), downTime: downTime,
                                      eventTime: Self.nanoseconds(sample.timestamp), pointers: ordered))
            pointers[change.touch] = nil
        }
        return events
    }

    /// UIKit cancels touches at the top-edge pan: the whole gesture ends with one CANCEL at the pointers'
    /// last places.
    public mutating func cancelled(_ changes: [TouchChange]) -> [MotionEvent] {
        let time = changes.filter { pointers[$0.touch] != nil }.compactMap { $0.samples.last?.timestamp }.max()
        return time.map { cancel(at: $0) } ?? []
    }

    /// The focus went while a gesture is open: it ends with one CANCEL at `timestamp`, seconds on the uptime
    /// clock.
    public mutating func focusLost(at timestamp: Double) -> [MotionEvent] {
        cancel(at: timestamp)
    }

    private mutating func cancel(at timestamp: Double) -> [MotionEvent] {
        guard !pointers.isEmpty else { return [] }
        let event = MotionEvent(action: .cancel, downTime: downTime, eventTime: Self.nanoseconds(timestamp),
                                pointers: orderedPointers())
        pointers = [:]
        return [event]
    }

    private func pointer(_ id: Int32, _ sample: TouchSample) -> MotionEvent.Pointer {
        MotionEvent.Pointer(
            id: id, x: Float(sample.x * scale), y: Float(sample.y * scale),
            pressure: sample.maximumForce > 0 ? Float(sample.force / sample.maximumForce) : 1,
            touchMajor: Float(2 * sample.majorRadius * scale))
    }

    private func orderedPointers() -> [MotionEvent.Pointer] {
        Self.ordered(pointers)
    }

    private static func ordered(_ pointers: [UInt: MotionEvent.Pointer]) -> [MotionEvent.Pointer] {
        pointers.values.sorted { $0.id < $1.id }
    }

    private static func lowestFreeID(in pointers: some Collection<MotionEvent.Pointer>) -> Int32 {
        let used = Set(pointers.map(\.id))
        return (0...).first { !used.contains($0) }!
    }

    private static func nanoseconds(_ seconds: Double) -> Int64 {
        Int64((seconds * 1e9).rounded())
    }
}
