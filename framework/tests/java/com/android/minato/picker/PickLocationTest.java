package com.android.minato.picker;

import java.nio.charset.StandardCharsets;

/**
 * The host's answer to a save or folder request on the host's JVM (tools/framework-host-check.sh): one line, a path
 * of plain names below the storage's root; anything else is refused (null), as the app can write the file too.
 */
public final class PickLocationTest {
    private static int failures;

    private static void check(boolean ok, String what) {
        if (!ok) {
            failures++;
            System.out.println("FAIL: " + what);
        }
    }

    private static String parse(String text) {
        return PickLocation.parse(text.getBytes(StandardCharsets.UTF_8));
    }

    public static void main(String[] args) {
        check("Download/note.txt".equals(parse("Download/note.txt\n")), "a path");
        check("Download".equals(parse("Download\n")), "one name");
        check("Minato/1/note 😀 ä (1).txt".equals(parse("Minato/1/note 😀 ä (1).txt\n")),
                "an emoji and an umlaut stay");
        for (final String bad : new String[] {"", "\n", "a", "/a\n", "a/\n", "a//b\n", "../a\n", "a/../b\n", "a/./b\n",
            "a\u0000b\n", "a\tb\n", "a\u202eb\n", "a\r\n", "a\nb\n", "a/b\nc\n", "x".repeat(256) + "\n"}) {
            check(parse(bad) == null, "refused: " + bad.replace("\n", "\\n"));
        }
        check(parse("a/".repeat(15) + "a\n") != null, "16 components");
        check(parse("a/".repeat(16) + "a\n") == null, "17 components refused");
        check(PickLocation.parse(new byte[] {'a', (byte) 0xFF, '\n'}) == null, "invalid UTF-8 refused");
        check(PickLocation.parse(new byte[PickLocation.MAX_BYTES + 2]) == null, "too many bytes refused");
        if (failures != 0) {
            System.out.println(failures + " failures");
            System.exit(1);
        }
        System.out.println("PickLocationTest ok");
    }
}
