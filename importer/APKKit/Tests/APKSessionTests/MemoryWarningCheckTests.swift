import Testing

@testable import APKSession

/// The Debug request "warning" (SessionCheck) posts iOS's memory warning as many times as its file says, through the
/// path iOS's own takes, and the event line says what the bridge did with each (its onLowMemory rate limit, line 580).
@Suite("MemoryWarningCheck")
struct MemoryWarningCheckTests {
    @Test func theRequestSaysHowManyWarnings() {
        #expect(MemoryWarningCheck.count(of: nil) == 1)
        #expect(MemoryWarningCheck.count(of: "") == 1)
        #expect(MemoryWarningCheck.count(of: "2") == 2)
        #expect(MemoryWarningCheck.count(of: " 3\n") == 3)
        #expect(MemoryWarningCheck.count(of: "two") == 1)
        #expect(MemoryWarningCheck.count(of: "0") == 1)
        #expect(MemoryWarningCheck.count(of: "99") == MemoryWarningCheck.most)
    }

    @Test func theEventSaysWhatTheBridgeDid() {
        #expect(MemoryWarningCheck.note(passedOn: true, since: nil) == "passed on")
        #expect(MemoryWarningCheck.note(passedOn: true, since: .seconds(61)) == "passed on")
        #expect(MemoryWarningCheck.note(passedOn: false, since: .milliseconds(12_400))
            == "held back, 12 s since the last")
        #expect(MemoryWarningCheck.note(passedOn: false, since: nil) == "held back, not started or waiting")
    }
}
