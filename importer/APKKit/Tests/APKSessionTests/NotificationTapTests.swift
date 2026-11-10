import Testing

@testable import APKSession

/// ADR-0074 decision 4, the creator's identity: a cold tap starts from the stored Intent.toUri only an explicit
/// activity of the notification's own package; anything else opens the app at its launcher.
@Suite struct NotificationTapTests {
    private let package = "com.example.app"
    private let activities: Set<String> = ["com.example.app.MainActivity", "com.example.app.ChatActivity"]

    private func start(_ uri: String) -> String? {
        NotificationTap.startIntent(uri, package: package, activities: activities)
    }

    @Test func anExplicitActivityOfTheAppStarts() {
        let uri = "intent:#Intent;component=com.example.app/.ChatActivity;S.room=7;end"
        #expect(start(uri) == uri)
        let full = "intent:#Intent;action=android.intent.action.VIEW;"
            + "component=com.example.app/com.example.app.MainActivity;end"
        #expect(start(full) == full)
        let data = "intent://chat/7#Intent;scheme=app;package=com.example.app;"
            + "component=com.example.app/.ChatActivity;end"
        #expect(start(data) == data)
    }

    @Test func anotherPackagesComponentOpensTheLauncher() {
        #expect(start("intent:#Intent;component=com.other/.ChatActivity;end") == nil)
        #expect(start("intent:#Intent;component=com.example.app.evil/.ChatActivity;end") == nil)
        #expect(start("intent:#Intent;package=com.other;component=com.example.app/.ChatActivity;end") == nil)
    }

    @Test func anImplicitIntentOpensTheLauncher() {
        #expect(start("intent:#Intent;action=android.intent.action.VIEW;end") == nil)
        #expect(start("intent://chat/7#Intent;scheme=app;package=com.example.app;end") == nil)
    }

    @Test func aComponentThatIsNoActivityOfTheAppOpensTheLauncher() {
        #expect(start("intent:#Intent;component=com.example.app/.SyncService;end") == nil)
        #expect(start("intent:#Intent;component=com.example.app/;end") == nil)
        #expect(start("intent:#Intent;component=com.example.app;end") == nil)
    }

    @Test func aSelectorOpensTheLauncher() {
        let uri = "intent:#Intent;component=com.example.app/.ChatActivity;SEL;component=com.other/.X;end"
        #expect(start(uri) == nil)
        let selected = "intent:#Intent;component=com.example.app/.ChatActivity;SEL;"
            + "action=android.intent.action.VIEW;end"
        #expect(start(selected) == nil)
    }

    @Test func whatIsNoIntentURIOpensTheLauncher() {
        #expect(start("") == nil)
        #expect(start("https://example.com/#Intent;component=com.example.app/.ChatActivity;end") == nil)
        #expect(start("intent:#Intent;component=com.example.app/.ChatActivity") == nil)                    // no end
        #expect(start("intent:#Intent;component=com.example.app/.ChatActivity;end;tail") == nil)
        #expect(start("intent:#Intent;component=com.example.app/.ChatActivity;component=com.other/.X;end") == nil)
        #expect(start("intent:#Intent;component=com.example.app/.Chat\u{7}Activity;end") == nil)        // a control
        #expect(start("intent:#Intent;component=com.example.app/.ChatActivity;S.x=\u{e4};end") == nil)  // not ASCII
    }

    @Test func anOversizedURIOpensTheLauncher() {
        let padding = String(repeating: "x", count: 2048)
        #expect(start("intent:#Intent;component=com.example.app/.ChatActivity;S.p=\(padding);end") == nil)
    }

    @Test func theTapsTagIsTheFNV1aOfItsBytes() {
        #expect(NotificationTap.tagHash("") == 0xcbf2_9ce4_8422_2325)
        #expect(NotificationTap.tagHash("a") == 0xaf63_dc4c_8601_ec8c)
        #expect(NotificationTap.tagHash("chat") != NotificationTap.tagHash("chats"))
    }
}
