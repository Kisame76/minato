import APKKit
import Foundation

/// A split set analysed over the APKs Minato installs (ADR-0048, decision 5).
public enum SetAnalysis {
    /// The base's report with the native code, label and icon of the base and its chosen splits, and the icon's bytes
    /// from whichever of them holds it. The set's verdict on missing splits replaces the base's, and the ABIs of the
    /// splits the choice skipped count, so a set whose ABI splits are 32-bit only says so. `archive` opens a member.
    public static func analyze(_ set: CheckedSet, choice: SplitChoice, preferredLanguages: [String],
                               archive: (SetMember) throws -> ZipArchive) throws -> (report: APKReport, icon: Data?) {
        guard let base = set.base else {
            let name = set.splits.first?.fileName ?? "?"
            throw SetRefusal(reason: "Split APK “\(name)” needs its base APK. Import it together with its base APK.")
        }
        let analysis = try APKAnalyzer.analyze(
            base: archive(base), splits: choice.chosen.map(archive), otherABIs: choice.skipped.flatMap(\.member.abis),
            needsSplits: set.needsSplits, preferredLanguages: preferredLanguages)
        return (analysis.report, try analysis.icon.flatMap { try APKAnalyzer.iconData(for: analysis.report, in: $0) })
    }
}
