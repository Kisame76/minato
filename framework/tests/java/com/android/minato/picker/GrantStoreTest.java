package com.android.minato.picker;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicReference;

/**
 * The store of the kept grants on the host's JVM (tools/framework-host-check.sh): an unreadable or oversize file is
 * not "no grants" (the cleaning must not run on it), a table is not cached before the app's folder exists, the kept
 * grants are capped at Android's 512 with the oldest going first, and take, release and save run one at a time.
 */
public final class GrantStoreTest {
    private static int failures;
    private static final String URI = "content://a.b/document/p%3A";

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
        final File root = Files.createTempDirectory("grant-store").toFile();

        final File big = new File(root, "big");
        write(new File(big, "uri-grants"), "1\t3\t" + URI + "x\n" + " ".repeat(GrantStore.MAX_FILE_BYTES));
        final GrantStore bigStore = new GrantStore(() -> big, URI);
        check(bigStore.table().persisted().isEmpty(), "an oversize file holds no grants");
        check(!bigStore.readable(), "an oversize file is not readable: the cleaning must not run");

        final File absent = new File(root, "absent");
        absent.mkdirs();
        check(new GrantStore(() -> absent, URI).readable(), "no file is a readable, empty set");

        final File late = new File(root, "late");
        write(new File(late, "uri-grants"), "5\t3\t" + URI + "a\n");
        final AtomicReference<File> folder = new AtomicReference<>();
        final GrantStore lateStore = new GrantStore(folder::get, URI);
        check(lateStore.table().persisted().isEmpty(), "before the folder exists the table is empty");
        check(!lateStore.readable(), "before the folder exists nothing is readable");
        folder.set(late);
        check(lateStore.table().persisted().size() == 1, "the table is not cached before the folder exists");
        check(lateStore.readable(), "after the folder exists the file is read");

        final File capped = new File(root, "capped");
        final GrantStore cappedStore = new GrantStore(() -> capped, URI);
        for (int i = 0; i < 600; i++) {
            cappedStore.table().grant(URI + i, GrantTable.READ | GrantTable.PERSISTABLE);
            cappedStore.take(URI + i, GrantTable.READ, 1000 + i);
        }
        final List<GrantTable.Entry> kept = cappedStore.table().persisted();
        check(kept.size() == GrantStore.MAX_PERSISTED, "512 grants are kept, not " + kept.size());
        check(cappedStore.table().persisted().stream().noneMatch(e -> e.uri.equals(URI + 0)), "the oldest went");
        check(cappedStore.table().persisted().stream().anyMatch(e -> e.uri.equals(URI + 599)), "the newest stays");

        final File many = new File(root, "many");
        final StringBuilder lines = new StringBuilder();
        for (int i = 0; i < 700; i++) {
            lines.append(i + 1).append("\t1\t").append(URI).append(i).append('\n');
        }
        write(new File(many, "uri-grants"), lines.toString());
        final List<GrantTable.Entry> read = new GrantStore(() -> many, URI).table().persisted();
        check(read.size() == GrantStore.MAX_PERSISTED && read.stream().noneMatch(e -> e.uri.equals(URI + 0)),
                "a file of 700 is read as the newest 512");

        final File busy = new File(root, "busy");
        final GrantStore busyStore = new GrantStore(() -> busy, URI);
        final List<Throwable> errors = new ArrayList<>();
        final List<Thread> threads = new ArrayList<>();
        for (int t = 0; t < 8; t++) {
            final int id = t;
            final Thread thread = new Thread(() -> {
                try {
                    for (int i = 0; i < 150; i++) {
                        final String uri = URI + "t" + id + "-" + i;
                        busyStore.table().grant(uri, GrantTable.READ | GrantTable.PERSISTABLE);
                        busyStore.take(uri, GrantTable.READ, i);
                        if (i % 2 == 0) {
                            busyStore.release(uri, GrantTable.READ);
                        }
                    }
                } catch (Throwable e) {
                    synchronized (errors) {
                        errors.add(e);
                    }
                }
            });
            threads.add(thread);
            thread.start();
        }
        for (final Thread thread : threads) {
            thread.join();
        }
        check(errors.isEmpty(), "take and release run on many threads: " + errors);
        check(busyStore.failedSaves() == 0, "no save failed: " + busyStore.failedSaves());
        check(new GrantStore(() -> busy, URI).table().persisted().size() == busyStore.table().persisted().size(),
                "the stored file is the table");

        if (failures > 0) {
            throw new AssertionError(failures + " failures");
        }
        System.out.println("GrantStoreTest ok");
    }
}
