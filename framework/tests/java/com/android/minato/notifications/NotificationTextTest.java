package com.android.minato.notifications;

import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;

/** ADR-0074 decision 2: APKRT_REQUEST_NOTIFICATION's text as the guest builds it, which the runtime checks whole. */
public final class NotificationTextTest {
    public void testAPostListsItsFieldsInOrder() {
        String text = NotificationText.post(7, "chat", "messages", 4, true, 1800000000000L, "friends", "Anna",
                "Group chat", "Hi!\nSee you");
        check(("post\t7\tchat\tmessages\t4\t1\t1800000000000\tfriends\tAnna\tGroup chat\tHi!\nSee you").equals(text),
                text);
    }

    public void testNullFieldsAreEmpty() {
        String text = NotificationText.post(-1, null, null, 3, false, 0, null, null, null, null);
        check("post\t-1\t\t\t3\t0\t0\t\t\t\t".equals(text), text);
    }

    public void testATabIsASpaceAndANewLineOnlyInTheText() {
        String text = NotificationText.post(1, "a\tb", "c", 3, false, 0, "", "x\ny", "s\tt", "line\none\tand");
        check("post\t1\ta b\tc\t3\t0\t0\t\tx y\ts t\tline\none and".equals(text), text);
    }

    public void testControlsBidiAndInvisibleCharactersGo() {
        String title = "a\u0007b\u202Ec\u200Bd\uFEFFe\u2028f\u0085g\u200F\u2060";
        String text = NotificationText.post(1, "", "", 3, false, 0, "", title, "", "");
        check(text.endsWith("\tabcdefg\t\t"), text);
    }

    public void testFieldsAreCutToTheirBytesAtACharacterBoundary() {
        String title = "\u00E4".repeat(200);  // 400 bytes of UTF-8, cut to 256: 128 characters
        String text = NotificationText.post(1, "t".repeat(100), "", 3, false, 0, "", title, "", "z".repeat(5000));
        String[] fields = text.split("\t", -1);
        check(fields[2].length() == 64, "tag " + fields[2].length());
        check(fields[8].equals("\u00E4".repeat(128)), "title " + fields[8].length());
        check(fields[10].getBytes(StandardCharsets.UTF_8).length == 4096, "text");
        String emoji = "\u1F600".repeat(70);  // 280 bytes, cut to 256: 64 whole pairs
        String[] cut = NotificationText.post(1, "", "", 3, false, 0, "", emoji, "", "").split("\t", -1);
        check(cut[8].equals("\u1F600".repeat(64)), "a surrogate pair stays whole");
    }

    public void testTheImportanceIsClamped() {
        check(NotificationText.post(1, "", "", 9, false, 0, "", "", "", "").startsWith("post\t1\t\t\t5\t"), "high");
        check(NotificationText.post(1, "", "", -1000, false, 0, "", "", "", "").startsWith("post\t1\t\t\t0\t"), "low");
    }

    public void testACancelAndACancelOfAll() {
        check("cancel\t7\tchat".equals(NotificationText.cancel(7, "chat")), "cancel");
        check("cancel\t7\t".equals(NotificationText.cancel(7, null)), "no tag");
        check("cancelAll".equals(NotificationText.cancelAll()), "all");
    }

    private static void check(boolean condition, String what) {
        if (!condition) {
            throw new AssertionError(what);
        }
    }

    public static void main(String[] args) throws Exception {
        int failed = 0, run = 0;
        for (Method test : NotificationTextTest.class.getDeclaredMethods()) {
            if (!test.getName().startsWith("test")) {
                continue;
            }
            run++;
            try {
                test.invoke(new NotificationTextTest());
            } catch (java.lang.reflect.InvocationTargetException e) {
                failed++;
                System.out.println("FAIL " + test.getName() + ": " + e.getCause());
            }
        }
        System.out.println("NotificationTextTest: " + (run - failed) + " of " + run + " fine");
        if (failed != 0) {
            System.exit(1);
        }
    }
}
