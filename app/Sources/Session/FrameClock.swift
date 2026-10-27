import APKRuntime
import APKSession
import Foundation
import QuartzCore

/// The frame clock (ADR-0036, decision 2): a CADisplayLink on a host thread of its own, never the main
/// thread, so SwiftUI's work does not delay its ticks. It ticks only while the gate is open, 60 to 120 times
/// a second, preferring 120 or the rate the guest asks for, and at least the app's FPS unlock (ADR-0066 decision 3).
@MainActor
final class DisplayLinkClock {
    /// The link and its run loop are only touched on the clock's thread; the other threads hand it work.
    nonisolated(unsafe) private let link: CADisplayLink
    nonisolated(unsafe) private let runLoop: CFRunLoop
    /// The guest's rate and the app's unlock, 0 for none; only touched on the clock's thread.
    nonisolated(unsafe) private var preferred: Float = 0
    nonisolated(unsafe) private var unlock: Float = 0
    private var isRunning = false

    init(sink: FrameTickSink) {
        let target = TickTarget(sink: sink)
        let started = DispatchSemaphore(value: 0)
        nonisolated(unsafe) var made: (CADisplayLink, CFRunLoop)?
        let thread = Thread {
            let link = CADisplayLink(target: target, selector: #selector(TickTarget.tick(_:)))
            link.isPaused = true
            link.preferredFrameRateRange = Self.caRange(FramePacing.range(preferred: 0))
            link.add(to: .current, forMode: .common)
            made = (link, CFRunLoopGetCurrent())
            started.signal()
            // The link keeps the run loop from running out.
            while true { RunLoop.current.run() }
        }
        thread.name = "Minato frame clock"
        thread.qualityOfService = .userInteractive
        thread.start()
        started.wait()
        (link, runLoop) = made!
    }

    /// Ticks while the gate is open.
    func run(_ on: Bool) {
        guard on != isRunning else { return }
        isRunning = on
        #if DEBUG
        SessionCheck.event(on ? "frame clock running" : "frame clock paused")
        #endif
        perform { self.link.isPaused = !on }
    }

    /// The rate the guest prefers, 0 for none (`ANativeWindow_setFrameRate`); from any thread.
    nonisolated func setPreferredRate(_ rate: Float) {
        perform {
            self.preferred = rate
            self.applyRange()
        }
    }

    /// The app's FPS unlock for this session, 0 for none, set before the program starts.
    func setUnlock(_ rate: Float) {
        perform {
            self.unlock = rate
            self.applyRange()
        }
    }

    /// On the clock's thread.
    nonisolated private func applyRange() {
        link.preferredFrameRateRange = Self.caRange(FramePacing.range(preferred: preferred, unlock: unlock))
    }

    nonisolated private func perform(_ work: @escaping @Sendable () -> Void) {
        CFRunLoopPerformBlock(runLoop, CFRunLoopMode.commonModes.rawValue, work)
        CFRunLoopWakeUp(runLoop)
    }

    nonisolated private static func caRange(_ range: FrameRateRange) -> CAFrameRateRange {
        CAFrameRateRange(minimum: range.minimum, maximum: range.maximum, preferred: range.preferred)
    }

    /// The display link's target: it retains it, so the clock's thread lives as long as the process.
    private final class TickTarget: NSObject, @unchecked Sendable {
        let sink: FrameTickSink

        init(sink: FrameTickSink) {
            self.sink = sink
        }

        @objc func tick(_ link: CADisplayLink) {
            sink.tick(FrameTick(timestamp: link.timestamp, targetTimestamp: link.targetTimestamp))
        }
    }
}

/// The ticks to the guest's vsync channels (ADR-0036, decision 1); the Debug build shows session-check the
/// intervals between them.
final class VsyncChannelSink: FrameTickSink, @unchecked Sendable {
    #if DEBUG
    /// Only on the clock's thread.
    private var last: FrameTick?
    private var intervals: [Int64] = []
    #endif

    func tick(_ tick: FrameTick) {
        apkrt_vsync_post(tick.time, tick.targetTime)
        #if DEBUG
        defer { last = tick }
        // The first tick after a pause starts anew.
        guard let last, tick.time - last.time < 500_000_000 else { return }
        intervals.append(tick.time - last.time)
        guard intervals.count == 120 else { return }
        let summary = SessionCheck.frames(intervals, period: tick.period, onMain: Thread.isMainThread)
        intervals = []
        DispatchQueue.main.async { SessionCheck.event(summary) }
        #endif
    }
}
