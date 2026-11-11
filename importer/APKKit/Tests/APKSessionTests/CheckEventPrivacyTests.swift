@testable import APKLibrary
import Foundation
import Testing

@testable import APKSession

/// The Debug build's check events (tmp/session-check/events) name none of the user's apps: an event after an end
/// that went wrong says the cause only, and any other app's name, label, package or file leaves every line.
@Suite("CheckEventPrivacy")
struct CheckEventPrivacyTests {
    static func entry(package: String, name: String, file: String) -> LibraryEntry {
        LibraryEntry(id: package, displayName: name, state: .ready, report: nil, base: nil, splits: [], obbs: [],
                     source: SourceFile(fileName: file, size: 1, modifiedMilliseconds: 0), iconFile: nil,
                     importedAt: Date())
    }

    @Test func theEventAfterAnEndThatWentWrongSaysTheCauseOnly() {
        let causes: [(LastSessionNotice.Cause, String)] = [
            (.signal(11), "signal 11"), (.failure("libmain.so is missing"), "failure libmain.so is missing"),
            (.endedByIOS, "endedByIOS"), (.exitStatus(1), "status 1"),
        ]
        for (cause, text) in causes {
            let notice = LastSessionNotice(name: "Holiday Photos", cause: cause, package: "com.example.holiday")
            #expect(notice.checkEvent == text)
            #expect(!notice.checkEvent.contains("Holiday"))
            #expect(!notice.checkEvent.contains("com.example"))
        }
    }

    @Test func anotherAppsNamesLeaveEveryLine() {
        let names = CheckEventPrivacy.names(of: Self.entry(package: "com.example.holiday", name: "Holiday Photos",
                                                           file: "Holiday Photos 2.apk"))
        let lines = [
            ("tile Holiday Photos | Ready", "tile <app> | Ready"),
            ("process gameMode com.example.holiday 0.412", "process gameMode <app> 0.412"),
            ("alert “Holiday Photos” Quit Unexpectedly | ok", "alert “<app>” Quit Unexpectedly | ok"),
            ("import Holiday Photos 2.apk done", "import <app> done"),
            ("scene didActivate state active", "scene didActivate state active"),
        ]
        for (line, redacted) in lines {
            #expect(CheckEventPrivacy.redact(line, names: names) == redacted)
        }
    }

    @Test func minatosOwnTestAppsKeepTheirNames() {
        let entry = Self.entry(package: "io.github.kisame76.minato.test.notify", name: "Notify Test",
                               file: "notify-test.apk")
        #expect(CheckEventPrivacy.names(of: entry).isEmpty)
        #expect(CheckEventPrivacy.names(package: entry.id, others: ["Notify Test"]).isEmpty)
        #expect(CheckEventPrivacy.names(package: "com.example.holiday", others: ["Holiday Photos"])
            == ["com.example.holiday", "Holiday Photos"])
        #expect(CheckEventPrivacy.redact("tile Notify Test | Ready", names: CheckEventPrivacy.names(of: entry))
            == "tile Notify Test | Ready")
    }

    @Test func aOneLetterNameLeavesTheLineAlone() {
        let names = CheckEventPrivacy.names(of: Self.entry(package: "com.example.a", name: "A", file: "a.apk"))
        #expect(CheckEventPrivacy.redact("A tile", names: names) == "A tile")
        #expect(CheckEventPrivacy.redact("process gameMode com.example.a", names: names) == "process gameMode <app>")
    }
}
