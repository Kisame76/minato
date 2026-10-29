/*
 * The shortcut service (gap 1 of the compatibility plan, batch 3): IShortcutService of system_server, local in the
 * app's process, which ServiceManager's table gives as "shortcut" and AOSP's ShortcutManager asks. It keeps the app's
 * dynamic shortcuts as Android 15's ShortcutService does for its publisher [verified: system-services-test on
 * apk-runner-35]:
 *   - an incoming shortcut without an activity gets the app's launcher activity, and must then have its mandatory
 *     fields (ShortcutInfo.enforceMandatoryFields: an ID, a short label, an intent);
 *   - at most 15 dynamic shortcuts per activity; more is refused with Android's message, before anything changes;
 *   - ranks: each activity's dynamic shortcuts are sorted by rank, a changed rank first among equal ones, then by their
 *     order in the call, and numbered from 0; a pushed shortcut without a rank gets rank 0, and pushes out the last one
 *     at the maximum; an update copies the fields it has;
 *   - disabling removes the dynamic shortcuts it names, as no shortcut is pinned;
 *   - the shortcuts come back as clones for their creator, in the order of the service's map (by ID's hash, from the
 *     end);
 *   - the app is in front, so it is never rate limited.
 * No launcher runs, so no shortcut is pinned, pinning is not supported and the manifest's shortcuts are not read; the
 * shortcuts live as long as the process, where Android keeps them on disk. Every other call is AIDL's Default answer.
 */
package android.content.pm;

import android.app.ActivityThread;
import android.app.Application;
import android.content.ComponentName;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.IntentSender;
import android.os.Binder;
import android.os.IBinder;
import android.util.ArrayMap;

import com.android.internal.infra.AndroidFuture;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.Objects;

@SuppressWarnings("rawtypes") // IShortcutService's ParceledListSlices, which its AIDL leaves untyped.
public final class LocalShortcutService extends IShortcutService.Default {
    /** ShortcutService's DEFAULT_MAX_SHORTCUTS_PER_ACTIVITY. */
    private static final int MAX_SHORTCUTS_PER_ACTIVITY = 15;
    /** ShortcutService's DEFAULT_MAX_UPDATES_PER_INTERVAL: an app in front keeps all of them. */
    private static final int MAX_UPDATES_PER_INTERVAL = 10;
    /** ShortcutService's DEFAULT_MAX_ICON_DIMENSION_DP. */
    private static final int MAX_ICON_DIMENSION_DP = 96;

    /** ShortcutPackage's order: rank, then a changed rank first, then the order in the call. */
    private static final Comparator<ShortcutInfo> RANK_ORDER = (a, b) -> {
        if (a.getRank() != b.getRank()) {
            return a.getRank() < b.getRank() ? -1 : 1;
        }
        if (a.isRankChanged() != b.isRankChanged()) {
            return a.isRankChanged() ? -1 : 1;
        }
        return Integer.compare(a.getImplicitRank(), b.getImplicitRank());
    };

    private final Binder mBinder = new Binder();
    /** The dynamic shortcuts by ID; under the service's lock. */
    private final ArrayMap<String, ShortcutInfo> mShortcuts = new ArrayMap<>();

    private LocalShortcutService() {
        mBinder.attachInterface(this, IShortcutService.Stub.DESCRIPTOR);
    }

    /** ServiceManager's maker for "shortcut": the Binder that carries a new service. */
    public static IBinder newBinder() {
        return new LocalShortcutService().asBinder();
    }

    @Override
    public IBinder asBinder() {
        return mBinder;
    }

    /** The app's launcher activity, which a shortcut without one gets, as ShortcutService's fillInDefaultActivity. */
    private static ComponentName launcherActivity(String packageName) {
        final Application application = ActivityThread.currentApplication();
        final Intent launch = application.getPackageManager().getLaunchIntentForPackage(packageName);
        if (launch == null || launch.getComponent() == null) {
            throw new IllegalStateException("Launcher activity not found for package " + packageName);
        }
        return launch.getComponent();
    }

    /**
     * ShortcutService's fixUpIncomingShortcutInfo for a new shortcut: the default activity and the mandatory fields;
     * the service keeps its own copy, dynamic, with only the long-lived flag of those the app gave.
     */
    private static ShortcutInfo incoming(String packageName, ShortcutInfo shortcut, int implicitRank) {
        Objects.requireNonNull(shortcut, "Null shortcut detected");
        if (shortcut.getActivity() == null) {
            shortcut.setActivity(launcherActivity(packageName));
        }
        shortcut.enforceMandatoryFields(false);
        final ShortcutInfo kept = shortcut.clone(0);
        kept.clearFlags(~ShortcutInfo.FLAG_LONG_LIVED);
        kept.addFlags(ShortcutInfo.FLAG_DYNAMIC);
        kept.setImplicitRank(implicitRank);
        if (kept.hasRank()) {
            kept.setRankChanged();
        }
        kept.setTimestamp(System.currentTimeMillis());
        return kept;
    }

    private static List<ShortcutInfo> list(ParceledListSlice shortcuts) {
        @SuppressWarnings("unchecked")
        final List<ShortcutInfo> list = shortcuts.getList();
        return list;
    }

    /** Throws Android's IllegalArgumentException when an activity would have more than the maximum. */
    private static void checkCount(List<ShortcutInfo> shortcuts) {
        final ArrayMap<ComponentName, Integer> counts = new ArrayMap<>();
        for (ShortcutInfo shortcut : shortcuts) {
            final int count = counts.getOrDefault(shortcut.getActivity(), 0) + 1;
            if (count > MAX_SHORTCUTS_PER_ACTIVITY) {
                throw new IllegalArgumentException("Max number of dynamic shortcuts exceeded");
            }
            counts.put(shortcut.getActivity(), count);
        }
    }

    /** The kept shortcuts with the new ones in place of those of the same ID. */
    private List<ShortcutInfo> merged(List<ShortcutInfo> added) {
        final ArrayMap<String, ShortcutInfo> all = new ArrayMap<>(mShortcuts);
        for (ShortcutInfo shortcut : added) {
            all.put(shortcut.getId(), shortcut);
        }
        return new ArrayList<>(all.values());
    }

    /** ShortcutPackage's adjustRanks: each activity's shortcuts numbered from 0 in RANK_ORDER. */
    private void adjustRanks() {
        final ArrayMap<ComponentName, List<ShortcutInfo>> byActivity = new ArrayMap<>();
        for (ShortcutInfo shortcut : mShortcuts.values()) {
            byActivity.computeIfAbsent(shortcut.getActivity(), activity -> new ArrayList<>()).add(shortcut);
        }
        for (List<ShortcutInfo> shortcuts : byActivity.values()) {
            shortcuts.sort(RANK_ORDER);
            for (int rank = 0; rank < shortcuts.size(); rank++) {
                shortcuts.get(rank).setRank(rank);
                shortcuts.get(rank).clearImplicitRankAndRankChangedFlag();
            }
        }
    }

    private void put(List<ShortcutInfo> shortcuts) {
        for (ShortcutInfo shortcut : shortcuts) {
            mShortcuts.put(shortcut.getId(), shortcut);
        }
        adjustRanks();
    }

    @Override
    public synchronized boolean setDynamicShortcuts(String packageName, ParceledListSlice shortcutInfoList,
            int userId) {
        final List<ShortcutInfo> shortcuts = new ArrayList<>();
        for (ShortcutInfo shortcut : list(shortcutInfoList)) {
            shortcuts.add(incoming(packageName, shortcut, shortcuts.size()));
        }
        checkCount(shortcuts);
        mShortcuts.clear();
        put(shortcuts);
        return true;
    }

    @Override
    public synchronized boolean addDynamicShortcuts(String packageName, ParceledListSlice shortcutInfoList,
            int userId) {
        final List<ShortcutInfo> shortcuts = new ArrayList<>();
        for (ShortcutInfo shortcut : list(shortcutInfoList)) {
            shortcuts.add(incoming(packageName, shortcut, shortcuts.size()));
        }
        checkCount(merged(shortcuts));
        put(shortcuts);
        return true;
    }

    @Override
    public synchronized void pushDynamicShortcut(String packageName, ShortcutInfo shortcut, int userId) {
        final ShortcutInfo kept = incoming(packageName, shortcut, 0);
        if (!kept.hasRank()) {
            kept.setRank(0);
        }
        kept.setRankChanged();
        if (!mShortcuts.containsKey(kept.getId())) {
            // At the maximum, the activity's last shortcut makes room.
            final List<ShortcutInfo> same = new ArrayList<>();
            for (ShortcutInfo other : mShortcuts.values()) {
                if (Objects.equals(other.getActivity(), kept.getActivity())) {
                    same.add(other);
                }
            }
            if (same.size() >= MAX_SHORTCUTS_PER_ACTIVITY) {
                same.sort(RANK_ORDER);
                mShortcuts.remove(same.get(same.size() - 1).getId());
            }
        }
        mShortcuts.put(kept.getId(), kept);
        adjustRanks();
    }

    @Override
    public synchronized boolean updateShortcuts(String packageName, ParceledListSlice shortcuts, int userId) {
        for (ShortcutInfo source : list(shortcuts)) {
            final ShortcutInfo target = mShortcuts.get(source.getId());
            if (target == null) {
                continue;
            }
            target.copyNonNullFieldsFrom(source);
            target.setTimestamp(System.currentTimeMillis());
            if (source.hasRank()) {
                target.setRankChanged();
                target.setImplicitRank(0);
            }
        }
        adjustRanks();
        return true;
    }

    @Override
    public synchronized void removeDynamicShortcuts(String packageName, List<String> shortcutIds, int userId) {
        for (String id : Objects.requireNonNull(shortcutIds, "shortcutIds must be provided")) {
            mShortcuts.remove(id);
        }
        adjustRanks();
    }

    @Override
    public synchronized void removeAllDynamicShortcuts(String packageName, int userId) {
        mShortcuts.clear();
    }

    @Override
    public synchronized void disableShortcuts(String packageName, List<String> shortcutIds,
            CharSequence disabledMessage, int disabledMessageResId, int userId) {
        removeDynamicShortcuts(packageName, shortcutIds, userId);
    }

    @Override
    public synchronized ParceledListSlice getShortcuts(String packageName, int matchFlags, int userId) {
        final List<ShortcutInfo> shortcuts = new ArrayList<>();
        if ((matchFlags & ShortcutManager.FLAG_MATCH_DYNAMIC) != 0) {
            for (int i = mShortcuts.size() - 1; i >= 0; i--) {
                shortcuts.add(mShortcuts.valueAt(i).clone(ShortcutInfo.CLONE_REMOVE_FOR_CREATOR));
            }
        }
        return new ParceledListSlice<>(shortcuts);
    }

    @Override
    public int getMaxShortcutCountPerActivity(String packageName, int userId) {
        return MAX_SHORTCUTS_PER_ACTIVITY;
    }

    @Override
    public int getRemainingCallCount(String packageName, int userId) {
        return MAX_UPDATES_PER_INTERVAL;
    }

    @Override
    public int getIconMaxDimensions(String packageName, int userId) {
        final float density = ActivityThread.currentApplication().getResources().getDisplayMetrics().density;
        return Math.round(MAX_ICON_DIMENSION_DP * density);
    }

    @Override
    public boolean isRequestPinItemSupported(int user, int requestType) {
        return false;
    }

    @Override
    public void requestPinShortcut(String packageName, ShortcutInfo shortcut, IntentSender resultIntent, int userId,
            AndroidFuture<String> ret) {
        ret.complete(String.valueOf(false));
    }

    @Override
    public void createShortcutResultIntent(String packageName, ShortcutInfo shortcut, int userId,
            AndroidFuture<Intent> ret) {
        ret.complete(null);
    }

    @Override
    public ParceledListSlice getShareTargets(String packageName, IntentFilter filter, int userId) {
        return new ParceledListSlice<>(new ArrayList<ShortcutManager.ShareShortcutInfo>());
    }
}
