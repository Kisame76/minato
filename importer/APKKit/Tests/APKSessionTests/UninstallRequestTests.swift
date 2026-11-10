import Testing

@testable import APKSession

/// ADR-0077: APKRT_REQUEST_UNINSTALL's text, "<target package>\n<asking package>". The host takes only the target from
/// it; the asking app is its own session's.
@Suite("Uninstall request")
struct UninstallRequestTests {
    @Test func theTargetIsTheFirstLine() {
        #expect(UninstallRequest.target(of: "io.github.kisame76.absent\nio.github.kisame76.minato.test.uninstall")
                == "io.github.kisame76.absent")
        #expect(UninstallRequest.target(of: "a.b\n") == "a.b")
        #expect(UninstallRequest.target(of: "a.b") == "a.b")
        #expect(UninstallRequest.target(of: "com.example_1.App2\nx") == "com.example_1.App2")
    }

    /// Only a package name as Android's PackageParser takes one names an app of the library; anything else, a file
    /// entry's id among it, is no app (answered as failed).
    @Test(arguments: ["", "\nio.github.a", "a", ".a.b", "a.b.", "a..b", "1a.b", "a.1b", "a.b/c", "../a.b", "a.b c",
                      "file:a.b", "a.b\u{0}", "a-b.c", "ä.b"])
    func otherTextNamesNoApp(text: String) {
        #expect(UninstallRequest.target(of: text) == nil)
    }
}
