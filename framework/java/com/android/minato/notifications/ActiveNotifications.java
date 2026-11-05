package com.android.minato.notifications;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;

/**
 * The app's active notifications (ADR-0074 decision 2), as NotificationManagerService keeps them for
 * getActiveNotifications, by tag and id: a post replaces the one of the same tag and id, a cancel takes it, a cancel
 * of all takes everything. Android 15's GroupHelper bundles the app's notifications that have no group of their own
 * once there are two: the system's summary ("ranker_group", Integer.MAX_VALUE, a value of null) is active while one of
 * them is left [measured: notify-test-granted on apk-runner-35]. `T` is what the service keeps of each, its
 * StatusBarNotification. Plain Java, so tools/framework-host-check.sh runs it on the host's JVM.
 */
public final class ActiveNotifications<T> {
    /** GroupHelper's AUTOGROUP_KEY and its summary's id. */
    public static final String AUTOGROUP_TAG = "ranker_group";
    public static final int AUTOGROUP_ID = Integer.MAX_VALUE;
    /** The ungrouped notifications that bring the summary, as on apk-runner-35. */
    static final int AUTOGROUP_AT = 2;

    /** One active notification. */
    public static final class Entry<T> {
        public final String tag;
        public final int id;
        public final String group;
        public final T value;

        Entry(String tag, int id, String group, T value) {
            this.tag = tag;
            this.id = id;
            this.group = group;
            this.value = value;
        }

        /** The system's group summary. */
        public boolean isAutogroupSummary() {
            return AUTOGROUP_TAG.equals(tag) && id == AUTOGROUP_ID && value == null;
        }
    }

    private final Map<String, Entry<T>> mEntries = new LinkedHashMap<>();
    private boolean mSummary;

    private static String key(String tag, int id) {
        return Objects.toString(tag) + "|" + id;
    }

    public synchronized void post(String tag, int id, String group, T value) {
        mEntries.put(key(tag, id), new Entry<>(tag, id, group, value));
        if (ungrouped() >= AUTOGROUP_AT) {
            mSummary = true;
        }
    }

    public synchronized void cancel(String tag, int id) {
        mEntries.remove(key(tag, id));
        if (ungrouped() == 0) {
            mSummary = false;
        }
    }

    public synchronized void cancelAll() {
        mEntries.clear();
        mSummary = false;
    }

    /**
     * The app's own notification of `id` whose tag's tagHash is `hash`, as the host's tap names it
     * (APKRT_EVENT_NOTIFICATION_TAP); null for none. The system's summary is never one.
     */
    public synchronized T find(int id, long hash) {
        for (Entry<T> entry : mEntries.values()) {
            if (entry.id == id && tagHash(entry.tag) == hash) {
                return entry.value;
            }
        }
        return null;
    }

    /** FNV-1a 64 of the tag's UTF-8, no tag as "null" as in the host's text: the host's NotificationTap.tagHash. */
    public static long tagHash(String tag) {
        long hash = 0xcbf29ce484222325L;
        for (byte b : Objects.toString(tag).getBytes(StandardCharsets.UTF_8)) {
            hash ^= b & 0xff;
            hash *= 0x100000001b3L;
        }
        return hash;
    }

    /** The entries, the system's summary among them while it is active. */
    public synchronized List<Entry<T>> entries() {
        List<Entry<T>> all = new ArrayList<>(mEntries.values());
        if (mSummary) {
            all.add(new Entry<>(AUTOGROUP_TAG, AUTOGROUP_ID, AUTOGROUP_TAG, null));
        }
        return all;
    }

    private int ungrouped() {
        int count = 0;
        for (Entry<T> entry : mEntries.values()) {
            if (entry.group == null || entry.group.isEmpty()) {
                count++;
            }
        }
        return count;
    }
}
