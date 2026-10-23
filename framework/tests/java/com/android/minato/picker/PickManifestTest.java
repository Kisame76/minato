package com.android.minato.picker;

import java.nio.charset.StandardCharsets;
import java.util.List;

/**
 * The host's manifest of a pick on the host's JVM (tools/framework-host-check.sh): the file names of the clones in the
 * app's Download folder, one per line. The framework reads it from the app's own data folder, which the app can write
 * too, so whatever is in it, only plain file names of the number the host announced get through, and anything else
 * makes the whole answer a cancel (parse returns null).
 */
public final class PickManifestTest {
    private static int failures;

    private static void check(boolean ok, String what) {
        if (!ok) {
            failures++;
            System.out.println("FAIL: " + what);
        }
    }

    private static List<String> parse(String text, int count) {
        return PickManifest.parse(text.getBytes(StandardCharsets.UTF_8), count);
    }

    private static void refused(String text, int count, String what) {
        check(parse(text, count) == null, "refused: " + what);
    }

    public static void main(String[] args) {
        check(parse("known.txt\n", 1).equals(List.of("known.txt")), "one name");
        check(parse("known.txt\ndata.bin\n", 2).equals(List.of("known.txt", "data.bin")), "two names, in order");
        check(parse("note 😀 ä (1).txt\n", 1).equals(List.of("note 😀 ä (1).txt")),
                "spaces, an emoji and an umlaut stay");
        refused("known.txt\n", 2, "fewer names than announced");
        refused("known.txt\ndata.bin\n", 1, "more names than announced");
        refused("known.txt", 1, "no newline at the end");
        refused("", 1, "an empty file for one pick");
        refused("\n", 1, "an empty name");
        refused("known.txt\n\n", 2, "an empty line");
        refused("a/b.txt\n", 1, "a slash");
        refused("../x\n", 1, "a parent");
        refused("..\n", 1, "..");
        refused(".\n", 1, ".");
        refused("a\0b\n", 1, "a NUL");
        refused("a\tb\n", 1, "a tab");
        refused("a\rb\n", 1, "a carriage return");
        refused("a\u0001b\n", 1, "a control character");
        refused("a\u007fb\n", 1, "DEL");
        refused("a\u0085b\n", 1, "a C1 control character");
        refused("a\u202eb\n", 1, "a bidi override");
        refused("a\u200bb\n", 1, "a zero-width space");
        refused("a\u2028b\n", 1, "a line separator");
        check(PickManifest.parse(new byte[] {'a', (byte) 0xC0, (byte) 0x80, '\n'}, 1) == null, "refused: overlong NUL");
        check(PickManifest.parse(new byte[] {'a', (byte) 0xFF, '\n'}, 1) == null, "refused: invalid UTF-8");
        check(PickManifest.parse(new byte[] {(byte) 0xED, (byte) 0xA0, (byte) 0x80, '\n'}, 1) == null,
                "refused: a surrogate encoded as UTF-8 (CESU-8)");
        check(parse("x".repeat(255) + "\n", 1) != null, "255 bytes");
        refused("x".repeat(256) + "\n", 1, "256 bytes");
        check(parse("ä".repeat(127) + "\n", 1) != null, "254 bytes of umlauts");
        refused("ä".repeat(128) + "x\n", 1, "256 bytes of umlauts and an x");
        refused("known.txt\n", 0, "a count of 0");
        refused("known.txt\n", -1, "a negative count");
        refused("known.txt\n", 1001, "a count over the limit");
        final StringBuilder many = new StringBuilder();
        for (int i = 0; i < 1000; i++) {
            many.append("f").append(i).append('\n');
        }
        check(parse(many.toString(), 1000).size() == 1000, "1000 names");
        refused(many + "f1000\n", 1001, "1001 names");
        check(PickManifest.parse(new byte[PickManifest.MAX_BYTES + 1], 1) == null, "refused: more bytes than 1000 names");
        refused("known.txt\nknown.txt\n", 2, "the same name twice");
        if (failures != 0) {
            System.out.println(failures + " failures");
            System.exit(1);
        }
        System.out.println("PickManifestTest ok");
    }
}
