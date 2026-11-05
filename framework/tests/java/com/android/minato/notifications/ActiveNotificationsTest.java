package com.android.minato.notifications;

import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;

/**
 * ADR-0074 decision 2: the app's active notifications as Android 15 lists them
 * (fixtures/expected/notify-test-granted.logcat): a post replaces one of the same tag and id, a second one without a
 * group of the app's own brings the system's group summary ("ranker_group", Integer.MAX_VALUE), which stays while one
 * of them is left, and a cancel of all takes everything.
 */
public final class ActiveNotificationsTest {
    private static List<String> keys(ActiveNotifications<String> active) {
        List<String> keys = new ArrayList<>();
        for (ActiveNotifications.Entry<String> entry : active.entries()) {
            keys.add(entry.tag + "|" + entry.id + " " + entry.value);
        }
        java.util.Collections.sort(keys);
        return keys;
    }

    public void testNotifyTestGrantedsSequence() {
        ActiveNotifications<String> active = new ActiveNotifications<>();
        active.post("chat", 7, null, "First");
        check(keys(active).equals(List.of("chat|7 First")), "first " + keys(active));
        active.post("chat", 7, null, "First again");
        check(keys(active).equals(List.of("chat|7 First again")), "replaced " + keys(active));
        active.post(null, 8, null, "Second");
        check(keys(active).equals(List.of("chat|7 First again", "null|8 Second", "ranker_group|2147483647 null")),
                "the summary comes " + keys(active));
        active.cancel("chat", 7);
        check(keys(active).equals(List.of("null|8 Second", "ranker_group|2147483647 null")),
                "the summary stays " + keys(active));
        active.cancelAll();
        check(keys(active).isEmpty(), "all gone " + keys(active));
    }

    public void testTheSummaryGoesWithTheLastUngroupedOne() {
        ActiveNotifications<String> active = new ActiveNotifications<>();
        active.post(null, 1, null, "a");
        active.post(null, 2, null, "b");
        active.cancel(null, 1);
        active.cancel(null, 2);
        check(keys(active).isEmpty(), "no summary alone " + keys(active));
    }

    public void testNotificationsOfTheAppsOwnGroupGetNoSystemSummary() {
        ActiveNotifications<String> active = new ActiveNotifications<>();
        active.post(null, 1, "mine", "a");
        active.post(null, 2, "mine", "b");
        check(keys(active).equals(List.of("null|1 a", "null|2 b")), "own group " + keys(active));
    }

    public void testATapFindsTheNotificationByItsIdAndTagHash() {
        ActiveNotifications<String> active = new ActiveNotifications<>();
        active.post("chat", 7, null, "a");
        active.post(null, 7, null, "b");
        active.post(null, 8, null, "c");
        check("a".equals(active.find(7, ActiveNotifications.tagHash("chat"))), "chat 7");
        check("b".equals(active.find(7, ActiveNotifications.tagHash(null))), "untagged 7");
        check(active.find(7, ActiveNotifications.tagHash("other")) == null, "another tag");
        check(ActiveNotifications.tagHash(null) == ActiveNotifications.tagHash("null"), "no tag is the text null");
        check(ActiveNotifications.tagHash(null) != ActiveNotifications.tagHash(""), "no tag is not the empty one");
        check(active.find(9, ActiveNotifications.tagHash(null)) == null, "another id");
        check(active.find(ActiveNotifications.AUTOGROUP_ID, ActiveNotifications.tagHash("ranker_group")) == null,
                "the system's summary is no tap's");
    }

    public void testTheTagHashIsFnv1a64OfTheUtf8() {
        check(ActiveNotifications.tagHash(null) == ActiveNotifications.tagHash("null"), "no tag is the tag null");
        check(ActiveNotifications.tagHash("") == 0xcbf29ce484222325L, "empty");
        check(ActiveNotifications.tagHash("a") == 0xaf63dc4c8601ec8cL, "a");
        check(ActiveNotifications.tagHash("\u00e4") != ActiveNotifications.tagHash("a"), "UTF-8 bytes");
    }

    public void testACancelOfWhatIsNotThereChangesNothing() {
        ActiveNotifications<String> active = new ActiveNotifications<>();
        active.post("t", 1, null, "a");
        active.cancel("t", 2);
        active.cancel(null, 1);
        check(keys(active).equals(List.of("t|1 a")), "tag and id both count " + keys(active));
    }

    private static void check(boolean condition, String what) {
        if (!condition) {
            throw new AssertionError(what);
        }
    }

    public static void main(String[] args) throws Exception {
        int failed = 0, run = 0;
        for (Method test : ActiveNotificationsTest.class.getDeclaredMethods()) {
            if (!test.getName().startsWith("test")) {
                continue;
            }
            run++;
            try {
                test.invoke(new ActiveNotificationsTest());
            } catch (java.lang.reflect.InvocationTargetException e) {
                failed++;
                System.out.println("FAIL " + test.getName() + ": " + e.getCause());
            }
        }
        System.out.println("ActiveNotificationsTest: " + (run - failed) + " of " + run + " fine");
        if (failed != 0) {
            System.exit(1);
        }
    }
}
