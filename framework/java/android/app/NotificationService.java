/*
 * The notification service (ADR-0045 decisions 3 and 4; gap 1 of the compatibility plan, batch 2):
 * INotificationManager of system_server, local in the app's process, which ServiceManager's table gives as
 * "notification" and NotificationManager asks. It answers as Android 15's NotificationManagerService and its
 * PreferencesHelper do for an app without POST_NOTIFICATIONS, which decision 6 never grants [verified: services-test on
 * apk-runner-35-bare]:
 *   - notifications off: not enabled, importance NONE, a post dropped and none active; not paused, no policy access,
 *     Do Not Disturb off;
 *   - the app's channel groups and channels kept and listed, each a copy, as a parcel through Binder makes one. A
 *     channel created again takes the new name and description, a group when it had none, and a lower importance while
 *     the user changed nothing, and comes back when it was deleted; a new one gets what an app cannot set reset. A
 *     deleted channel stays as deleted, gone from every answer; a deleted group takes its channels along.
 * They live as long as the process, where Android keeps them in its policy file across starts. Every other call is
 * AIDL's Default answer, as for the power service. Not under com.android.minato, whose classes
 * tools/framework-host-check.sh compiles on the host without android.*.
 */
package android.app;

import android.Manifest;
import android.content.pm.PackageManager;
import android.content.pm.ParceledListSlice;
import android.os.Binder;
import android.os.IBinder;
import android.os.Process;
import android.provider.Settings;
import android.service.notification.StatusBarNotification;
import android.text.TextUtils;
import android.util.ArrayMap;

import com.android.internal.util.Preconditions;

import java.util.ArrayList;
import java.util.List;
import java.util.Objects;

@SuppressWarnings("rawtypes") // INotificationManager's lists, which AIDL names raw.
public final class NotificationService extends INotificationManager.Default {
    /** PreferencesHelper's NOTIFICATION_CHANNEL_COUNT_LIMIT and NOTIFICATION_CHANNEL_GROUP_COUNT_LIMIT. */
    private static final int CHANNEL_LIMIT = 5000;
    private static final int GROUP_LIMIT = 6000;

    private final Binder mBinder = new Binder();
    /** The app's channels, deleted ones among them, and its groups, by ID; under the service's lock. */
    private final ArrayMap<String, NotificationChannel> mChannels = new ArrayMap<>();
    private final ArrayMap<String, NotificationChannelGroup> mGroups = new ArrayMap<>();

    private NotificationService() {
        mBinder.attachInterface(this, INotificationManager.Stub.DESCRIPTOR);
    }

    /** ServiceManager's maker for "notification": the Binder that carries a new service. */
    public static IBinder newBinder() {
        return new NotificationService().asBinder();
    }

    @Override
    public IBinder asBinder() {
        return mBinder;
    }

    // Whether the app may notify: POST_NOTIFICATIONS, as PermissionHelper.hasPermission asks.

    @Override
    public boolean areNotificationsEnabled(String pkg) {
        checkSameApp(pkg);
        return hasPermission();
    }

    @Override
    public int getPackageImportance(String pkg) {
        checkSameApp(pkg);
        return hasPermission() ? NotificationManager.IMPORTANCE_DEFAULT : NotificationManager.IMPORTANCE_NONE;
    }

    @Override
    public boolean isPackagePaused(String pkg) {
        Objects.requireNonNull(pkg);
        checkSameApp(pkg);
        return false;
    }

    @Override
    public boolean isNotificationPolicyAccessGranted(String pkg) {
        return false;
    }

    @Override
    public int getZenMode() {
        return Settings.Global.ZEN_MODE_OFF;
    }

    // Posts: dropped, as NotificationManagerService suppresses each of an app without the permission.

    @Override
    public void enqueueNotificationWithTag(String pkg, String opPkg, String tag, int id, Notification notification,
            int userId) {
        checkSameApp(pkg);
    }

    @Override
    public void cancelNotificationWithTag(String pkg, String opPkg, String tag, int id, int userId) {
        checkSameApp(pkg);
    }

    @Override
    public void cancelAllNotifications(String pkg, int userId) {
        checkSameApp(pkg);
    }

    @Override
    public ParceledListSlice getAppActiveNotifications(String callingPkg, int userId) {
        checkSameApp(callingPkg);
        return new ParceledListSlice<StatusBarNotification>(new ArrayList<>());
    }

    // Channels and groups, as PreferencesHelper keeps them for the package.

    @Override
    public void createNotificationChannelGroups(String pkg, ParceledListSlice channelGroupList) {
        checkSameApp(pkg);
        List<?> groups = channelGroupList.getList();
        for (int i = 0; i < groups.size(); i++) {
            NotificationChannelGroup group = (NotificationChannelGroup) groups.get(i);
            Objects.requireNonNull(group);
            createGroup(group.clone());
        }
    }

    private synchronized void createGroup(NotificationChannelGroup group) {
        Objects.requireNonNull(group.getId());
        if (mGroups.size() >= GROUP_LIMIT) {
            throw new IllegalStateException("Limit exceed; cannot create more groups");
        }
        group.setBlocked(false);
        NotificationChannelGroup old = mGroups.get(group.getId());
        if (old != null) {
            // An app cannot change whether the group is blocked, nor what the user locked.
            group.setBlocked(old.isBlocked());
            group.unlockFields(group.getUserLockedFields());
            group.lockFields(old.getUserLockedFields());
        }
        group.setChannels(new ArrayList<>());
        mGroups.put(group.getId(), group);
    }

    @Override
    public void createNotificationChannels(String pkg, ParceledListSlice channelsList) {
        checkSameApp(pkg);
        List<?> channels = channelsList.getList();
        for (int i = 0; i < channels.size(); i++) {
            NotificationChannel channel = (NotificationChannel) channels.get(i);
            Objects.requireNonNull(channel, "channel in list is null");
            createChannel(channel.copy());
        }
    }

    private synchronized void createChannel(NotificationChannel channel) {
        Objects.requireNonNull(channel.getId());
        Preconditions.checkArgument(!TextUtils.isEmpty(channel.getName()));
        Preconditions.checkArgument(channel.getImportance() >= NotificationManager.IMPORTANCE_NONE
                && channel.getImportance() <= NotificationManager.IMPORTANCE_MAX, "Invalid importance level");
        if (channel.getGroup() != null && !mGroups.containsKey(channel.getGroup())) {
            throw new IllegalArgumentException("NotificationChannelGroup doesn't exist");
        }
        if (NotificationChannel.DEFAULT_CHANNEL_ID.equals(channel.getId())) {
            throw new IllegalArgumentException("Reserved id");
        }
        NotificationChannel existing = mChannels.get(channel.getId());
        if (existing != null) {
            if (existing.isDeleted()) {
                existing.setDeleted(false);
                existing.setDeletedTimeMs(-1);
            }
            existing.setName(channel.getName().toString());
            existing.setDescription(channel.getDescription());
            existing.setBlockable(channel.isBlockable());
            if (channel.getGroup() != null && existing.getGroup() == null) {
                existing.setGroup(channel.getGroup());
            }
            // An app may lower the importance while the user has changed nothing.
            if (existing.getUserLockedFields() == 0 && channel.getImportance() < existing.getImportance()) {
                existing.setImportance(channel.getImportance());
            }
            if (existing.getOriginalImportance() == NotificationManager.IMPORTANCE_UNSPECIFIED) {
                existing.setOriginalImportance(channel.getImportance());
            }
            return;
        }
        if (mChannels.size() >= CHANNEL_LIMIT) {
            throw new IllegalStateException("Limit exceed; cannot create more channels");
        }
        // What an app cannot set: the package's defaults, none changed by a user.
        channel.setBypassDnd(false);
        channel.setLockscreenVisibility(NotificationManager.VISIBILITY_NO_OVERRIDE);
        channel.setAllowBubbles(NotificationChannel.DEFAULT_ALLOW_BUBBLE);
        channel.setImportantConversation(false);
        channel.unlockFields(channel.getUserLockedFields());
        channel.setImportanceLockedByCriticalDeviceFunction(false);
        channel.setOriginalImportance(channel.getImportance());
        if (channel.getParentChannelId() != null) {
            Preconditions.checkArgument(mChannels.containsKey(channel.getParentChannelId()),
                    "Tried to create a conversation channel without a preexisting parent");
        }
        mChannels.put(channel.getId(), channel);
    }

    @Override
    public synchronized NotificationChannel getNotificationChannel(String callingPkg, int userId, String pkg,
            String channelId) {
        checkSameApp(callingPkg);
        checkReadsOwn(callingPkg, pkg, userId);
        NotificationChannel channel = mChannels.get(channelId == null ? NotificationChannel.DEFAULT_CHANNEL_ID
                : channelId);
        return channel == null || channel.isDeleted() ? null : channel.copy();
    }

    @Override
    public synchronized ParceledListSlice getNotificationChannels(String callingPkg, String targetPkg, int userId) {
        checkSameApp(callingPkg);
        checkReadsOwn(callingPkg, targetPkg, userId);
        List<NotificationChannel> channels = new ArrayList<>();
        for (int i = 0; i < mChannels.size(); i++) {
            NotificationChannel channel = mChannels.valueAt(i);
            if (!channel.isDeleted()) {
                channels.add(channel.copy());
            }
        }
        return new ParceledListSlice<>(channels);
    }

    @Override
    public void deleteNotificationChannel(String pkg, String channelId) {
        checkSameApp(pkg);
        if (NotificationChannel.DEFAULT_CHANNEL_ID.equals(channelId)) {
            throw new IllegalArgumentException("Cannot delete default channel");
        }
        synchronized (this) {
            NotificationChannel channel = mChannels.get(channelId);
            if (channel != null) {
                delete(channel);
            }
        }
    }

    private static void delete(NotificationChannel channel) {
        if (!channel.isDeleted()) {
            channel.setDeleted(true);
            channel.setDeletedTimeMs(System.currentTimeMillis());
        }
    }

    @Override
    public synchronized ParceledListSlice getNotificationChannelGroups(String pkg) {
        checkSameApp(pkg);
        // Each group with its channels that are not deleted, the empty ones too.
        ArrayMap<String, NotificationChannelGroup> groups = new ArrayMap<>();
        for (int i = 0; i < mChannels.size(); i++) {
            NotificationChannel channel = mChannels.valueAt(i);
            if (!channel.isDeleted() && channel.getGroup() != null && mGroups.containsKey(channel.getGroup())) {
                NotificationChannelGroup group = groups.get(channel.getGroup());
                if (group == null) {
                    group = withoutChannels(mGroups.get(channel.getGroup()));
                    groups.put(channel.getGroup(), group);
                }
                group.addChannel(channel.copy());
            }
        }
        for (int i = 0; i < mGroups.size(); i++) {
            if (!groups.containsKey(mGroups.keyAt(i))) {
                groups.put(mGroups.keyAt(i), withoutChannels(mGroups.valueAt(i)));
            }
        }
        return new ParceledListSlice<>(new ArrayList<>(groups.values()));
    }

    @Override
    public synchronized NotificationChannelGroup getNotificationChannelGroup(String pkg, String channelGroupId) {
        checkSameApp(pkg);
        if (channelGroupId == null || !mGroups.containsKey(channelGroupId)) {
            return null;
        }
        NotificationChannelGroup group = withoutChannels(mGroups.get(channelGroupId));
        for (int i = 0; i < mChannels.size(); i++) {
            NotificationChannel channel = mChannels.valueAt(i);
            if (!channel.isDeleted() && channelGroupId.equals(channel.getGroup())) {
                group.addChannel(channel.copy());
            }
        }
        return group;
    }

    @Override
    public synchronized void deleteNotificationChannelGroup(String pkg, String channelGroupId) {
        checkSameApp(pkg);
        if (TextUtils.isEmpty(channelGroupId) || mGroups.remove(channelGroupId) == null) {
            return;
        }
        for (int i = 0; i < mChannels.size(); i++) {
            if (channelGroupId.equals(mChannels.valueAt(i).getGroup())) {
                delete(mChannels.valueAt(i));
            }
        }
    }

    private static NotificationChannelGroup withoutChannels(NotificationChannelGroup group) {
        NotificationChannelGroup copy = group.clone();
        copy.setChannels(new ArrayList<>());
        return copy;
    }

    /** NotificationManagerService's checkCallerIsSystemOrSameApp, for the one app of the process. */
    private static void checkSameApp(String pkg) {
        if (!Objects.equals(pkg, ActivityThread.currentPackageName())) {
            throw new SecurityException("Package " + pkg + " is not owned by uid " + Process.myUid());
        }
    }

    /** getConversationNotificationChannel's check: an app reads its own channels, and no other user's. */
    private static void checkReadsOwn(String callingPkg, String targetPkg, int userId) {
        if (!Objects.equals(callingPkg, targetPkg) || userId != Process.myUserHandle().getIdentifier()) {
            throw new SecurityException("Pkg " + callingPkg + " cannot read channels for " + targetPkg + " in "
                    + userId);
        }
    }

    private static boolean hasPermission() {
        Application app = ActivityThread.currentApplication();
        return app != null && app.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                == PackageManager.PERMISSION_GRANTED;
    }
}
