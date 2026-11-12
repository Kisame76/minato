import Foundation
import Testing

@testable import APKSession

/// ADR-0030 V5's data step: a Debug build hands the MINATO_* entries of tmp/guest-environment to the guest, and
/// skips every other line that is not blank or a comment, naming it for the log.
@Suite("DebugGuestEnvironment")
struct DebugGuestEnvironmentTests {
    @Test func takesMinatoEntries() {
        let parsed = DebugGuestEnvironment.parse("MINATO_VULKAN_BGRA8_FIRST=1\nMINATO_CHECK=a=b\n")
        #expect(parsed.entries == ["MINATO_VULKAN_BGRA8_FIRST=1", "MINATO_CHECK=a=b"])
        #expect(parsed.skipped == [])
    }

    @Test func skipsCommentsAndBlankLines() {
        let parsed = DebugGuestEnvironment.parse("# the swapchain's order\n\n   \n  MINATO_X=1  \n")
        #expect(parsed.entries == ["MINATO_X=1"])
        #expect(parsed.skipped == [])
    }

    /// Only MINATO_* keys pass, so the file cannot change what the runtime sets.
    @Test func skipsOtherKeysByName() {
        let parsed = DebugGuestEnvironment.parse("PATH=/tmp\nBOOTCLASSPATH=x\nminato_x=1\nMINATO_Y=2\n")
        #expect(parsed.entries == ["MINATO_Y=2"])
        #expect(parsed.skipped == ["PATH", "BOOTCLASSPATH", "minato_x"])
    }

    @Test func skipsLinesWithoutAKeyOrValueSign() {
        let parsed = DebugGuestEnvironment.parse("MINATO_FLAG\n=1\nMINATO_=1\nMINATO_A B=1\n")
        #expect(parsed.entries == [])
        #expect(parsed.skipped == ["MINATO_FLAG", "=1", "MINATO_", "MINATO_A B"])
    }

    @Test func takesWindowsLineEnds() {
        let parsed = DebugGuestEnvironment.parse("MINATO_A=1\r\nMINATO_B=2\r\n")
        #expect(parsed.entries == ["MINATO_A=1", "MINATO_B=2"])
    }

    /// Minato's own switches (MINATO_AUDIO_MUTE, MINATO_MEM_COUNT) are read with the host process's getenv, not the
    /// guest's: the entries reach it as well, with their value after the first "=".
    @Test func entriesReachTheHostEnvironment() {
        unsetenv("MINATO_HOST_ENV_TEST")
        DebugGuestEnvironment.exportToHost(["MINATO_HOST_ENV_TEST=a=b"])
        #expect(getenv("MINATO_HOST_ENV_TEST").map { String(cString: $0) } == "a=b")
        unsetenv("MINATO_HOST_ENV_TEST")
    }

    @Test func anEmptyFileGivesNothing() {
        #expect(DebugGuestEnvironment.parse("") == DebugGuestEnvironment.Parsed(entries: [], skipped: []))
    }
}
