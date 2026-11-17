import Foundation
import Testing

@testable import APKSession

/// ADR-0030 V5's data step: a Debug build hands the MINATO_* entries of tmp/guest-environment to the guest, and
/// skips every other line that is not blank or a comment, naming it for the log.
@Suite("DebugGuestEnvironment")
struct DebugGuestEnvironmentTests {
    /// The file, then the entries it hands over and the lines it skips: MINATO_* entries (the value may hold "=");
    /// comments and blank lines skipped silently; only MINATO_* keys pass, so the file cannot change what the runtime
    /// sets; a line without a key or a value sign is skipped; Windows line ends; an empty file.
    @Test(arguments: [
        ("MINATO_VULKAN_BGRA8_FIRST=1\nMINATO_CHECK=a=b\n", ["MINATO_VULKAN_BGRA8_FIRST=1", "MINATO_CHECK=a=b"], []),
        ("# the swapchain's order\n\n   \n  MINATO_X=1  \n", ["MINATO_X=1"], []),
        ("PATH=/tmp\nBOOTCLASSPATH=x\nminato_x=1\nMINATO_Y=2\n", ["MINATO_Y=2"], ["PATH", "BOOTCLASSPATH", "minato_x"]),
        ("MINATO_FLAG\n=1\nMINATO_=1\nMINATO_A B=1\n", [], ["MINATO_FLAG", "=1", "MINATO_", "MINATO_A B"]),
        ("MINATO_A=1\r\nMINATO_B=2\r\n", ["MINATO_A=1", "MINATO_B=2"], []),
        ("", [], []),
    ] as [(String, [String], [String])])
    func theFileGivesItsMinatoEntries(file: String, entries: [String], skipped: [String]) {
        #expect(DebugGuestEnvironment.parse(file) == DebugGuestEnvironment.Parsed(entries: entries, skipped: skipped))
    }

    /// Minato's own switches (MINATO_AUDIO_MUTE, MINATO_MEM_COUNT) are read with the host process's getenv, not the
    /// guest's: the entries reach it as well, with their value after the first "=".
    @Test func entriesReachTheHostEnvironment() {
        unsetenv("MINATO_HOST_ENV_TEST")
        DebugGuestEnvironment.exportToHost(["MINATO_HOST_ENV_TEST=a=b"])
        #expect(getenv("MINATO_HOST_ENV_TEST").map { String(cString: $0) } == "a=b")
        unsetenv("MINATO_HOST_ENV_TEST")
    }
}
