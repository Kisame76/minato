package com.android.minato.picker;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * The URI grants of the app (ADR-0057 step b), as Android's UriGrantsManagerService keeps them for it: a document
 * picker's result grants its URIs the modes of the result's flags; the app keeps a grant with
 * takePersistableUriPermission, which needs a grant with FLAG_GRANT_PERSISTABLE_URI_PERMISSION that covers the modes
 * taken (SecurityException else), and only a kept grant outlives the app's process. A grant with
 * FLAG_GRANT_PREFIX_URI_PERMISSION covers the URIs below its own (a tree's documents). The table's text, one kept grant
 * a line (time, modes, URI, tab-separated), is what the framework stores in the app's data folder; whatever else the
 * file holds is dropped when it is read. Plain Java: tools/framework-host-check.sh compiles it without android.*.
 */
public final class GrantTable {
    /** Intent's FLAG_GRANT_READ_URI_PERMISSION, FLAG_GRANT_WRITE_URI_PERMISSION, ...PERSISTABLE... and ...PREFIX.... */
    public static final int READ = 1, WRITE = 2, PERSISTABLE = 0x40, PREFIX = 0x80;
    private static final int MODES = READ | WRITE;
    /** In the stored text the modes come with this bit when the grant covers the URIs below its own. */
    private static final int STORED_PREFIX = 4;

    /** A kept grant: UriPermission's URI, modes and persisted time. */
    public static final class Entry {
        public final String uri;
        public final int modes;
        public final long time;
        /** Whether the grant covers the URIs below its own: a folder's. */
        public final boolean prefix;

        Entry(String uri, int modes, long time, boolean prefix) {
            this.uri = uri;
            this.modes = modes;
            this.time = time;
            this.prefix = prefix;
        }
    }

    private static final class Grant {
        int flags;
        int kept;
        long time;
    }

    private final Map<String, Grant> mGrants = new LinkedHashMap<>();

    /** Grants `uri` the modes in `flags` (READ, WRITE, with PERSISTABLE and PREFIX as they came). */
    public synchronized void grant(String uri, int flags) {
        final Grant grant = mGrants.computeIfAbsent(uri, key -> new Grant());
        grant.flags |= flags & (MODES | PERSISTABLE | PREFIX);
    }

    /** Whether every mode in `modes` (READ, WRITE) is granted for `uri`, by its own grant or a prefix grant above it. */
    public synchronized boolean allows(String uri, int modes) {
        if ((modes & MODES) == 0 || (modes & ~MODES) != 0) {
            return false;
        }
        for (final Map.Entry<String, Grant> entry : mGrants.entrySet()) {
            final String base = entry.getKey();
            final Grant grant = entry.getValue();
            final boolean covers = uri.equals(base)
                    || ((grant.flags & PREFIX) != 0 && uri.startsWith(base) && uri.charAt(base.length()) == '/');
            if (covers && ((grant.flags | grant.kept) & modes) == modes) {
                return true;
            }
        }
        return false;
    }

    /** Keeps the grant of `uri` for the modes in `modes` at `now`; SecurityException without a grant that allows it. */
    public synchronized void take(String uri, int modes, long now) {
        final Grant grant = mGrants.get(uri);
        modes &= MODES;
        if (grant == null || modes == 0 || (grant.flags & PERSISTABLE) == 0 || (grant.flags & modes) != modes) {
            throw new SecurityException("No persistable permission grants found for Uri " + uri);
        }
        if (grant.kept == 0) {
            grant.time = now;
        }
        grant.kept |= modes;
    }

    /** Drops the modes in `modes` from the kept grant of `uri`; the grant of this run stays. */
    public synchronized void release(String uri, int modes) {
        final Grant grant = mGrants.get(uri);
        if (grant != null) {
            grant.kept &= ~modes;
            if (grant.kept == 0) {
                grant.time = 0;
            }
        }
    }

    private static boolean startsWithAny(String text, String[] prefixes) {
        for (final String prefix : prefixes) {
            if (text.startsWith(prefix)) {
                return true;
            }
        }
        return false;
    }

    /** The kept grants, in the order they were made. */
    public synchronized List<Entry> persisted() {
        final List<Entry> kept = new ArrayList<>();
        for (final Map.Entry<String, Grant> entry : mGrants.entrySet()) {
            if (entry.getValue().kept != 0) {
                kept.add(new Entry(entry.getKey(), entry.getValue().kept, entry.getValue().time,
                        (entry.getValue().flags & PREFIX) != 0));
            }
        }
        return kept;
    }

    /** The text of the kept grants. */
    public synchronized String serialize() {
        final StringBuilder text = new StringBuilder();
        for (final Entry entry : persisted()) {
            text.append(entry.time).append('\t').append(entry.modes | (entry.prefix ? STORED_PREFIX : 0)).append('\t')
                    .append(entry.uri).append('\n');
        }
        return text.toString();
    }

    /**
     * The grants of a text from {@link #serialize}: lines that are not well formed, whose URI starts with none of
     * `uriPrefixes` (the app can write the file, so it grants nothing but what the framework itself grants), and
     * repeated URIs, are dropped.
     */
    public static GrantTable parse(String text, String... uriPrefixes) {
        final GrantTable table = new GrantTable();
        for (final String line : text.split("\n", -1)) {
            final String[] field = line.split("\t", -1);
            if (field.length != 3 || !field[0].matches("[0-9]{1,18}") || !field[1].matches("[1-7]") || (Integer.parseInt(field[1]) & MODES) == 0
                    || !field[2].matches("[a-z][a-z0-9+.-]*://[^\\s]+") || !startsWithAny(field[2], uriPrefixes)
                    || table.mGrants.containsKey(field[2])) {
                continue;
            }
            final Grant grant = new Grant();
            final int stored = Integer.parseInt(field[1]);
            grant.kept = stored & MODES;
            grant.flags = grant.kept | PERSISTABLE | ((stored & STORED_PREFIX) != 0 ? PREFIX : 0);
            grant.time = Long.parseLong(field[0]);
            table.mGrants.put(field[2], grant);
        }
        return table;
    }
}
