import Foundation

/// Minato's version as About and the app report show it (ADR-0070, part 1): "1.0.0 (Beta 1)" for a
/// release, whose label tools/release.sh sets, and "1.0.0 (build 7)" without a label, a developer's build.
public struct MinatoVersion: Sendable, Equatable {
    public let short: String
    public let build: String
    public let label: String?

    public init(short: String, build: String, label: String?) {
        self.short = short
        self.build = build
        self.label = label
    }

    /// From the app's Info.plist: CFBundleShortVersionString, CFBundleVersion and MinatoReleaseLabel.
    public init(info: [String: Any]?) {
        self.init(short: info?["CFBundleShortVersionString"] as? String ?? "?",
                  build: info?["CFBundleVersion"] as? String ?? "?",
                  label: info?["MinatoReleaseLabel"] as? String)
    }

    public var text: String {
        if let label, !label.isEmpty { "\(short) (\(label))" } else { "\(short) (build \(build))" }
    }
}
