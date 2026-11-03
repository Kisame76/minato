import APKKit
import APKTestSupport
import Foundation
import Testing

@testable import APKLibrary

/// ADR-0070, the App's part 4: the app report's text and the issue form's URL from fixed entries, every prefilled
/// dropdown value one of the options .github/ISSUE_TEMPLATE/app-report.yml lists, and MinatoVersion's two forms.
@Suite("App report")
struct AppReportTests {
    typealias Sandbox = LibraryTests.Sandbox

    let device = AppReport.Device(minato: "1.0.0 (Beta 1)", model: "iPhone16,2", ios: "27.2")

    /// An app imported from one APK.
    private func entry(_ sandbox: Sandbox, label: String = "Sample Game") async throws -> LibraryEntry {
        let library = try Library(paths: sandbox.paths)
        _ = try await library.importFile(sandbox.file(SampleAPK.make(package: "org.example.game", versionCode: 7,
                                                                     label: label), named: "game.apk"))
        return try #require(await library.entries.first)
    }

    /// The dropdown's options in the issue form, by the field's id.
    private func options(of id: String) throws -> [String] {
        let root = URL(filePath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        let form = try String(contentsOf: root.appending(path: ".github/ISSUE_TEMPLATE/app-report.yml"),
                              encoding: .utf8)
        let lines = form.split(separator: "\n", omittingEmptySubsequences: false).map(String.init)
        let start = try #require(lines.firstIndex { $0.trimmingCharacters(in: .whitespaces) == "id: \(id)" })
        var options: [String] = []
        var inOptions = false
        for line in lines[(start + 1)...] {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if trimmed.hasPrefix("- type:") { break }
            if trimmed == "options:" { inOptions = true; continue }
            if inOptions, trimmed.hasPrefix("- ") { options.append(String(trimmed.dropFirst(2))) }
            else if inOptions { inOptions = false }
        }
        return options
    }

    private func fields(_ url: URL) throws -> [String: String] {
        let components = try #require(URLComponents(url: url, resolvingAgainstBaseURL: false))
        return Dictionary((components.queryItems ?? []).map { ($0.name, $0.value ?? "") }) { first, _ in first }
    }

    @Test func minatoVersionHasABetaFormAndADevelopersForm() {
        #expect(MinatoVersion(short: "1.0.0", build: "7", label: "Beta 1").text == "1.0.0 (Beta 1)")
        #expect(MinatoVersion(short: "1.0.0", build: "7", label: "").text == "1.0.0 (build 7)")
        #expect(MinatoVersion(short: "1.0.0", build: "7", label: nil).text == "1.0.0 (build 7)")
        let info: [String: Any] = ["CFBundleShortVersionString": "1.2.0", "CFBundleVersion": "12",
                                   "MinatoReleaseLabel": "Beta 3"]
        #expect(MinatoVersion(info: info).text == "1.2.0 (Beta 3)")
        #expect(MinatoVersion(info: nil).text == "? (build ?)")
    }

    @Test func everyPrefilledValueIsAnOptionOfTheForm() throws {
        let what = try options(of: "what")
        let format = try options(of: "format")
        #expect(!what.isEmpty && !format.isEmpty)
        for value in AppReport.What.allCases { #expect(what.contains(value.rawValue), "\(value.rawValue)") }
        for value in AppReport.Format.allCases { #expect(format.contains(value.rawValue), "\(value.rawValue)") }
    }

    @Test func theTextHoldsTheAppTheIPhoneTheStatusTheEndAndTheLog() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let entry = try await entry(sandbox)
        let report = AppReport(entry: entry, device: device, status: "Ready, without libfoo.so",
                               issues: ["libfoo.so will not load, so the app runs without it:\nno symbol bar"],
                               ending: .signal(11, name: "SIGSEGV"), log: "I/tag: one\nF/libc: Fatal signal 11\n")
        let text = report.text
        for part in ["Minato: 1.0.0 (Beta 1)", "iPhone: iPhone16,2, iOS 27.2", "App: Sample Game",
                     "Package: org.example.game", "Version: 7.0 (7)", "SDK: min ", "target ", "ABIs: arm64-v8a",
                     "Runtime: ", "Format: .apk", "Status: Ready, without libfoo.so",
                     "- libfoo.so will not load, so the app runs without it:\n  no symbol bar",
                     "Last session: it ended with signal 11 (SIGSEGV)",
                     "Log of the last session\nI/tag: one\nF/libc: Fatal signal 11\n"] {
            #expect(text.contains(part), "\(part)")
        }
        // Nothing of the Mac's or the iPhone's folders.
        #expect(!text.contains(sandbox.root.path) && !text.contains("/Users/") && !text.contains("/var/"))
    }

    @Test func withoutALogOrAnEndTheirPartsAreLeftOut() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let report = AppReport(entry: try await entry(sandbox), device: device, status: "Ready", issues: [],
                               ending: nil, log: nil)
        #expect(!report.text.contains("Last session") && !report.text.contains("Log of the last session"))
        #expect(!report.text.contains("Issues"))
    }

    @Test func theURLOpensTheFormWithItsFieldsByTheirIds() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let report = AppReport(entry: try await entry(sandbox), device: device, status: "Ready", issues: [],
                               ending: .startFailed("libmain.so is missing"), log: "I/tag: secret\n")
        let url = report.url
        #expect(url.absoluteString.hasPrefix("https://github.com/Kisame76/minato/issues/new?"))
        let fields = try fields(url)
        #expect(fields["template"] == "app-report.yml")
        #expect(fields["app"] == "Sample Game")
        #expect(fields["package"] == "org.example.game")
        #expect(fields["version"] == "7.0")
        #expect(fields["minato"] == "1.0.0 (Beta 1)")
        #expect(fields["device"] == "iPhone16,2")
        #expect(fields["ios"] == "27.2")
        #expect(fields["what"] == "It does not start")
        #expect(fields["format"] == ".apk")
        #expect(fields["details"]?.contains("libmain.so is missing") == true)
        // The source is the user's to choose, and the report goes by the clipboard.
        #expect(fields["source"] == nil && fields["report"] == nil)
        #expect(!url.absoluteString.contains("secret"))
    }

    @Test func fieldsArePercentEncoded() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let label = "Tom & Jerry+ 100% #1 = fun?"
        let report = AppReport(entry: try await entry(sandbox, label: label), device: device, status: "Ready",
                               issues: [], ending: nil, log: nil)
        let query = try #require(report.url.query(percentEncoded: true))
        #expect(!query.contains(" ") && !query.contains("+") && !query.contains("#") && !query.contains("?"))
        #expect(query.contains("app=Tom%20%26%20Jerry%2B%20100%25%20%231%20%3D%20fun%3F"))
        #expect(try fields(report.url)["app"] == label)
    }

    @Test func whatHappensFollowsTheEntryAndTheEnd() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        var entry = try await entry(sandbox)
        func what(_ ending: AppReport.Ending?) -> AppReport.What? {
            AppReport(entry: entry, device: device, status: "", issues: [], ending: ending, log: nil).what
        }
        #expect(what(nil) == nil)
        #expect(what(.signal(6, name: "SIGABRT")) == .crashes)
        #expect(what(.startFailed("no")) == .doesNotStart)
        #expect(what(.exitStatus(1)) == nil)
        #expect(what(.endedByIOS) == nil)
        entry.state = .incompatible
        #expect(what(nil) == .incompatible)
        entry.state = .invalid("not a ZIP file")
        #expect(what(nil) == .incompatible)
    }

    @Test func theFormatFollowsHowTheAppWasPicked() async throws {
        let sandbox = try Sandbox()
        defer { sandbox.remove() }
        let one = try await entry(sandbox)
        func format(_ change: (inout LibraryEntry) -> Void) -> AppReport.Format? {
            var entry = one
            change(&entry)
            return AppReport(entry: entry, device: device, status: "", issues: [], ending: nil, log: nil).format
        }
        let split = PackageFile(fileName: "split_config.arm64_v8a.apk", splitName: "config.arm64_v8a", size: 1,
                                sha256: "00", kind: .config)
        let obb = PackageFile(fileName: "main.7.org.example.game.obb", splitName: nil, size: 1, sha256: "00")
        #expect(format { _ in } == .apk)
        #expect(format { $0.splits = [split] } == .splitAPKs)
        #expect(format { $0.obbs = [obb] } == .withOBBs)
        for (name, kind) in [("game.apks", AppReport.Format.apks), ("game.XAPK", .xapk), ("game.apkm", .apkm),
                             ("game.aab", .aab)] {
            #expect(format { $0.source.fileName = name; $0.splits = [split]; $0.obbs = [obb] } == kind)
        }
        let notAnAPK = format {
            $0.state = .invalid("not a ZIP file")
            $0.base = nil
            $0.source.fileName = "notes.txt"
        }
        #expect(notAnAPK == nil)
    }
}
