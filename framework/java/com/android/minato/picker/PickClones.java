package com.android.minato.picker;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

/**
 * The clones of an app's picks in its Download folder (ADR-0057 decision 2): the host clones each pick there, and a
 * clone lives as long as the app keeps its grant. The framework lists the names it was told about in a file of the
 * app's data folder, and at the app's next start deletes those that no kept grant names, so that the folder holds the
 * kept clones and the app's own files. Only plain names in the list are used, as the app can write the list too, and
 * nothing but a regular file of that name in Download is deleted. Plain Java: tools/framework-host-check.sh compiles
 * it without android.*.
 */
public final class PickClones {
    /** The most bytes a list is read with: many times what the framework writes between two starts. */
    public static final int MAX_LIST_BYTES = 1 << 20;

    private PickClones() {
    }

    /** Adds `names` to the list file `list`. */
    public static void add(File list, List<String> names) throws IOException {
        final StringBuilder text = new StringBuilder();
        for (final String name : names) {
            text.append(name).append('\n');
        }
        if (list.getParentFile() != null) {
            list.getParentFile().mkdirs();
        }
        Files.write(list.toPath(), text.toString().getBytes(StandardCharsets.UTF_8), StandardOpenOption.CREATE,
                StandardOpenOption.APPEND);
    }

    /** Deletes the listed clones in `download` that are not in `keep`, and rewrites the list with those that are. */
    public static void dropUnkept(File download, File list, Set<String> keep) throws IOException {
        if (Files.isSymbolicLink(list.toPath()) || !list.isFile()) {
            return;
        }
        if (list.length() > MAX_LIST_BYTES) {
            // Not a list the framework wrote (1000 picks a time are 256 KB): the app made it, and nothing is deleted.
            list.delete();
            return;
        }
        final Set<String> kept = new LinkedHashSet<>();
        for (final String name : new String(Files.readAllBytes(list.toPath()), StandardCharsets.UTF_8).split("\n")) {
            if (!PickManifest.plainName(name)) {
                continue;
            }
            if (keep.contains(name)) {
                kept.add(name);
                continue;
            }
            final File file = new File(download, name);
            if (!Files.isSymbolicLink(file.toPath()) && file.isFile()) {
                file.delete();
            }
        }
        final StringBuilder text = new StringBuilder();
        for (final String name : kept) {
            text.append(name).append('\n');
        }
        final File partial = new File(list.getPath() + ".partial");
        Files.write(partial.toPath(), text.toString().getBytes(StandardCharsets.UTF_8));
        Files.move(partial.toPath(), list.toPath(), StandardCopyOption.REPLACE_EXISTING);
    }
}
