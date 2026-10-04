#if DEBUG
import APKLibrary
import Darwin
import Foundation
import UIKit

/// Answers tools/update-check.sh (ADR-0034, test 4) and tools/set-check.sh (ADR-0048 b7c) in the Simulator. A script
/// leaves its requests as files in tmp/update-check/; nothing happens without that folder. Each process appends what it
/// shows to tmp/update-check/events, one line per event: "<seconds since 1970> <pid> <event>".
///   answer  "replace" or "keep": the library answers the question that is up, and removes the file.
///   end     a package: the library removes that app, then the whole folder, which ends the check.
enum UpdateCheck {
    static let folder = URL.temporaryDirectory.appending(path: "update-check", directoryHint: .isDirectory)

    private static var isOn: Bool { FileManager.default.fileExists(atPath: folder.path) }

    private static func text(of name: String) -> String? {
        guard isOn, let text = try? String(contentsOf: folder.appending(path: name), encoding: .utf8) else { return nil }
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? nil : trimmed
    }

    static func event(_ text: String) {
        guard isOn else { return }
        let line = String(format: "%.3f", Date.now.timeIntervalSince1970) + " \(getpid()) \(text)\n"
        let file = folder.appending(path: "events")
        if let handle = try? FileHandle(forWritingTo: file) {
            defer { try? handle.close() }
            _ = try? handle.seekToEnd()
            try? handle.write(contentsOf: Data(line.utf8))
        } else {
            try? Data(line.utf8).write(to: file)
        }
    }

    /// The alert that is up: "alert <title> | <message> | <buttons>", once it is on the screen.
    @MainActor
    @discardableResult
    static func alertShown() async -> Bool {
        guard isOn else { return false }
        for _ in 0..<50 {
            try? await Task.sleep(for: .seconds(0.1))
            if let alert = SessionCheck.topmost() as? UIAlertController, alert.preferredStyle == .alert {
                let buttons = alert.actions.compactMap(\.title).joined(separator: " / ")
                event("alert \(alert.title ?? "") | \(alert.message ?? "") | \(buttons)")
                return true
            }
        }
        event("alert not shown, on top: \(SessionCheck.topmost().map { String(describing: type(of: $0)) } ?? "nothing")")
        return false
    }

    @MainActor
    static func recordAlert() {
        Task { await alertShown() }
    }

    /// The tile of each result's entry once the import is done, as the library shows it: "tile <name> | <status line>".
    static func tiles(of results: [ImportResult], in entries: [LibraryEntry]) {
        guard isOn else { return }
        for result in results {
            guard let entry = entries.first(where: { $0.id == result.entry.id }) else { continue }
            event("tile \(entry.displayName) | \(Texts.statusLine(Texts.status(of: entry)))")
        }
    }

    /// The question is up: records it, then answers it when the script asks for that.
    @MainActor
    static func questionShown(_ question: UpdateQuestion, in library: LibraryModel) {
        guard isOn else { return }
        Task { @MainActor in
            guard await alertShown(), let answer = text(of: "answer") else { return }
            try? FileManager.default.removeItem(at: folder.appending(path: "answer"))
            try? await Task.sleep(for: .seconds(0.5))
            await library.answer(question, replace: answer == "replace")
        }
    }

    /// The check is over: the library removes the requested app, then the folder.
    @MainActor
    static func endIfRequested(in library: LibraryModel) async {
        guard let package = text(of: "end") else { return }
        if let entry = library.entries.first(where: { $0.id == package }) {
            await library.remove([entry])
        }
        try? FileManager.default.removeItem(at: folder)
    }
}
#endif
