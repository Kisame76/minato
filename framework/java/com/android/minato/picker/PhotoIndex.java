package com.android.minato.picker;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * The photos and videos the photo picker returned to the app (ADR-0057 step c), by the ID in their picker URI
 * (content://media/picker/0/com.android.providers.media.photopicker/media/&lt;ID&gt;): Android's IDs are MediaStore's,
 * here a counter from 1000000001. The index is stored in the app's data folder, so that a kept grant still finds its
 * clone after a restart: one line a photo (ID, then PhotoManifest's six fields, tab-separated). Whatever else the file
 * holds is dropped when it is read. Plain Java: tools/framework-host-check.sh compiles it without android.*.
 */
public final class PhotoIndex {
    public static final long FIRST_ID = 1_000_000_001L;

    private final Map<Long, PhotoManifest.Item> mItems = new LinkedHashMap<>();
    private long mNext = FIRST_ID;

    /** Adds `item`, the ID it gets; a clone that is in the index already keeps its ID, as a photo has one ID. */
    public synchronized long add(PhotoManifest.Item item) {
        for (final Map.Entry<Long, PhotoManifest.Item> entry : mItems.entrySet()) {
            if (entry.getValue().name.equals(item.name)) {
                return entry.getKey();
            }
        }
        final long id = mNext++;
        mItems.put(id, item);
        return id;
    }

    public synchronized PhotoManifest.Item get(long id) {
        return mItems.get(id);
    }

    /** The IDs, in the order they were added. */
    public synchronized List<Long> ids() {
        return new ArrayList<>(mItems.keySet());
    }

    /** Drops every photo whose ID is not in `keep`; the names of the clones that went. */
    public synchronized List<String> dropUnkept(Set<Long> keep) {
        final List<String> gone = new ArrayList<>();
        mItems.entrySet().removeIf(entry -> {
            if (keep.contains(entry.getKey())) {
                return false;
            }
            gone.add(entry.getValue().name);
            return true;
        });
        return gone;
    }

    public synchronized String serialize() {
        final StringBuilder text = new StringBuilder();
        for (final Map.Entry<Long, PhotoManifest.Item> entry : mItems.entrySet()) {
            final PhotoManifest.Item item = entry.getValue();
            text.append(entry.getKey()).append('\t').append(item.name).append('\t').append(item.width).append('\t')
                    .append(item.height).append('\t').append(item.durationMs).append('\t').append(item.takenMs)
                    .append('\t').append(item.orientation).append('\n');
        }
        return text.toString();
    }

    /** The index of a text from {@link #serialize}: lines that are not well formed, and repeated IDs or names, go. */
    public static PhotoIndex parse(String text) {
        final PhotoIndex index = new PhotoIndex();
        final Set<String> names = new java.util.HashSet<>();
        for (final String line : text.split("\n", -1)) {
            final int tab = line.indexOf('\t');
            if (tab < 1 || !line.substring(0, tab).matches("[0-9]{10,18}")) {
                continue;
            }
            final long id = Long.parseLong(line.substring(0, tab));
            final List<PhotoManifest.Item> parsed = PhotoManifest.parse((line.substring(tab + 1) + "\n")
                    .getBytes(java.nio.charset.StandardCharsets.UTF_8), 1);
            if (id < FIRST_ID || parsed == null || index.mItems.containsKey(id) || !names.add(parsed.get(0).name)) {
                continue;
            }
            index.mItems.put(id, parsed.get(0));
            index.mNext = Math.max(index.mNext, id + 1);
        }
        return index;
    }
}
