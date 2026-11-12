import APKLibrary
import APKSession
import SwiftUI
import UIKit
import UniformTypeIdentifiers

/// Report a Problem (ADR-0070, the App's part 2): the report as editable text, and the line that it goes into a
/// public issue, so that the user reads and trims it first. By default it names the engine, the Android versions,
/// Minato's version and what went wrong, not the app (the user's rule); naming the app and adding the last
/// session's log are the user's own switches, and the text shows exactly what they add. "Open GitHub" copies it
/// (this device only, for 10 minutes) and opens the issue form with the fields it fills, never the app; "Share"
/// hands it to any app. Nothing leaves the iPhone without one of the two taps.
struct ReportSheet: View {
    @State private var report: AppReport
    @State private var summary: String
    @State private var log: String
    @Environment(\.dismiss) private var dismiss
    @Environment(\.openURL) private var openURL

    init(report: AppReport) {
        _report = State(initialValue: report)
        _summary = State(initialValue: report.summary)
        _log = State(initialValue: report.shownLog ?? "")
    }

    private var text: String {
        AppReport.text(summary: summary, log: report.includesLog ? log : nil)
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    Toggle("Include the App's Name", isOn: $report.namesApp)
                    if report.log?.isEmpty == false {
                        Toggle("Include the Log of the Last Session", isOn: $report.includesLog)
                    }
                } footer: {
                    Text((report.log?.isEmpty == false ? "Without them" : "Without it")
                        + ", the report names the engine, the Android versions, Minato's version and what went "
                        + "wrong, not the app. Changing a switch writes the report again.")
                }
                Section {
                    TextEditor(text: $summary)
                        .font(.footnote.monospaced())
                        .frame(minHeight: 240)
                        .accessibilityLabel("Report")
                } header: {
                    Text("This goes into a public issue: remove anything personal.")
                }
                if report.includesLog {
                    Section("Log of the Last Session") {
                        TextEditor(text: $log)
                            .font(.caption2.monospaced())
                            .frame(minHeight: 320)
                            .accessibilityLabel("Log of the Last Session")
                    }
                }
            }
            .onChange(of: report.namesApp) {
                summary = report.summary
                log = report.shownLog ?? ""
            }
            .onChange(of: report.includesLog) { log = report.shownLog ?? "" }
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
                        // This device's pasteboard only (no Universal Clipboard), cleared after 10 minutes.
                        UIPasteboard.general.setItems([[UTType.plainText.identifier: text]],
                                                      options: [.localOnly: true,
                                                                .expirationDate: Date.now.addingTimeInterval(600)])
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
    /// the iOS version. UIDevice is the main actor's.
    @MainActor static var current: AppReport.Device {
        AppReport.Device(minato: MinatoVersion(info: Bundle.main.infoDictionary).text, model: modelIdentifier,
                         ios: UIDevice.current.systemVersion)
    }

    private static var modelIdentifier: String {
        let model = GuestInsets.Housing.model(environment: ProcessInfo.processInfo.environment, machine: machine)
        return model.isEmpty ? "?" : model
    }

    /// sysctl's hw.machine, "iPhone16,2"; the Mac's in the Simulator.
    static var machine: String? {
        var size = 0
        guard sysctlbyname("hw.machine", nil, &size, nil, 0) == 0, size > 0 else { return nil }
        var machine = [CChar](repeating: 0, count: size)
        guard sysctlbyname("hw.machine", &machine, &size, nil, 0) == 0 else { return nil }
        return String(decoding: machine.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }, as: UTF8.self)
    }
}
