/// Decision 6, the rules of one run: why it is invalid, empty for a valid run. Whether the layer's
/// size and drawable count match the other variant's spans runs, so BenchPlan checks that.
public enum RunValidity {
    public static let lowPower = "low_power"
    public static let thermalStart = "thermal_start"
    public static let thermalAboveFair = "thermal_above_fair"
    public static let linkPeriod = "link_period"
    public static let debugger = "debugger"
    public static let focusLoss = "focus_loss"

    public static func reasons(for record: BenchRunRecord) -> [String] {
        var reasons: [String] = []
        if record.lowPower { reasons.append(lowPower) }
        if record.thermalStart != .nominal { reasons.append(thermalStart) }
        if ([record.thermalStart] + record.thermalChanges.map(\.state)).contains(where: { $0 > .fair }) {
            reasons.append(thermalAboveFair)
        }
        let period = 1e9 / Double(record.run.configuration.rate)
        // An APK that asked for no rate (requested_rate 0) runs at the panel's rate (ADR-0042): judged by its frame
        // times, so the link period is no reason for it. Every other run, the twin included, must match its rate.
        let unasked = record.run.variant == .apk && record.requestedRate == 0
        let offPeriod = record.linkPeriodMedianNs <= 0 || abs(Double(record.linkPeriodMedianNs) - period) > period / 100
        if !unasked && offPeriod {
            reasons.append(linkPeriod)
        }
        if record.debugger { reasons.append(debugger) }
        if !record.focusLosses.isEmpty { reasons.append(focusLoss) }
        let run = record.run, godot = run.configuration.scene == .godot
        // Test 2's script ends with its done file, which counts the rows of its scene.csv (ADR-0042, decision 4).
        if godot && record.doneFrames != record.sceneFrames {
            reasons.append("done \(record.doneFrames), frames \(record.sceneFrames)")
        }
        // A drawable taken before frame 0 would shift every frame by one. The GLES twin's CAEAGLLayer gives no
        // drawables: its frames are the script's alone.
        let drawn = !(godot && run.variant == .twin && run.configuration.api == .gles)
        if (drawn && record.sceneFrames != record.drawables) || record.sceneFrames == 0 {
            reasons.append("frames \(record.sceneFrames), drawables \(record.drawables)")
        }
        return reasons
    }
}
