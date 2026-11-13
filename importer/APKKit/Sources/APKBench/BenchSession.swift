/// A recorded session (ADR-0041 d): a library app run in the benchmark build with
/// -recordSession <package>, its drawables recorded from before the guest's start to its end. One row of
/// session.csv: run.csv's conditions without the fields of a BenchRun, and the recording's own counts.
public struct BenchSessionRecord: Equatable, Sendable {
    public var package: String
    public var maxFPS = 0
    /// The display link's median period during the session, 0 without ticks.
    public var linkPeriodMedianNs: Int64 = 0
    public var thermalStart: BenchThermal = .nominal
    public var thermalChanges: [BenchRunRecord.ThermalChange] = []
    public var lowPower = false
    public var batteryState = "unknown"
    /// 0 to 1, -1 when unknown.
    public var batteryLevel: Float = -1
    public var commit = ""
    public var configuration = ""
    public var debugger = false
    public var qos = ""
    public var pixelFormat = ""
    public var drawableWidth = 0
    public var drawableHeight = 0
    public var maxDrawableCount = 0
    /// Drawables past the recording's capacity, not recorded.
    public var lost = 0
    /// Presented handlers that had not run when the recording stopped.
    public var unanswered = 0
    /// The program's exit status, nil when it never ended (a start that failed, the time limit).
    public var exitStatus: Int32?

    public init(package: String) {
        self.package = package
    }

    /// The state at the end: the last change, else the state at the start.
    public var thermalEnd: BenchThermal { thermalChanges.last?.state ?? thermalStart }

    /// The layer's values and the drawables past the capacity.
    public mutating func take(_ recording: BenchRecording) {
        qos = recording.qos
        pixelFormat = BenchRecording.pixelFormatName(recording.pixelFormat)
        drawableWidth = recording.drawableWidth
        drawableHeight = recording.drawableHeight
        maxDrawableCount = recording.maxDrawableCount
        lost = recording.lost
    }

    /// Drawable k is frame k only while every drawable is one of the guest's presents: nil when the layer gave
    /// as many drawables, recorded or lost, as the guest presented, else what differs.
    public static func drawableNote(drawables: Int, lost: Int, presents: UInt64) -> String? {
        let taken = drawables + lost
        guard UInt64(taken) != presents else { return nil }
        return "the layer gave \(taken) drawables for \(presents) presents of the guest"
    }
}

extension BenchPlan {
    /// A session's recording: 60 s at 120 Hz. Past that, drawables count as lost.
    public static let sessionRecordingCapacity = 7200
}

extension BenchCSV {
    public static let drawablesHeader = "frame,next_drawable_ns,presented_ns,tick_timed"
    public static let sessionHeader = """
        package,max_fps,link_period_median_ns,thermal_start,thermal_end,thermal_changes,low_power,battery_state,\
        battery_level,commit,configuration,debugger,qos,pixel_format,drawable_width,drawable_height,\
        max_drawable_count,lost,unanswered,exit_status
        """

    /// One row per drawable in the order the layer gave them, so row k is the guest's frame k; tick_timed 1 for a
    /// window's post by contents, whose presented_ns is the display link's refresh, not a present's time.
    public static func drawables(_ drawables: [BenchDrawable]) -> String {
        lines([drawablesHeader] + drawables.enumerated().map {
            "\($0),\($1.nextDrawableNs),\($1.presentedNs),\($1.tickTimed ? 1 : 0)"
        })
    }

    /// The header and the record's row.
    public static func session(_ record: BenchSessionRecord) -> String {
        let fields: [String] = [
            record.package, "\(record.maxFPS)", "\(record.linkPeriodMedianNs)", record.thermalStart.rawValue,
            record.thermalEnd.rawValue,
            record.thermalChanges.map { "\($0.timeNs)=\($0.state.rawValue)" }.joined(separator: "|"),
            flag(record.lowPower), record.batteryState, "\(record.batteryLevel)", record.commit, record.configuration,
            flag(record.debugger), record.qos, record.pixelFormat, "\(record.drawableWidth)",
            "\(record.drawableHeight)", "\(record.maxDrawableCount)", "\(record.lost)", "\(record.unanswered)",
            record.exitStatus.map(String.init) ?? "",
        ]
        return lines([sessionHeader, fields.map(quoted).joined(separator: ",")])
    }
}
