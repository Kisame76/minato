package com.android.minato.picker;

import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Set;

/**
 * The host's manifest of a photo pick and the index of the picked photos on the host's JVM
 * (tools/framework-host-check.sh): the app can write the manifest and the index, so only plain names of a known
 * picture or video kind and bounded numbers get through, and anything else makes the answer a cancel.
 */
public final class PhotoManifestTest {
    private static int failures;

    private static void check(boolean ok, String what) {
        if (!ok) {
            failures++;
            System.out.println("FAIL: " + what);
        }
    }

    private static List<PhotoManifest.Item> parse(String text, int count) {
        return PhotoManifest.parse(text.getBytes(StandardCharsets.UTF_8), count);
    }

    private static void refused(String text, int count, String what) {
        check(parse(text, count) == null, "refused: " + what);
    }

    public static void main(String[] args) {
        final List<PhotoManifest.Item> one = parse("a.jpg\t64\t48\t0\t1700000000000\t0\n", 1);
        check(one != null && one.get(0).name.equals("a.jpg") && one.get(0).width == 64 && one.get(0).height == 48
                && one.get(0).durationMs == 0 && one.get(0).takenMs == 1700000000000L
                && one.get(0).orientation == 0 && one.get(0).mimeType().equals("image/jpeg"), "one photo");
        final List<PhotoManifest.Item> two = parse("a.JPG\t1\t2\t0\t3\t90\nclip.mp4\t1920\t1080\t5000\t3\t270\n", 2);
        check(two != null && two.get(0).mimeType().equals("image/jpeg") && two.get(1).mimeType().equals("video/mp4")
                && two.get(1).durationMs == 5000 && two.get(1).orientation == 270, "a photo and a video");
        for (final String name : new String[] {"a.png", "a.gif", "a.webp", "a.bmp", "a.webm", "a.3gp", "a.jpeg"}) {
            check(parse(name + "\t1\t1\t0\t0\t0\n", 1) != null, "kind: " + name);
        }
        refused("a.txt\t1\t1\t0\t0\t0\n", 1, "a name of another kind");
        refused("a\t1\t1\t0\t0\t0\n", 1, "a name without extension");
        refused("a/b.jpg\t1\t1\t0\t0\t0\n", 1, "a slash");
        refused("..\t1\t1\t0\t0\t0\n", 1, "a parent");
        refused("a\u0001.jpg\t1\t1\t0\t0\t0\n", 1, "a control character");
        refused("a.jpg\t1\t1\t0\t0\n", 1, "five fields");
        refused("a.jpg\t1\t1\t0\t0\t0\t0\n", 1, "seven fields");
        refused("a.jpg\t-1\t1\t0\t0\t0\n", 1, "a negative number");
        refused("a.jpg\t01\t1\t0\t0\t0\n", 1, "a leading zero");
        refused("a.jpg\t \t1\t0\t0\t0\n", 1, "a blank number");
        refused("a.jpg\t1.5\t1\t0\t0\t0\n", 1, "a fraction");
        refused("a.jpg\t1\t1\t0\t0\t45\n", 1, "an orientation of 45");
        refused("a.jpg\t1048577\t1\t0\t0\t0\n", 1, "a width over the limit");
        refused("a.jpg\t1\t1\t31536000001\t0\t0\n", 1, "a duration over a year");
        refused("a.jpg\t1\t1\t0\t100000000000001\t0\n", 1, "a time over the limit");
        refused("a.jpg\t1\t1\t0\t1234567890123456789\t0\n", 1, "19 digits");
        refused("a.jpg\t1\t1\t0\t0\t0", 1, "no newline at the end");
        refused("a.jpg\t1\t1\t0\t0\t0\n", 2, "fewer lines than announced");
        refused("a.jpg\t1\t1\t0\t0\t0\nb.jpg\t1\t1\t0\t0\t0\n", 1, "more lines than announced");
        refused("a.jpg\t1\t1\t0\t0\t0\na.jpg\t1\t1\t0\t0\t0\n", 2, "a name twice");
        refused("a.jpg\t1\t1\t0\t0\t0\n", 0, "a count of 0");
        refused("a.jpg\t1\t1\t0\t0\t0\n", 1001, "a count over the limit");
        check(PhotoManifest.parse(new byte[] {'a', (byte) 0xFF, '.', 'j', 'p', 'g', '\n'}, 1) == null,
                "refused: invalid UTF-8");
        check(PhotoManifest.parse(new byte[PhotoManifest.MAX_BYTES + 1], 1) == null, "refused: too many bytes");

        final PhotoIndex index = new PhotoIndex();
        final long first = index.add(one.get(0));
        final long second = index.add(two.get(1));
        check(index.add(one.get(0)) == PhotoIndex.FIRST_ID, "a clone in the index keeps its ID");
        check(first == PhotoIndex.FIRST_ID && second == PhotoIndex.FIRST_ID + 1, "IDs count up from 1000000001");
        check(index.get(first).name.equals("a.jpg") && index.get(second).name.equals("clip.mp4"), "lookup");
        check(index.get(first + 5) == null, "an unknown ID has none");
        final PhotoIndex again = PhotoIndex.parse(index.serialize());
        check(again.ids().equals(index.ids()) && again.get(second).durationMs == 5000
                && again.get(second).orientation == 270, "the index survives its text");
        check(again.add(one.get(0)) == second + 1 || again.add(two.get(0)) > second, "and goes on counting");
        check(PhotoIndex.parse("garbage\n1\ta.jpg\t1\t1\t0\t0\t0\n9999999999\tb.txt\t1\t1\t0\t0\t0\n"
                + "1000000005\t../x.jpg\t1\t1\t0\t0\t0\n").ids().isEmpty(), "bad lines and IDs below the first go");
        check(PhotoIndex.parse("1000000001\ta.jpg\t1\t1\t0\t0\t0\n1000000001\tb.jpg\t1\t1\t0\t0\t0\n"
                + "1000000002\ta.jpg\t1\t1\t0\t0\t0\n").ids().size() == 1, "an ID or a name once");
        final List<String> gone = index.dropUnkept(Set.of(second));
        check(gone.equals(List.of("a.jpg")) && index.get(first) == null && index.get(second) != null,
                "dropping the unkept names the clones that go");
        if (failures != 0) {
            System.out.println(failures + " failures");
            System.exit(1);
        }
        System.out.println("PhotoManifestTest ok");
    }
}
