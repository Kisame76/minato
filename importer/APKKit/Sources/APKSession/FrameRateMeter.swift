/// The FPS label's number (ADR-0032, decision 9): the frames presented in the last second, counted at the
/// window's gate (`apkrt_window_presents`) and sampled about once a second.
public struct FrameRateMeter: Sendable {
    private var last: (window: UInt64, presents: UInt64, time: Double)?

    public init() {}

    /// Window `window` had presented `presents` frames so far at `time` seconds. Returns its frames per
    /// second since the last sample, nil when there is nothing to compare with: the first sample, a new
    /// window, no window (0), or a counter back at 0, which `apkrt_window_presents` gives once the window is
    /// removed. A sample without time passing keeps the last one.
    public mutating func sample(window: UInt64, presents: UInt64, at time: Double) -> Int? {
        guard window != 0 else {
            last = nil
            return nil
        }
        guard let last, last.window == window, presents >= last.presents else {
            last = (window, presents, time)
            return nil
        }
        let elapsed = time - last.time
        guard elapsed > 0 else { return nil }
        self.last = (window, presents, time)
        return Int((Double(presents - last.presents) / elapsed).rounded())
    }
}
