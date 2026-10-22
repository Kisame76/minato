package com.android.minato.picker;

import java.nio.charset.StandardCharsets;

/**
 * The text of a picker request on the host's JVM (tools/framework-host-check.sh): whatever an app puts into its
 * picker intent, the text stays inside the limits that the runtime enforces (guest_interface.h), so the runtime does
 * not refuse the request of an app that did nothing wrong, and a hostile value never reaches the host as it is.
 */
public final class PickerRequestTest {
    private static int failures;

    private static void check(boolean ok, String what) {
        if (!ok) {
            failures++;
            System.out.println("FAIL: " + what);
        }
    }

    /** The runtime's rules for the text, written again: five tab-separated fields inside the limits. */
    private static boolean wellFormed(String text) {
        if (text.getBytes(StandardCharsets.UTF_8).length >= 1535) {
            return false;
        }
        final String[] field = text.split("\t", -1);
        if (field.length != 5) {
            return false;
        }
        if (!field[0].matches("[1-5]") || !field[1].matches("[01]") || !field[2].matches("0|[1-9][0-9]{0,3}")
                || Integer.parseInt(field[2]) > 1000) {
            return false;
        }
        final int kind = field[0].charAt(0) - '0';
        if ((field[1].equals("1") && kind != 1 && kind != 2 && kind != 5) || (!field[2].equals("0") && kind != 5)
                || (!field[3].isEmpty() && kind == 4) || (!field[4].isEmpty() && kind != 3)) {
            return false;
        }
        if (!field[3].isEmpty()) {
            final String[] types = field[3].split("\n", -1);
            if (types.length > 8) {
                return false;
            }
            for (String type : types) {
                if (!type.matches("[A-Za-z0-9!#$&^_.+*-]+/[A-Za-z0-9!#$&^_.+*-]+") || type.length() > 100) {
                    return false;
                }
            }
        }
        if (field[4].getBytes(StandardCharsets.UTF_8).length > 200) {
            return false;
        }
        return field[4].codePoints().allMatch(PickerRequest::plainCodePoint);
    }

    private static void sane() {
        final String text = PickerRequest.text(5, "image/*", new String[] {"video/*"}, true, 20, "ignored");
        check(text.equals("5\t1\t20\timage/*\nvideo/*\t"), "a plain request: " + text);
        check(wellFormed(text), "a plain request is well formed");
        check(PickerRequest.text(3, "text/plain", null, false, 0, "note.txt").equals("3\t0\t0\ttext/plain\tnote.txt"),
                "a save");
        check(PickerRequest.text(4, null, null, false, 0, null).equals("4\t0\t0\t\t"), "a folder: no types, no title");
        check(PickerRequest.text(4, "text/plain", new String[] {"a/b"}, true, 9, "t").equals("4\t0\t0\t\t"),
                "a folder takes no types, several, maximum or title");
        check(PickerRequest.text(1, "a/b", null, true, 9, "t").equals("1\t1\t0\ta/b\t"),
                "an open: several, no maximum");
        check(PickerRequest.text(2, "a/b", null, false, 9, "t").equals("2\t0\t0\ta/b\t"),
                "a get: no maximum, no title");
        check(PickerRequest.text(3, "a/b", null, true, 9, "t").equals("3\t0\t0\ta/b\tt"),
                "a save: a title, not several");
    }

    private static void hostileTypes() {
        final String[] hostile = {"foo", "", "/", "a/", "/b", "a/b/c", "a/b\nc/d", "a/b\tc/d", "a/b;c", "a/b c",
            "a/b\u0000", "a/ä", "a/b\r", "text/plain\ntext/html", "a".repeat(101) + "/b", "a/" + "b".repeat(99)};
        for (String type : hostile) {
            final String text = PickerRequest.text(1, type, null, false, 0, null);
            check(wellFormed(text), "type " + type.length() + " chars stays well formed: " + text.replace("\n", "\\n"));
            if (!type.equals("a/" + "b".repeat(99)) && !type.equals("text/plain\ntext/html")) {
                check(text.equals("1\t0\t0\t\t"), "the hostile type is left out: " + text.replace("\n", "\\n"));
            }
        }
        check(PickerRequest.text(1, "a/" + "b".repeat(98), null, false, 0, null).contains("b".repeat(98)),
                "a type of 100 characters is kept");
        final String[] many = new String[30];
        for (int i = 0; i < many.length; i++) {
            many[i] = "x/" + i;
        }
        final String text = PickerRequest.text(2, "a/a", many, false, 0, null);
        check(wellFormed(text), "30 types stay well formed");
        check(text.split("\t", -1)[3].split("\n").length == 8, "30 types are cut to 8");
        check(wellFormed(PickerRequest.text(2, null, new String[] {null, "a/a", null}, false, 0, null)),
                "null entries are left out");
    }

    private static void hostileTitles() {
        final String[] titles = {"new\nline", "tab\there", "bell\u0007", "esc\u001b[2J", "nul\u0000", "del\u007f",
            "c1\u0085control", "line\u2028sep", "para\u2029sep", "bidi\u202eoverride",
            "isolate\u2066x", "zero\u200bwidth", "bom\ufeffmark", "note\ufff9x", "lone high \ud800",
            "lone low \udc00", "x".repeat(10000), "😀".repeat(300), "€".repeat(300), "ä".repeat(300)};
        for (String title : titles) {
            final String text = PickerRequest.text(3, null, null, false, 0, title);
            check(wellFormed(text), "title " + title.length() + " chars stays well formed");
            check(!text.contains("\ud800") && !text.contains("\udc00"), "no lone surrogate survives");
        }
        check(PickerRequest.text(3, null, null, false, 0, "x".repeat(250)).endsWith("\t" + "x".repeat(200)),
                "a long title is cut to 200 bytes");
        check(PickerRequest.text(3, null, null, false, 0, "€".repeat(100)).endsWith("\t" + "€".repeat(66)),
                "a title of 3-byte characters is cut at a character: 66 of them are 198 bytes");
        check(PickerRequest.text(3, null, null, false, 0, "😀".repeat(100)).endsWith(
                "\t" + "😀".repeat(50)), "a title of 4-byte characters is cut at a code point");
        check(PickerRequest.text(3, null, null, false, 0, "café € 😀").endsWith(
                "\tcafé € 😀"), "UTF-8 of every length is kept");
        check(PickerRequest.text(3, null, null, false, 0, "a\nb\tc").endsWith("\tabc"), "control characters go");
    }

    private static void hostileMaximum() {
        check(PickerRequest.text(5, null, null, false, -5, null).equals("5\t0\t0\t\t"), "a negative maximum is 0");
        check(PickerRequest.text(5, null, null, false, Integer.MIN_VALUE, null).equals("5\t0\t0\t\t"),
                "the smallest int is 0");
        check(PickerRequest.text(5, null, null, false, 99999, null).equals("5\t0\t1000\t\t"), "a huge maximum is 1000");
        check(PickerRequest.text(5, null, null, false, Integer.MAX_VALUE, null).equals("5\t0\t1000\t\t"),
                "the largest int is 1000");
        check(PickerRequest.text(5, null, null, false, 1000, null).equals("5\t0\t1000\t\t"), "1000 is kept");
    }

    public static void main(String[] args) {
        sane();
        hostileTypes();
        hostileTitles();
        hostileMaximum();
        if (failures > 0) {
            System.out.println("PickerRequestTest: " + failures + " failures");
            System.exit(1);
        }
        System.out.println("ok   PickerRequestTest");
    }
}
