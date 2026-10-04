/// ProcessInfo.ThermalState, as the CSV writes it.
public enum BenchThermal: String, Sendable, Comparable, CaseIterable {
    case nominal, fair, serious, critical

    public static func < (a: Self, b: Self) -> Bool {
        allCases.firstIndex(of: a)! < allCases.firstIndex(of: b)!
    }
}

/// A frame of a run (decision 5). Times are host time, the guest's CLOCK_MONOTONIC, in ns.
public struct BenchFrame: Equatable, Sendable {
    public var index: Int
    public var tickNs: Int64
    public var nextDrawableNs: Int64
    /// 0: the drawable never reached the screen.
    public var presentedNs: Int64
    /// The render thread's CPU time for the frame, measured by the scene.
    public var cpuNs: Int64

    public init(index: Int, tickNs: Int64, nextDrawableNs: Int64, presentedNs: Int64, cpuNs: Int64) {
        self.index = index
        self.tickNs = tickNs
        self.nextDrawableNs = nextDrawableNs
        self.presentedNs = presentedNs
        self.cpuNs = cpuNs
    }
}

/// A run's conditions (decision 5), one row of run.csv.
public struct BenchRunRecord: Equatable, Sendable {
    public struct ThermalChange: Equatable, Sendable {
        public var timeNs: Int64
        public var state: BenchThermal

        public init(timeNs: Int64, state: BenchThermal) {
            self.timeNs = timeNs
            self.state = state
        }
    }

    public var run: BenchRun
    /// The scene's N for the load.
    public var meshes = 0
    public var requestedRate: Float = 0
    public var maxFPS: Int = 0
    /// The display link's median period during the run, 0 without ticks.
    public var linkPeriodMedianNs: Int64 = 0
    public var thermalStart: BenchThermal = .nominal
    public var thermalChanges: [ThermalChange] = []
    public var lowPower = false
    public var batteryState = "unknown"
    /// 0 to 1, -1 when unknown.
    public var batteryLevel: Float = -1
    public var qos = ""
    public var pixelFormat = ""
    public var drawableWidth = 0
    public var drawableHeight = 0
    public var maxDrawableCount = 0
    public var commit = ""
    public var configuration = ""
    public var debugger = false
    /// When the app lost focus, in host time.
    public var focusLosses: [Int64] = []
    public var startNs: Int64 = 0
    /// 0: no frame reached the screen.
    public var firstPresentNs: Int64 = 0
    /// Rows of the scene's scene.csv and drawables the layer recorded; not columns of run.csv, the
    /// reason for unequal counts names them.
    public var sceneFrames = 0
    public var drawables = 0
    /// The frames test 2's done file counts (ADR-0042 c); 0 for ADR-0037's scene, which writes none.
    public var doneFrames = 0

    public init(run: BenchRun) {
        self.run = run
    }

    /// The state at the end: the last change, else the state at the start.
    public var thermalEnd: BenchThermal { thermalChanges.last?.state ?? thermalStart }
}

public enum BenchCSVError: Error, Equatable {
    case malformed(String)
}

/// frames.csv and run.csv of a run (decision 5), which tools/bench-summary.py reads.
public enum BenchCSV {
    public static let framesHeader = "index,tick_ns,next_drawable_ns,presented_ns,cpu_ns"
    public static let runHeader = """
        run,variant,api,rate,load,meshes,requested_rate,max_fps,link_period_median_ns,thermal_start,thermal_end,\
        thermal_changes,low_power,battery_state,battery_level,qos,pixel_format,drawable_width,drawable_height,\
        max_drawable_count,commit,configuration,debugger,focus_losses,start_ns,first_present_ns,valid,\
        invalid_reasons
        """

    public static func frames(_ frames: [BenchFrame]) -> String {
        lines([framesHeader] + frames.map {
            "\($0.index),\($0.tickNs),\($0.nextDrawableNs),\($0.presentedNs),\($0.cpuNs)"
        })
    }

    /// The header and the record's row, with its validity (RunValidity).
    public static func run(_ record: BenchRunRecord) -> String {
        let reasons = RunValidity.reasons(for: record)
        let configuration = record.run.configuration
        let fields: [String] = [
            record.run.description, record.run.variant.rawValue, configuration.api.rawValue, "\(configuration.rate)",
            configuration.load.rawValue, "\(record.meshes)", "\(record.requestedRate)", "\(record.maxFPS)",
            "\(record.linkPeriodMedianNs)", record.thermalStart.rawValue, record.thermalEnd.rawValue,
            record.thermalChanges.map { "\($0.timeNs)=\($0.state.rawValue)" }.joined(separator: "|"),
            flag(record.lowPower), record.batteryState, "\(record.batteryLevel)", record.qos, record.pixelFormat,
            "\(record.drawableWidth)", "\(record.drawableHeight)", "\(record.maxDrawableCount)", record.commit,
            record.configuration, flag(record.debugger), record.focusLosses.map(String.init).joined(separator: "|"),
            "\(record.startNs)", "\(record.firstPresentNs)", flag(reasons.isEmpty), reasons.joined(separator: "|"),
        ]
        return lines([runHeader, fields.map(quoted).joined(separator: ",")])
    }

    /// What the plan needs from a run.csv.
    public static func result(fromRun text: String) throws -> BenchResult {
        let rows = try parse(text)
        let header = runHeader.split(separator: ",").map(String.init)
        guard rows.count == 2, rows[0] == header, rows[1].count == header.count else {
            throw BenchCSVError.malformed("not a run.csv")
        }
        let field = Dictionary(uniqueKeysWithValues: zip(header, rows[1]))
        guard let run = BenchRun(field["run"]!), let width = Int(field["drawable_width"]!),
            let height = Int(field["drawable_height"]!), let count = Int(field["max_drawable_count"]!),
            ["0", "1"].contains(field["valid"]!)
        else { throw BenchCSVError.malformed(rows[1].joined(separator: ",")) }
        return BenchResult(run: run, valid: field["valid"] == "1",
                           invalidReasons: field["invalid_reasons"]!.split(separator: "|").map(String.init),
                           drawableWidth: width, drawableHeight: height, maxDrawableCount: count)
    }

    static func flag(_ value: Bool) -> String { value ? "1" : "0" }

    static func lines(_ lines: [String]) -> String { lines.map { $0 + "\n" }.joined() }

    /// RFC 4180: a field with a comma, a quote or a line break goes in quotes, its quotes doubled.
    static func quoted(_ field: String) -> String {
        guard field.contains(where: { $0 == "," || $0 == "\"" || $0.isNewline }) else { return field }
        return "\"" + field.replacing("\"", with: "\"\"") + "\""
    }

    /// The rows of a CSV text, RFC 4180 quoting undone.
    static func parse(_ text: String) throws -> [[String]] {
        var rows: [[String]] = []
        var row: [String] = []
        var field = ""
        var inQuotes = false
        var characters = text.makeIterator()
        var pending = characters.next()
        while let character = pending {
            pending = characters.next()
            if inQuotes {
                if character != "\"" {
                    field.append(character)
                } else if pending == "\"" {
                    field.append("\"")
                    pending = characters.next()
                } else {
                    inQuotes = false
                }
            } else if character == "\"" && field.isEmpty {
                inQuotes = true
            } else if character == "," {
                row.append(field)
                field = ""
            } else if character.isNewline {
                row.append(field)
                rows.append(row)
                row = []
                field = ""
            } else {
                field.append(character)
            }
        }
        guard !inQuotes else { throw BenchCSVError.malformed("an open quote") }
        if !row.isEmpty || !field.isEmpty {
            row.append(field)
            rows.append(row)
        }
        return rows
    }
}
