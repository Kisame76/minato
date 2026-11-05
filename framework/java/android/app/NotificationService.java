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
 * They live as long as the process, where Android keeps them in its policy file across starts. Toasts (ADR-0041 gap 4
 * (b4)) go through the queue ToastQueue keeps as NotificationManagerService does, on the main thread: a text toast is
 * drawn by ToastPresenter in the app's process, where Android's SystemUI draws it, and a toast with a view of its own
 * by the app's Toast.TN; each shows in a requested window of type TYPE_TOAST with a window token of the service's,
 * which WindowManagerGlobal takes. Every other call is AIDL's Default answer, as for the power service. Not under com.android.minato, whose classes
 * tools/framework-host-check.sh compiles on the host without android.*.
 */
package android.app;

import android.Manifest;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ParceledListSlice;
import android.content.res.Resources;
import android.os.Binder;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Process;
import android.os.RemoteException;
import android.provider.Settings;
import android.service.notification.StatusBarNotification;
import android.text.TextUtils;
import android.util.ArrayMap;
import android.util.Log;
import android.view.View;
import android.widget.ToastPresenter;

import com.android.internal.util.Preconditions;
import com.android.minato.app.ToastQueue;
import com.android.minato.notifications.ActiveNotifications;
import com.android.minato.notifications.NotificationText;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Objects;
import java.util.Set;

@SuppressWarnings("rawtypes") // INotificationManager's lists, which AIDL names raw.
public final class NotificationService extends INotificationManager.Default {
    /** PreferencesHelper's NOTIFICATION_CHANNEL_COUNT_LIMIT and NOTIFICATION_CHANNEL_GROUP_COUNT_LIMIT. */
    private static final int CHANNEL_LIMIT = 5000;
    private static final int GROUP_LIMIT = 6000;

    private final Binder mBinder = new Binder();
    /** The app's channels, deleted ones among them, and its groups, by ID; under the service's lock. */
    private final ArrayMap<String, NotificationChannel> mChannels = new ArrayMap<>();
    private final ArrayMap<String, NotificationChannelGroup> mGroups = new ArrayMap<>();
    /** The app's active notifications (ADR-0074 decision 2); under the service's lock. */
    private final ActiveNotifications<StatusBarNotification> mActive = new ActiveNotifications<>();

    /** NotificationManagerService's tag. */
    private static final String TAG = "NotificationService";
    /** The window tokens of the toasts that show, which a window of TYPE_TOAST must name (gap 4 (b4)). */
    private static final Set<IBinder> sToastWindows = Collections.synchronizedSet(new HashSet<>());
    /** The toast queue and what each queued toast shows, by the app's token; under mToastLock. */
    private final Object mToastLock = new Object();
    private final ArrayMap<IBinder, ToastContent> mToastContents = new ArrayMap<>();
    private final Handler mMain = new Handler(Looper.getMainLooper());
    private final ToastQueue mToasts = new ToastQueue(new ToastHost());

    /** The process's service, for the host's taps; the last one made. */
    private static volatile NotificationService sCurrent;

    private NotificationService() {
        mBinder.attachInterface(this, INotificationManager.Stub.DESCRIPTOR);
        sCurrent = this;
    }

    /**
     * The host's tap on the notification of `id` whose tag hashes to `tagHash` (APKRT_EVENT_NOTIFICATION_TAP, ADR-0074
     * decision 4), on the main thread: its content PendingIntent is sent, as SystemUI sends it, and with
     * FLAG_AUTO_CANCEL the notification goes. A notification no longer active is no tap's.
     */
    static void onHostTap(int id, long tagHash) {
        final NotificationService service = sCurrent;
        if (service == null) {
            return;
        }
        final StatusBarNotification shown;
        synchronized (service) {
            shown = service.mActive.find(id, tagHash);
        }
        if (shown == null) {
            Log.w(TAG, "a tap on a notification that is not active: id=" + id);
            return;
        }
        final Notification notification = shown.getNotification();
        if (notification.contentIntent != null) {
            try {
                notification.contentIntent.send();
            } catch (PendingIntent.CanceledException e) {
                Log.w(TAG, "the tapped notification's content intent was cancelled");
            }
        }
        if ((notification.flags & Notification.FLAG_AUTO_CANCEL) != 0) {
            service.cancelNotificationWithTag(shown.getPackageName(), shown.getOpPkg(), shown.getTag(),
                    shown.getId(), 0);
        }
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

    // Posts (ADR-0074 decision 2): without POST_NOTIFICATIONS dropped, as NotificationManagerService suppresses each,
    // and on a channel the app never made dropped too, as Android 15 does [measured: notify-test-granted]. Else kept
    // as active, Android's GroupHelper summary with them (ActiveNotifications), and shown by the host on iOS.

    @Override
    public synchronized void enqueueNotificationWithTag(String pkg, String opPkg, String tag, int id,
            Notification notification, int userId) {
        checkSameApp(pkg);
        if (notification == null || !hasPermission()) {
            return;
        }
        final NotificationChannel channel = mChannels.get(notification.getChannelId());
        if (channel == null || channel.isDeleted()) {
            Log.e(TAG, "No Channel found for pkg=" + pkg + ", channelId=" + notification.getChannelId() + ", id=" + id
                    + ", tag=" + tag);
            return;
        }
        mActive.post(tag, id, notification.getGroup(), new StatusBarNotification(pkg, opPkg, id, tag,
                Process.myUid(), Process.myPid(), notification, Process.myUserHandle(), null,
                System.currentTimeMillis()));
        final CharSequence big = notification.extras.getCharSequence(Notification.EXTRA_BIG_TEXT);
        final CharSequence text = big != null ? big : notification.extras.getCharSequence(Notification.EXTRA_TEXT);
        host(NotificationText.post(id, tag, channel.getId(), channel.getImportance(), channel.getSound() != null,
                notification.when, notification.getGroup(),
                notification.extras.getCharSequence(Notification.EXTRA_TITLE),
                notification.extras.getCharSequence(Notification.EXTRA_SUB_TEXT), startIntent(pkg, notification),
                text));
    }

    /**
     * The content intent for a cold tap (ADR-0074 decision 4): Intent.toUri of a PendingIntent the app made for one of
     * its own activities; null for any other, so that the tap only opens the app. The host and ActivityThread check it
     * again before the start.
     */
    private static String startIntent(String pkg, Notification notification) {
        final PendingIntent content = notification.contentIntent;
        if (content == null || !content.isActivity() || !pkg.equals(content.getCreatorPackage())) {
            return null;
        }
        // As system_server reads its PendingIntentRecord: PendingIntent.getIntent needs a permission apps lack.
        final Intent intent = LocalActivityManagerService.instance().pendingIntentIntent(content.getTarget());
        if (intent == null || intent.getComponent() == null || !pkg.equals(intent.getComponent().getPackageName())) {
            return null;
        }
        return intent.toUri(Intent.URI_INTENT_SCHEME);
    }

    @Override
    public synchronized void cancelNotificationWithTag(String pkg, String opPkg, String tag, int id, int userId) {
        checkSameApp(pkg);
        mActive.cancel(tag, id);
        if (hasPermission()) {
            host(NotificationText.cancel(id, tag));
        }
    }

    @Override
    public synchronized void cancelAllNotifications(String pkg, int userId) {
        checkSameApp(pkg);
        mActive.cancelAll();
        if (hasPermission()) {
            host(NotificationText.cancelAll());
        }
    }

    @Override
    public synchronized ParceledListSlice getAppActiveNotifications(String callingPkg, int userId) {
        checkSameApp(callingPkg);
        final List<StatusBarNotification> active = new ArrayList<>();
        for (ActiveNotifications.Entry<StatusBarNotification> entry : mActive.entries()) {
            active.add(entry.value != null ? entry.value : new StatusBarNotification(callingPkg, callingPkg, entry.id,
                    entry.tag, Process.myUid(), Process.myPid(), new Notification(), Process.myUserHandle(), null,
                    System.currentTimeMillis()));
        }
        return new ParceledListSlice<StatusBarNotification>(active);
    }

    /** The host's side of a post or cancel; one the session refuses (no grant in the runtime) stays in the app. */
    private static void host(String text) {
        final int result = ActivityThread.requestNotification(text);
        if (result != 0) {
            Log.w(TAG, "the host did not take a notification request: " + result);
        }
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
        final int before = mChannels.size();
        for (int i = 0; i < channels.size(); i++) {
            NotificationChannel channel = (NotificationChannel) channels.get(i);
            Objects.requireNonNull(channel, "channel in list is null");
            createChannel(channel.copy());
        }
        if (mChannels.size() > before) {
            // As Android 13's PermissionPolicyService: an app below targetSdk 33 is asked at its first channel.
            ActivityThread.promptForNotificationsIfDue();
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

    /** A queued toast's text and callback, or the app's TN, and once it shows its window token. */
    private static final class ToastContent {
        final CharSequence text;
        final ITransientNotificationCallback textCallback;
        final ITransientNotification view;
        IBinder windowToken;
        ToastPresenter presenter;

        ToastContent(CharSequence text, ITransientNotificationCallback textCallback, ITransientNotification view) {
            this.text = text;
            this.textCallback = textCallback;
            this.view = view;
        }
    }

    /**
     * @hide WindowManagerGlobal's, at a window of TYPE_TOAST: whether `token` is a toast's window token that the
     * service gave, as WMS's addWindow finds the notification service's toast token.
     */
    public static boolean isToastWindowToken(IBinder token) {
        return token != null && sToastWindows.contains(token);
    }

    @Override
    public boolean enqueueTextToast(String pkg, IBinder token, CharSequence text, int duration, boolean isUiContext,
            int displayId, ITransientNotificationCallback callback) {
        return enqueueToast(pkg, token, text, callback, null, duration);
    }

    @Override
    public boolean enqueueToast(String pkg, IBinder token, ITransientNotification callback, int duration,
            boolean isUiContext, int displayId) {
        return enqueueToast(pkg, token, null, null, callback, duration);
    }

    /** NotificationManagerService's enqueueToast, for the one display: the toast is queued, true, or refused. */
    private boolean enqueueToast(String pkg, IBinder token, CharSequence text,
            ITransientNotificationCallback textCallback, ITransientNotification view, int duration) {
        if (pkg == null || (text == null && view == null) || (text != null && view != null) || token == null) {
            Log.e(TAG, "Not enqueuing toast. pkg=" + pkg + " text=" + text + " callback=" + view + " token="
                    + token);
            return false;
        }
        checkSameApp(pkg);
        synchronized (mToastLock) {
            final boolean known = mToastContents.containsKey(token);
            if (!known) {
                mToastContents.put(token, new ToastContent(text, textCallback, view));
            }
            final boolean queued = mToasts.enqueue(pkg, token, text != null, duration);
            if (!queued && !known) {
                mToastContents.remove(token);
            }
            return queued;
        }
    }

    @Override
    public void cancelToast(String pkg, IBinder token) {
        if (pkg == null || token == null) {
            Log.e(TAG, "Not cancelling notification. pkg=" + pkg + " token=" + token);
            return;
        }
        synchronized (mToastLock) {
            mToasts.cancel(pkg, token);
        }
    }

    @Override
    public void finishToken(String pkg, IBinder token) {
        // The window token goes with the toast's hide (ToastHost.hide).
    }

    /** What the toast queue asks: the toast's show and hide, its duration's end and the app's foreground. */
    private final class ToastHost implements ToastQueue.Host {
        @Override
        public boolean show(ToastQueue.Toast toast) {
            final ToastContent content = mToastContents.get((IBinder) toast.token);
            final Context context = ActivityThread.currentApplication();
            if (content == null || context == null) {
                return false;
            }
            content.windowToken = new Binder();
            sToastWindows.add(content.windowToken);
            if (content.view != null) {
                try {
                    content.view.show(content.windowToken);
                    return true;
                } catch (RemoteException e) {
                    sToastWindows.remove(content.windowToken);
                    return false;
                }
            }
            // As SystemUI's ToastUI shows a text toast: its view at the default gravity and offset.
            final IBinder token = (IBinder) toast.token;
            final IBinder windowToken = content.windowToken;
            final int duration = toast.duration();
            mMain.post(() -> {
                final Resources res = context.getResources();
                content.presenter = new ToastPresenter(context, null, NotificationService.this, toast.pkg);
                final View view = ToastPresenter.getTextToastView(context, content.text);
                content.presenter.show(view, token, windowToken, duration,
                        res.getInteger(com.android.internal.R.integer.config_toastDefaultGravity), 0,
                        res.getDimensionPixelSize(com.android.internal.R.dimen.toast_y_offset), 0, 0,
                        content.textCallback);
            });
            return true;
        }

        @Override
        public void hide(ToastQueue.Toast toast) {
            final ToastContent content = mToastContents.remove((IBinder) toast.token);
            if (content == null || content.windowToken == null) {
                return;
            }
            sToastWindows.remove(content.windowToken);
            if (content.view != null) {
                try {
                    content.view.hide();
                } catch (RemoteException e) {
                    Log.w(TAG, "Object died trying to hide custom toast " + toast.token + " in package "
                            + toast.pkg);
                }
                return;
            }
            mMain.post(() -> {
                if (content.presenter != null && content.presenter.getView() != null) {
                    content.presenter.hide(content.textCallback);
                }
            });
        }

        @Override
        public void schedule(ToastQueue.Toast toast, long delay) {
            mMain.postDelayed(() -> {
                synchronized (mToastLock) {
                    mToasts.durationReached(toast);
                }
            }, toast, delay);
        }

        @Override
        public void unschedule(ToastQueue.Toast toast) {
            mMain.removeCallbacksAndMessages(toast);
        }

        @Override
        public boolean foreground() {
            final ActivityThread thread = ActivityThread.currentActivityThread();
            return thread != null && thread.isAppForeground();
        }

        @Override
        public void log(String line) {
            Log.w(TAG, line);
        }
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
