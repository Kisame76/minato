package com.android.minato.picker;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.function.Supplier;

/**
 * The kept grants of the app and their file, <data>/.minato/uri-grants (ADR-0057 step b). The table is read once, when
 * the app's folder exists, and never cached before: a table cached empty would be stored over the old grants. A file
 * that cannot be read, or is larger than the framework ever writes, is no "no grants": readable() is false then and
 * the cleaning of clones, which deletes what no kept grant names, must not run. take, release and save run one at a
 * time and share the one partial file. Plain Java: tools/framework-host-check.sh compiles it without android.*.
 */
public final class GrantStore {
    /** The most bytes of the file that are read: 1000 picks a time are far below. */
    public static final int MAX_FILE_BYTES = 1 << 20;
    public static final int MAX_PERSISTED = GrantTable.MAX_PERSISTED;

    private final Supplier<File> mFolder;
    private final String[] mUriPrefixes;
    private GrantTable mTable;
    private boolean mReadable;
    private int mFailedSaves;

    /** A store of the folder `folder` gives (null while the app has none); only URIs with `uriPrefixes` are kept. */
    public GrantStore(Supplier<File> folder, String... uriPrefixes) {
        mFolder = folder;
        mUriPrefixes = uriPrefixes;
    }

    /**
     * The text of `file`: "" when there is none, null when it is a link, no regular file, larger than the limit or
     * cannot be read.
     */
    public static String readText(File file) {
        if (!Files.isSymbolicLink(file.toPath()) && !file.exists()) {
            return "";
        }
        try {
            if (Files.isSymbolicLink(file.toPath()) || !file.isFile() || file.length() > MAX_FILE_BYTES) {
                return null;
            }
            return new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
        } catch (IOException | RuntimeException e) {
            return null;
        }
    }

    /** The table; an empty one that is not kept while the folder does not exist yet. */
    public synchronized GrantTable table() {
        if (mTable != null) {
            return mTable;
        }
        final File folder = mFolder.get();
        if (folder == null) {
            return new GrantTable();
        }
        GrantTable table = new GrantTable();
        boolean readable = false;
        final String text = readText(new File(folder, "uri-grants"));
        if (text != null) {
            try {
                table = GrantTable.parse(text, mUriPrefixes);
                readable = true;
            } catch (RuntimeException e) {
                table = new GrantTable();
            }
        }
        mTable = table;
        mReadable = readable;
        return mTable;
    }

    /** Whether the stored grants were read (none stored counts): only then may clones be deleted by them. */
    public synchronized boolean readable() {
        table();
        return mTable != null && mReadable;
    }

    public synchronized void take(String uri, int modes, long now) {
        table().take(uri, modes, now);
        table().trim(MAX_PERSISTED);
        save();
    }

    public synchronized void release(String uri, int modes) {
        table().release(uri, modes);
        save();
    }

    /** Stores the table; a failure is counted, the grants of the run stay. */
    public synchronized void save() {
        final File folder = mFolder.get();
        if (folder == null || mTable == null) {
            return;
        }
        try {
            folder.mkdirs();
            final File partial = new File(folder, "uri-grants.partial");
            Files.write(partial.toPath(), mTable.serialize().getBytes(StandardCharsets.UTF_8));
            Files.move(partial.toPath(), new File(folder, "uri-grants").toPath(), StandardCopyOption.REPLACE_EXISTING);
        } catch (IOException e) {
            mFailedSaves++;
        }
    }

    public synchronized int failedSaves() {
        return mFailedSaves;
    }
}
