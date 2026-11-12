import Synchronization
import Testing

@testable import APKSession

/// The Debug check's events on the app's console (MINATO_CHECK_CONSOLE=1): the device check reads them from
/// `devicectl device process launch --console` instead of copying the events file through the phone's file service.
/// Each event goes to the console once, as the same line the events file gets, with a prefix the reader looks for;
/// without the variable nothing goes there.
@Suite struct CheckConsoleTests {
    final class Sink: Sendable {
        let lines = Mutex<[String]>([])
        func write(_ text: String) { lines.withLock { $0.append(text) } }
    }

    @Test func anEventGoesToTheConsoleOnceWithTheVariable() {
        let sink = Sink()
        let console = CheckConsole(environment: ["MINATO_CHECK_CONSOLE": "1"], sink: sink.write)
        let line = CheckConsole.line(time: 1_794_430_000.25, pid: 812, text: "tap io.github.kisame76.minato.test.java")
        #expect(line == "1794430000.250 812 tap io.github.kisame76.minato.test.java\n")
        console.event(line)
        #expect(sink.lines.withLock { $0 }
                == ["minato-check-event 1794430000.250 812 tap io.github.kisame76.minato.test.java\n"])
    }

    @Test func withoutTheVariableNothingGoesToTheConsole() {
        for environment in [[:], ["MINATO_CHECK_CONSOLE": "0"], ["MINATO_CHECK_CONSOLE": ""]] {
            let sink = Sink()
            let console = CheckConsole(environment: environment, sink: sink.write)
            console.event(CheckConsole.line(time: 1, pid: 1, text: "tap"))
            #expect(sink.lines.withLock { $0 }.isEmpty)
            #expect(!console.isOn)
        }
    }

    @Test func theConsoleGetsTheRedactedLineOnly() {
        let sink = Sink()
        let console = CheckConsole(environment: ["MINATO_CHECK_CONSOLE": "1"], sink: sink.write)
        let text = CheckEventPrivacy.redact("tap com.example.holiday", names: ["com.example.holiday"])
        console.event(CheckConsole.line(time: 2, pid: 3, text: text))
        let lines = sink.lines.withLock { $0 }
        #expect(lines == ["minato-check-event 2.000 3 tap <app>\n"])
        #expect(!lines.joined().contains("holiday"))
    }

    @Test func aLineWithANewlineInsideStaysOneConsoleLine() {
        let sink = Sink()
        let console = CheckConsole(environment: ["MINATO_CHECK_CONSOLE": "1"], sink: sink.write)
        console.event(CheckConsole.line(time: 2, pid: 3, text: "window dump a\nb"))
        #expect(sink.lines.withLock { $0 } == ["minato-check-event 2.000 3 window dump a b\n"])
    }

    /// The guest's log and stdout reach the console too, a line at a time behind their own prefixes, however the
    /// pipe cuts them; what is left at the end goes as one more line.
    @Test func theGuestsOutputGoesToTheConsoleALineAtATime() {
        let sink = Sink()
        let console = CheckConsole(environment: ["MINATO_CHECK_CONSOLE": "1"], sink: sink.write)
        var lines = CheckConsole.Lines(prefix: CheckConsole.guestLogPrefix)
        console.guest(&lines, Array("I/java-test: on".utf8))
        #expect(sink.lines.withLock { $0 }.isEmpty)
        console.guest(&lines, Array("Resume\nI/java-test: done\nW/x: ".utf8))
        console.guest(&lines, Array("half".utf8))
        console.end(&lines)
        #expect(sink.lines.withLock { $0 } == ["minato-guest-log I/java-test: onResume\n",
                                                "minato-guest-log I/java-test: done\n", "minato-guest-log W/x: half\n"])
        var out = CheckConsole.Lines(prefix: CheckConsole.guestOutPrefix)
        console.guest(&out, Array("app-start-test: MessageDigest ok\n".utf8))
        #expect(sink.lines.withLock { $0 }.last == "minato-guest-out app-start-test: MessageDigest ok\n")
    }

    @Test func withoutTheVariableTheGuestsOutputStaysOffTheConsole() {
        let sink = Sink()
        let console = CheckConsole(environment: [:], sink: sink.write)
        var lines = CheckConsole.Lines(prefix: CheckConsole.guestLogPrefix)
        console.guest(&lines, Array("I/java-test: done\n".utf8))
        console.end(&lines)
        #expect(sink.lines.withLock { $0 }.isEmpty)
    }
}
