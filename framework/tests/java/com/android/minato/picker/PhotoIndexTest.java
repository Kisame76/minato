package com.android.minato.picker;

import java.util.List;
import java.util.Set;

/** The photo index on the host's JVM: an ID a dropped photo had is not given again, also after a restart. */
public final class PhotoIndexTest {
    private static int failures;

    private static void check(boolean ok, String what) {
        if (!ok) {
            failures++;
            System.out.println("FAIL: " + what);
        }
    }

    private static PhotoManifest.Item item(String name) {
        return PhotoManifest.parse((name + "\t1\t1\t0\t0\t0\n").getBytes(java.nio.charset.StandardCharsets.UTF_8), 1)
                .get(0);
    }

    public static void main(String[] args) {
        final PhotoIndex index = new PhotoIndex();
        final long a = index.add(item("a.jpg"));
        final long b = index.add(item("b.jpg"));
        final long c = index.add(item("c.jpg"));
        index.dropUnkept(Set.of(a, b));
        check(index.add(item("d.jpg")) == c + 1, "a dropped ID is not given again in the run");
        final PhotoIndex again = PhotoIndex.parse(index.serialize());
        check(again.add(item("e.jpg")) == c + 2, "nor after the index was stored and read");
        final PhotoIndex dropped = new PhotoIndex();
        dropped.add(item("a.jpg"));
        final long second = dropped.add(item("b.jpg"));
        dropped.dropUnkept(Set.of());
        final PhotoIndex restarted = PhotoIndex.parse(dropped.serialize());
        check(restarted.add(item("c.jpg")) == second + 1, "an empty index remembers the next ID");
        check(PhotoIndex.parse("next\tabc\n").add(item("x.jpg")) == PhotoIndex.FIRST_ID, "a bad next line is dropped");
        check(PhotoIndex.parse("next\t5\n").add(item("x.jpg")) == PhotoIndex.FIRST_ID,
                "a next below the first ID is dropped");
        final List<Long> ids = again.ids();
        check(ids.equals(List.of(a, b, c + 1, c + 2)), "the IDs are " + ids);
        if (failures > 0) {
            throw new AssertionError(failures + " failures");
        }
        System.out.println("PhotoIndexTest ok");
    }
}
