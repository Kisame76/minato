import Testing

@testable import APKSession

/// ADR-0074 decisions 2 and 3: APKRT_REQUEST_NOTIFICATION's text as the host reads it, and a posted notification as
/// UNNotificationContent takes it.
@Suite struct NotificationRequestTests {
    private let post = "post\t7\tchat\tmessages\t4\t1\t1800000000000\tfriends\tAnna\tGroup chat"
        + "\tintent:#Intent;component=com.example/.Chat;end\tHi!\nSee you"

    @Test func aPostCarriesItsFieldsInOrder() throws {
        guard case .post(let note) = try #require(NotificationRequest(text: post)) else {
            Issue.record("not a post")
            return
        }
        #expect(note.id == 7 && note.tag == "chat" && note.channel == "messages" && note.importance == 4)
        #expect(note.sound && note.when == 1_800_000_000_000 && note.group == "friends")
        #expect(note.title == "Anna" && note.subText == "Group chat" && note.text == "Hi!\nSee you")
        #expect(note.intent == "intent:#Intent;component=com.example/.Chat;end")
    }

    @Test func aCancelAndACancelOfAll() {
        #expect(NotificationRequest(text: "cancel\t7\tchat") == .cancel(id: 7, tag: "chat"))
        #expect(NotificationRequest(text: "cancel\t-3\t") == .cancel(id: -3, tag: ""))
        #expect(NotificationRequest(text: "cancelAll") == .cancelAll)
    }

    @Test func aTextTheHostCannotReadIsNoRequest() {
        #expect(NotificationRequest(text: "post\t7") == nil)
        #expect(NotificationRequest(text: "post\tx\t\t\t3\t0\t0\t\t\t\t\t") == nil)
        #expect(NotificationRequest(text: "post\t7\t\t\t9\t0\t0\t\t\t\t\t") == nil)
        #expect(NotificationRequest(text: "post\t7\t\t\t3\t0\t0\t\t\t\t") == nil)  // the intent field missing
        #expect(NotificationRequest(text: "cancel\t7") == nil)
        #expect(NotificationRequest(text: "shout") == nil)
    }

    @Test func theIdentifierIsTheAppsTagAndID() throws {
        guard case .post(let note) = try #require(NotificationRequest(text: post)) else { return }
        #expect(note.identifier(package: "com.example") == "com.example|chat|7")
        #expect(NotificationPost.identifier(package: "com.example", tag: "", id: 3) == "com.example||3")
        #expect(NotificationPost.prefix(package: "com.example") == "com.example|")
    }

    @Test func theThreadIsTheGroupOrElseTheChannel() throws {
        guard case .post(var note) = try #require(NotificationRequest(text: post)) else { return }
        #expect(note.thread(package: "com.example") == "com.example|friends")
        note.group = ""
        #expect(note.thread(package: "com.example") == "com.example|messages")
    }

    @Test func theImportanceChoosesHowItShowsInFront() {
        #expect(NotificationPost.presentation(importance: 5) == [.banner, .list, .sound])
        #expect(NotificationPost.presentation(importance: 4) == [.banner, .list, .sound])
        #expect(NotificationPost.presentation(importance: 3) == [.list, .sound])
        #expect(NotificationPost.presentation(importance: 2) == [.list])
        #expect(NotificationPost.presentation(importance: 1) == [.list])
        #expect(NotificationPost.presentation(importance: 0) == [])
    }

    @Test func lowImportanceIsPassiveAndOnlyDefaultAndAboveSound() throws {
        guard case .post(var note) = try #require(NotificationRequest(text: post)) else { return }
        #expect(!note.passive && note.plays)
        note.importance = 2
        #expect(note.passive && !note.plays)
        note.importance = 3
        note.sound = false
        #expect(!note.passive && !note.plays)
    }

    @Test func importanceNoneIsNotShown() throws {
        guard case .post(var note) = try #require(NotificationRequest(text: post)) else { return }
        note.importance = 0
        #expect(!note.shows)
        note.importance = 1
        #expect(note.shows)
    }

    /// java-start-check's lines (the Debug host, only while the check's folder is there): iOS's delivered list and a
    /// presentation's options, for a test app's own notifications only; any other app gives no line.
    @Test func theCheckLinesNameATestAppsOwnNotificationsOnly() {
        let package = "io.github.kisame76.minato.test.notifygranted"
        let delivered = [("\(package)|null|8", "Second"), ("other.app|chat|1", "Private"),
                         ("\(package)|chat|7", "First again"), ("\(package)x|chat|9", "Look-alike")]
        #expect(NotificationPost.checkLine(delivered: delivered, package: package)
            == "[chat|7 First again, null|8 Second]")
        #expect(NotificationPost.checkLine(delivered: [], package: package) == "[]")
        #expect(NotificationPost.checkLine(delivered: delivered, package: "com.example.chat") == nil)
        #expect(NotificationPost.checkLine(presented: "\(package)|chat|7", options: [.sound, .banner, .list],
                                           package: package) == "chat|7 banner list sound")
        #expect(NotificationPost.checkLine(presented: "\(package)|null|8", options: [.list], package: package)
            == "null|8 list")
        #expect(NotificationPost.checkLine(presented: "com.example.chat|a|1", options: [.list],
                                           package: "com.example.chat") == nil)
        #expect(NotificationPost.checkLine(presented: "other.app|a|1", options: [.list], package: package) == nil)
    }

    /// The Debug events of a post, a cancel and a tap name a test app's tag, but never another app's: a tag is the
    /// app's own text.
    @Test func theEventsKeyNamesATestAppsTagOnly() {
        #expect(NotificationPost.checkKey(tag: "chat", id: 7, package: "io.github.kisame76.minato.test.notify")
            == "chat|7")
        #expect(NotificationPost.checkKey(tag: "null", id: 8, package: "io.github.kisame76.minato.test.notify")
            == "null|8")
        #expect(NotificationPost.checkKey(tag: "order 4711 for Holiday", id: 3, package: "com.example.shop")
            == "<app>|3")
    }
}
