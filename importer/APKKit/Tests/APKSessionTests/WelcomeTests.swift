import Foundation
import Testing

@testable import APKSession

/// The welcome sheet: once at Minato's first launch, and once after an update for whoever never saw it.
struct WelcomeTests {
    /// UserDefaults of their own, as a new install's.
    private func defaults() -> UserDefaults {
        let name = "welcome-\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: name)!
        defaults.removePersistentDomain(forName: name)
        return defaults
    }

    @Test func theFirstLaunchShowsItAndAfterItNothingDoes() {
        let defaults = defaults()
        #expect(Welcome(defaults: defaults).needsShowing)
        Welcome(defaults: defaults).shown()
        #expect(!Welcome(defaults: defaults).needsShowing)
        // The next launch, and an update's, read the same defaults.
        #expect(!Welcome(defaults: defaults).needsShowing)
    }

    @Test func anUpdateShowsItToWhoeverNeverSawIt() {
        // An earlier build left its own preferences, but no welcome.
        let defaults = defaults()
        defaults.set("library", forKey: "selectedTab")
        defaults.set(true, forKey: "showsFPS")
        #expect(Welcome(defaults: defaults).needsShowing)
    }
}
