import APKFormats
import APKKit
import Foundation

/// What `recheckAll` did.
public struct RecheckSummary: Sendable, Equatable {
    /// The entries checked again, in their order in the library.
    public var checked: [String]
    /// "<entry>: <reason>" for each entry that could not be checked again; it keeps its report and its stamp, so the
    /// next pass tries it again.
    public var failures: [String]

    public init(checked: [String], failures: [String]) {
        self.checked = checked
        self.failures = failures
    }
}

/// An entry's installed APKs could not be read back as they were installed.
struct RecheckUnreadable: Error, LocalizedError {
    var errorDescription: String? { "its installed APKs cannot be read" }
}

extension Library {
    /// The entries whose report another Minato made (ADR-0055 decision 5): its stamp differs from `checkStamp`, or it
    /// has none. Nothing while the app has set no stamp.
    public func entriesToRecheck() -> [String] {
        guard let checkStamp else { return [] }
        return entries.filter { $0.report != nil && $0.report?.checkedBy != checkStamp }.map(\.id)
    }

    /// Checks again, entry by entry, every entry another Minato checked (`entriesToRecheck`). `starting` hears of each
    /// one before it runs, to show it as checking; while it is awaited, the library serves other calls, such as an
    /// import. An entry that fails keeps its report and is reported; the others go on.
    public func recheckAll(starting: @Sendable (String) async -> Void = { _ in }) async -> RecheckSummary {
        var summary = RecheckSummary(checked: [], failures: [])
        for id in entriesToRecheck() {
            await starting(id)
            do {
                if try recheck(id: id) { summary.checked.append(id) }
            } catch {
                summary.failures.append("\(id): \(error.localizedDescription)")
            }
        }
        return summary
    }

    /// Checks the entry again as an import of its installed APKs would (ADR-0055 decision 5): the analyzer over the APK
    /// or the set, then, for a ready app, the converter's check of its libraries into a new lib/ of originals, which the
    /// next tap signs, as after a certificate change. The new report, with this Minato's stamp, replaces the old one,
    /// and the state follows it: Ready, Ready with degraded libraries, or Incompatible, which drops lib/ but keeps the
    /// APKs, unlike an incompatible import: a Minato that refuses wrongly must not cost the app, the next one checks it
    /// again. An entry without APKs (an incompatible import) only takes the stamp. Returns whether it was checked;
    /// throws when its APKs cannot be read, and then nothing changes.
    @discardableResult
    public func recheck(id: String) throws -> Bool {
        guard let stamp = checkStamp, let entry = entries.first(where: { $0.id == id }),
              let old = entry.report, old.checkedBy != stamp else { return false }
        guard entry.base != nil else {
            try record(id: id) { $0.report?.checkedBy = stamp }
            return false
        }
        let apks = installedAPKs(of: entry)
        guard !apks.isEmpty else { throw RecheckUnreadable() }
        let archives = Dictionary(uniqueKeysWithValues: apks.map { ($0.member.splitName, $0.archive) })
        var report: APKReport
        if entry.splits.isEmpty {
            report = try APKAnalyzer.analyze(apks[0].archive, preferredLanguages: preferredLanguages)
        } else {
            guard case .success(let set) = SetChecks.check(apks.map(\.member)) else { throw RecheckUnreadable() }
            let choice = SplitSelector.choose(set, for: device)
            report = try SetAnalysis.analyze(set, choice: choice, preferredLanguages: preferredLanguages) {
                archives[$0.splitName]!
            }.report
            if let reason = choice.incompatibility {
                report.compatibility.issues.append(.splitChoice(reason))
                report.compatibility.status = .incompatible
            }
        }
        report.checkedBy = stamp
        if report.compatibility.status == .ready {
            do {
                let installed = try installNativeCode(of: report, from: archives,
                                                      into: paths.versionDirectory(for: entry), signing: nil)
                report.compatibility.issues += installed.findings
                report.compatibility.checks = installed.checks
            } catch let refusal as RefusedLibrary {
                report.compatibility.status = .incompatible
                report.compatibility.issues += refusal.issues
            }
        }
        let checked = report
        try record(id: id) { entry in
            entry.report = checked
            entry.state = switch checked.compatibility.status {
            case .ready: .ready
            case .needsSplits: .needsSplits
            case .incompatible: .incompatible
            }
            entry.signedWith = nil
            entry.containerVersion = nil
        }
        if checked.compatibility.status == .incompatible {
            try? FileManager.default.removeItem(at: paths.libraryDirectory(for: entry))
        }
        return true
    }
}
