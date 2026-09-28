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
        if record.linkPeriodMedianNs <= 0 || abs(Double(record.linkPeriodMedianNs) - period) > period / 100 {
            reasons.append(linkPeriod)
        }
        if record.debugger { reasons.append(debugger) }
        if !record.focusLosses.isEmpty { reasons.append(focusLoss) }
        // A drawable taken before frame 0 would shift every frame by one.
        if record.sceneFrames != record.drawables || record.sceneFrames == 0 {
            reasons.append("frames \(record.sceneFrames), drawables \(record.drawables)")
        }
        return reasons
    }
}
