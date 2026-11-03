import APKLibrary
import APKSession
import SwiftUI
import UIKit

/// Report a Problem (ADR-0070, the App's part 2): the report as editable text, the last session's log under its own
/// heading, and the line that it goes into a public issue, so that the user reads and trims it first. "Open GitHub"
/// copies it and opens the issue form with the fields it fills; "Share" hands it to any app. Nothing leaves the
/// iPhone without one of the two taps.
struct ReportSheet: View {
    let report: AppReport
    @State private var summary: String
    @State private var log: String
    @Environment(\.dismiss) private var dismiss
    @Environment(\.openURL) private var openURL

    init(report: AppReport) {
        self.report = report
        _summary = State(initialValue: report.summary)
        _log = State(initialValue: report.log ?? "")
    }

    private var text: String {
        AppReport.text(summary: summary, log: log)
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextEditor(text: $summary)
                        .font(.footnote.monospaced())
                        .frame(minHeight: 240)
                } header: {
                    Text("This goes into a public issue: remove anything personal.")
                }
                if report.log?.isEmpty == false {
                    Section("Log of the Last Session") {
                        TextEditor(text: $log)
                            .font(.caption2.monospaced())
                            .frame(minHeight: 320)
                    }
                }
            }
            .navigationTitle("Report a Problem")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .bottomBar) {
                    ShareLink("Share", item: text)
                }
                ToolbarItem(placement: .bottomBar) {
                    Button("Open GitHub") {
                        UIPasteboard.general.string = text
                        openURL(report.url)
                    }
                }
            }
        }
    }
}

extension AppReport.Ending {
    /// How the last session ended, as the library's notice says it.
    init(_ cause: LastSessionNotice.Cause) {
        switch cause {
        case .signal(let number): self = .signal(number, name: LastSessionNotice.signalName(number))
        case .failure(let reason): self = .startFailed(reason)
        case .endedByIOS: self = .endedByIOS
        case .exitStatus(let status): self = .exitStatus(status)
        }
    }
}

extension AppReport.Device {
    /// This iPhone: Minato's version, the model identifier (hw.machine, the simulated model's in the Simulator) and
    /// the iOS version.
    static var current: AppReport.Device {
        AppReport.Device(minato: MinatoVersion(info: Bundle.main.infoDictionary).text, model: modelIdentifier,
                         ios: UIDevice.current.systemVersion)
    }

    private static var modelIdentifier: String {
        if let simulated = ProcessInfo.processInfo.environment["SIMULATOR_MODEL_IDENTIFIER"] { return simulated }
        var size = 0
        guard sysctlbyname("hw.machine", nil, &size, nil, 0) == 0, size > 0 else { return "?" }
        var machine = [CChar](repeating: 0, count: size)
        guard sysctlbyname("hw.machine", &machine, &size, nil, 0) == 0 else { return "?" }
        return String(decoding: machine.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }, as: UTF8.self)
    }
}
