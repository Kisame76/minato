package com.android.minato.picker;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.List;
import java.util.Set;

/**
 * The clones of an app's picks on the host's JVM (tools/framework-host-check.sh): at the next start the listed clones
 * that no kept grant names go, the kept ones and the app's own files stay, and a name in the list that is not a plain
 * name, or names a link or a folder, deletes nothing.
 */
public final class PickClonesTest {
    private static int failures;

    private static void check(boolean ok, String what) {
        if (!ok) {
            failures++;
            System.out.println("FAIL: " + what);
        }
    }

    private static void write(File file, String text) throws Exception {
        Files.write(file.toPath(), text.getBytes(StandardCharsets.UTF_8));
    }

    public static void main(String[] args) throws Exception {
        final File root = Files.createTempDirectory("pick-clones").toFile();
        final File download = new File(root, "Download");
        final File list = new File(root, "data/.minato/clones");
        download.mkdirs();
        write(new File(download, "kept.txt"), "k");
        write(new File(download, "gone.txt"), "g");
        write(new File(download, "own.txt"), "o");
        write(new File(root, "outside.txt"), "x");
        new File(download, "folder").mkdir();
        Files.createSymbolicLink(new File(download, "link").toPath(), new File(root, "outside.txt").toPath());
        PickClones.add(list, List.of("kept.txt", "gone.txt"));
        PickClones.add(list, List.of("gone.txt", "missing.txt", "../outside.txt", "folder", "link", "a/b"));
        PickClones.dropUnkept(download, list, Set.of("kept.txt"));
        check(new File(download, "kept.txt").exists(), "a kept clone stays");
        check(!new File(download, "gone.txt").exists(), "a clone nobody keeps goes");
        check(new File(download, "own.txt").exists(), "an unlisted file stays");
        check(new File(root, "outside.txt").exists(), "a name with a parent deletes nothing outside");
        check(new File(download, "folder").isDirectory(), "a folder stays");
        check(new File(download, "link").exists() && new File(root, "outside.txt").exists(), "a link stays");
        check(new String(Files.readAllBytes(list.toPath()), StandardCharsets.UTF_8).equals("kept.txt\n"),
                "the list holds the kept names only");
        PickClones.dropUnkept(download, list, Set.of());
        check(!new File(download, "kept.txt").exists(), "a clone whose grant was released goes at the next start");
        write(new File(download, "big.txt"), "b");
        write(list, "big.txt\n" + "x".repeat(PickClones.MAX_LIST_BYTES));
        PickClones.dropUnkept(download, list, Set.of());
        check(new File(download, "big.txt").exists() && !list.exists(), "a list over the limit deletes nothing");
        PickClones.dropUnkept(download, new File(root, "no-such-list"), Set.of());
        check(new File(download, "own.txt").exists(), "without a list nothing happens");
        if (failures != 0) {
            System.out.println(failures + " failures");
            System.exit(1);
        }
        System.out.println("PickClonesTest ok");
    }
}
