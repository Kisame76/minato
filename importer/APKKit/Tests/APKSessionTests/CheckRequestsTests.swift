import Foundation
import Testing

@testable import APKSession

/// The Debug check's requests without the phone's file service: the whole run's at launch in MINATO_CHECK_REQUESTS
/// (`devicectl device process launch --environment-variables`), the mid-session ones as minato://check/<name>?<value>
/// (`devicectl device process launch --payload-url`, `simctl openurl`). Both become the files tmp/session-check/ holds
/// when the script copies them there, so the app reads them as it always did.
@Suite struct CheckRequestsTests {
    @Test func theLaunchVariableHoldsOneRequestALine() {
        let value = "tap=io.github.kisame76.minato.test.java\nguest-log=yes\nno-compile\nfps=hide\n\ninstall-deny=\n"
        let requests = CheckRequests.launch(environment: [CheckRequests.variable: value])
        #expect(requests == [
            CheckRequests.Request(name: "tap", value: "io.github.kisame76.minato.test.java"),
            CheckRequests.Request(name: "guest-log", value: "yes"),
            CheckRequests.Request(name: "no-compile", value: "yes"),
            CheckRequests.Request(name: "fps", value: "hide"),
            CheckRequests.Request(name: "install-deny", value: ""),
        ])
    }

    @Test func aValueKeepsItsSpacesAndItsEqualsSigns() {
        let requests = CheckRequests.launch(environment: [
            CheckRequests.variable: "notification-repost=io.github.kisame76.minato.test.notify 10\nquit=a=b",
        ])
        #expect(requests.map(\.value) == ["io.github.kisame76.minato.test.notify 10", "a=b"])
    }

    @Test func aValueOfSeveralLinesComesPercentEncoded() {
        let requests = CheckRequests.launch(environment: [
            CheckRequests.variable:
                "end=io.github.kisame76.minato.test.java%0Aio.github.kisame76.minato.test.context\nfps=100%25",
        ])
        #expect(requests.map(\.value)
                == ["io.github.kisame76.minato.test.java\nio.github.kisame76.minato.test.context", "100%"])
    }

    @Test func anEmptyURLValueIsAnEmptyFile() throws {
        let url = try #require(URL(string: "minato://check/install-deny?"))
        #expect(CheckRequests.request(from: url) == CheckRequests.Request(name: "install-deny", value: ""))
    }

    @Test func withoutTheVariableThereIsNoRequest() {
        #expect(CheckRequests.launch(environment: [:]).isEmpty)
        #expect(CheckRequests.launch(environment: [CheckRequests.variable: ""]).isEmpty)
    }

    @Test func aNameThatIsNoPlainFileNameIsDropped() {
        let value = "../events=x\nsub/dir=x\n.hidden=x\nUpper=x\n=x\n \(String(repeating: "a", count: 65))=x\nmenu=yes"
        #expect(CheckRequests.launch(environment: [CheckRequests.variable: value])
                == [CheckRequests.Request(name: "menu", value: "yes")])
    }

    @Test func aCheckURLIsOneRequest() throws {
        let tap = try #require(URL(string: "minato://check/tap?io.github.kisame76.minato.test.java"))
        #expect(CheckRequests.request(from: tap)
                == CheckRequests.Request(name: "tap", value: "io.github.kisame76.minato.test.java"))
        let menu = try #require(URL(string: "minato://check/menu"))
        #expect(CheckRequests.request(from: menu) == CheckRequests.Request(name: "menu", value: "yes"))
        let repost = try #require(
            URL(string: "minato://check/notification-repost?io.github.kisame76.minato.test.notify%2010"))
        #expect(CheckRequests.request(from: repost)?.value == "io.github.kisame76.minato.test.notify 10")
    }

    @Test func otherURLsAreNoCheckRequest() throws {
        for text in ["minato://open", "minato://check", "minato://check/", "minato://check/../events?x",
                     "minato://check/a/b", "file:///check/menu", "https://check/menu", "minato://other/menu"] {
            let url = try #require(URL(string: text))
            #expect(CheckRequests.request(from: url) == nil, "\(text)")
        }
        #expect(CheckRequests.isCheck(try #require(URL(string: "minato://check/../events"))))
        #expect(!CheckRequests.isCheck(try #require(URL(string: "minato://open"))))
    }

    @Test func requestsBecomeTheFilesTheScriptWouldHaveCopied() throws {
        let folder = FileManager.default.temporaryDirectory
            .appending(path: "check-requests-\(UUID().uuidString)/session-check", directoryHint: .isDirectory)
        defer { try? FileManager.default.removeItem(at: folder.deletingLastPathComponent()) }
        try CheckRequests.write([CheckRequests.Request(name: "tap", value: "io.github.kisame76.minato.test.java"),
                                 CheckRequests.Request(name: "events", value: "")], into: folder)
        #expect(try String(contentsOf: folder.appending(path: "tap"), encoding: .utf8)
                == "io.github.kisame76.minato.test.java\n")
        #expect(try Data(contentsOf: folder.appending(path: "events")).isEmpty)
        // A later request replaces the file, as the script's copy does.
        try CheckRequests.write([CheckRequests.Request(name: "tap", value: "io.github.kisame76.minato.test.context")],
                                into: folder)
        #expect(try String(contentsOf: folder.appending(path: "tap"), encoding: .utf8)
                == "io.github.kisame76.minato.test.context\n")
    }
}
