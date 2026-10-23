package com.android.minato.picker;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.List;
import java.util.Set;

/**
 * What an earlier run left of picks on the host's JVM: a manifest nobody read (a refused or late answer) is deleted
 * and its clone names go to the list, so that the cleaning drops them unless a grant keeps them; photo clones that
 * the index does not name go; links, folders and other names stay.
 */
public final class PickSweepTest {
    private static int failures;

    private static void check(boolean ok, String what) {
        if (!ok) {
            failures++;
            System.out.println("FAIL: " + what);
        }
    }

    private static void write(File file, String text) throws Exception {
        file.getParentFile().mkdirs();
        Files.write(file.toPath(), text.getBytes(StandardCharsets.UTF_8));
    }

    public static void main(String[] args) throws Exception {
        final File root = Files.createTempDirectory("pick-sweep").toFile();
        final File picks = new File(root, "picks");
        final File list = new File(root, "clones");
        final File download = new File(root, "Download");
        write(new File(picks, "7"), "late.txt\nsecond.txt\n");
        write(new File(picks, "8"), "../bad\n");
        write(new File(picks, "9"), "photo.jpg\t10\t10\t0\t0\t0\n");
        write(new File(download, "late.txt"), "l");
        write(new File(download, "second.txt"), "s");
        PickClones.sweepPicks(picks, list);
        check(picks.list().length == 0, "the manifests are deleted");
        PickClones.dropUnkept(download, list, Set.of("second.txt"));
        check(!new File(download, "late.txt").exists(), "the clone of a late answer goes");
        check(new File(download, "second.txt").exists(), "a clone a grant keeps stays");
        PickClones.sweepPicks(new File(root, "none"), list);

        final File photos = new File(root, "photos");
        write(new File(photos, "keep.jpg"), "k");
        write(new File(photos, "stray.jpg"), "s");
        new File(photos, "folder").mkdirs();
        write(new File(root, "outside"), "o");
        Files.createSymbolicLink(new File(photos, "link").toPath(), new File(root, "outside").toPath());
        PickClones.dropStrays(photos, Set.of("keep.jpg"));
        check(new File(photos, "keep.jpg").exists(), "a photo in the index stays");
        check(!new File(photos, "stray.jpg").exists(), "a photo clone nobody names goes");
        check(new File(photos, "folder").isDirectory() && new File(root, "outside").exists(), "folders and links stay");
        if (failures > 0) {
            throw new AssertionError(failures + " failures");
        }
        System.out.println("PickSweepTest ok");
    }
}
