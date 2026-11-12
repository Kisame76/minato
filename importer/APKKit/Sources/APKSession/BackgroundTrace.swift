import APKLibrary
import Foundation

/// Minato's own lines in the session log (SessionLog.ownTag) about each trip to the background, so that a run on a
/// phone tells when the guest answered its pause and its stop, how often and how long the commit gate held a thread
/// in the background (apkrt_background_gate), and what the memory did across the trip: the footprint and ANGLE's
/// texture and depth counts at didEnterBackground and 10 s after the return. Counts and times only: no app name, no
/// guest text. Times are uptime nanoseconds.
public struct BackgroundTrace: Sendable {
    /// When the memory after a return is taken.
    public static let afterReturn: Duration = .seconds(10)

    /// The trips so far, from 1.
    public private(set) var trip = 0
    /// The running trip's didEnterBackground, nil between trips.
    public private(set) var enteredAt: UInt64?
    private var before = ""
    private var back: (trip: Int, seconds: Double, parked: UInt32, longest: UInt64)?

    public init() {}

    /// didEnterBackground, with apkrt_memory_summary's line then: a new trip, and its line.
    public mutating func entered(at time: UInt64, memory summary: String) -> String {
        trip += 1
        enteredAt = time
        before = Self.memory(summary)
        back = nil
        return SessionLog.ownTag + "trip \(trip) to the background: \(before)"
    }

    /// Whether the host times the guest's answer to `callback`: a pause or a stop during a trip.
    public func times(_ callback: Lifecycle.Callback) -> Bool {
        enteredAt != nil && (callback == .onPause || callback == .onStop)
    }

    /// The scene is active again, with the commit gate's waits of the trip (apkrt_background_gate: how many, the
    /// longest in ns): the trip that ended, whose memory the host takes `afterReturn` later, or nil.
    public mutating func returned(at time: UInt64, gateParked: UInt32, gateLongest: UInt64) -> Int? {
        guard let enteredAt else { return nil }
        self.enteredAt = nil
        back = (trip, Double(time &- enteredAt) / 1e9, gateParked, gateLongest)
        return trip
    }

    /// The return's line with the memory before the trip and `after`, apkrt_memory_summary's line 10 s after it; nil
    /// when another trip began meanwhile.
    public func returnLine(trip: Int, after summary: String) -> String? {
        guard let back, back.trip == trip else { return nil }
        let seconds = String(format: "%.1f", back.seconds)
        return SessionLog.ownTag + "trip \(trip) back after \(seconds) s: \(before) before, "
            + "\(Self.memory(summary)) 10 s after; commit gate: \(back.parked) parked, longest "
            + "\(back.longest / 1_000_000) ms"
    }

    /// The line for the guest's answer to the pause or stop of trip `trip`: the wait's result (1 answered, 0 the time
    /// is up, else the session ended) and when it came.
    public static func answerLine(trip: Int, callback: Lifecycle.Callback, result: Int32, at time: UInt64,
                                  enteredAt: UInt64) -> String {
        let name = callback == .onPause ? "pause" : callback == .onStop ? "stop" : "\(callback)"
        let ms = time >= enteredAt ? (time - enteredAt) / 1_000_000 : 0
        let outcome = switch result {
        case 1: "answered"
        case 0: "not answered"
        default: "unanswered, the session ended"
        }
        return SessionLog.ownTag + "trip \(trip) \(name) \(outcome) \(ms) ms after the background"
    }

    /// The footprint, ANGLE's texture and depth counts of apkrt_memory_summary's line.
    static func memory(_ summary: String) -> String {
        let fields = summary.split(separator: " ")
        let foot = fields.first { $0.hasPrefix("foot=") }.map(String.init) ?? "foot=?"
        let counts = fields.filter { $0.hasPrefix("tex=") || $0.hasPrefix("depth=") }
        return ([foot] + counts.map(String.init)).joined(separator: " ")
    }
}
