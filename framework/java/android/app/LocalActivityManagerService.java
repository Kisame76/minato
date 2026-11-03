/*
 * The activity manager service of Minato's one app process (ADR-0045 decision 5, step d2): what system_server's
 * ActivityManagerService answers an app's receivers and broadcasts with, answered in the process, as ServiceManager's
 * "activity". Registration follows registerReceiverWithFeatureTraced at android-15.0.0_r1: both export flags throw
 * IllegalArgumentException; a receiver of a target SDK of 34 or later must name one of them unless every action of its
 * filter is a protected broadcast of framework-res's manifest, else SecurityException, as the compat change
 * DYNAMIC_RECEIVER_EXPLICIT_EXPORT_REQUIRED (@EnabledSince UPSIDE_DOWN_CAKE) has it. Registration answers the first
 * sticky broadcast the filter matches, and a receiver gets each of them at once, as its initial sticky broadcast;
 * a null receiver only looks them up. The sticky broadcasts are the system's: the battery's ACTION_BATTERY_CHANGED
 * (LocalBatteryService), kept as the last of its kind, as ActivityManagerService keeps one per filterEquals. A
 * broadcast reaches the registered filters that match it, by
 * priority and then in registration order, each receiver with its own copy of the intent and of the result: an
 * unordered one at once and assumed delivered, as Android 14's queue does for registered receivers; an ordered one
 * receiver by receiver, the next after the one before finishes (finishReceiver, from the app thread, as
 * BroadcastReceiver.PendingResult sends it), until the end or an abort, then its final receiver, also when nothing
 * matched. Broadcasts reach the process one after the other, as Android's per-process queue delivers them: one waits
 * while an ordered one before it waits for a receiver. The one app is sender and receiver alike, so exported or not,
 * every filter matches it, and permissions are the app's own. PendingIntents (step d3) are LocalPendingIntents'. Not
 * yet written, each answered as the generated methods are, with a line once and Android's default: the app's own
 * sticky broadcasts, broadcast options and app ops, manifest receivers (ADR-0045 decision 5), a PendingIntent's intent,
 * tag
 * and target, StrictMode's dropbox, instrumentation and locus contexts that framework.jar also asks the activity
 * manager about. Started and bound services (steps SV2 and SV3) are LocalActiveServices'.
 */
package android.app;

import android.content.ComponentName;
import android.content.Context;
import android.content.IIntentReceiver;
import android.content.IIntentSender;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.LocusId;
import android.content.pm.PackageManager;
import android.content.pm.ParceledListSlice;
import android.content.pm.ResolveInfo;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Process;
import android.os.RemoteCallback;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.StrictMode;
import android.os.UserHandle;
import android.util.Slog;

import com.android.internal.os.IResultReceiver;
import com.android.minato.services.Unwritten;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;

public final class LocalActivityManagerService extends IActivityManager.Stub {
    private static final String TAG = "ActivityManager";
    /** ActivityManagerService's MAX_RECEIVERS_ALLOWED_PER_APP. */
    private static final int MAX_RECEIVERS_ALLOWED_PER_APP = 1000;

    private final Object mLock = new Object();
    /** The process's PendingIntents (step d3). */
    private final LocalPendingIntents mPendingIntents = new LocalPendingIntents(this);
    /** The process's started services (step SV2). */
    private final LocalActiveServices mServices = new LocalActiveServices();
    /** The registered receivers by their IIntentReceiver's Binder; under mLock, as the two below. */
    private final HashMap<IBinder, ReceiverList> mReceivers = new HashMap<>();
    /** Every registered filter in registration order, which ties of priority keep. */
    private final ArrayList<BroadcastFilter> mFilters = new ArrayList<>();
    /** The broadcasts for the process, the first delivering. */
    private final ArrayDeque<Broadcast> mQueue = new ArrayDeque<>();
    /** The system's sticky broadcasts, one per filterEquals; under mLock. */
    private final ArrayList<Intent> mStickies = new ArrayList<>();

    /** A registered receiver and its filters, as ActivityManagerService's ReceiverList. */
    private static final class ReceiverList {
        final IIntentReceiver mReceiver;
        final ArrayList<BroadcastFilter> mFilters = new ArrayList<>();

        ReceiverList(IIntentReceiver receiver) {
            mReceiver = receiver;
        }
    }

    /** A registered filter: a copy of the app's, as Binder's parcel gives the service one. */
    private static final class BroadcastFilter {
        final IntentFilter mFilter;
        final ReceiverList mList;
        final String mPermission;

        BroadcastFilter(IntentFilter filter, ReceiverList list, String permission) {
            mFilter = new IntentFilter(filter);
            mList = list;
            mPermission = permission;
        }
    }

    /** A broadcast and, for an ordered one, its result and its next receiver. */
    private static final class Broadcast {
        final Intent mIntent;
        final List<BroadcastFilter> mReceivers;
        final boolean mOrdered;
        final IIntentReceiver mResultTo;
        int mNext;
        int mResultCode;
        String mResultData;
        Bundle mResultExtras;
        boolean mAborted;
        /** The receiver an ordered broadcast waits for, null while it waits for none. */
        ReceiverList mWaitingFor;
        /** A sticky broadcast a receiver gets as it registers (BroadcastReceiver.isInitialStickyBroadcast). */
        boolean mInitialSticky;

        Broadcast(Intent intent, List<BroadcastFilter> receivers, boolean ordered, IIntentReceiver resultTo, int code,
                String data, Bundle extras) {
            mIntent = intent;
            mReceivers = receivers;
            mOrdered = ordered;
            mResultTo = resultTo;
            mResultCode = code;
            mResultData = data;
            mResultExtras = extras;
        }
    }

    private LocalActivityManagerService() {
    }

    /** ServiceManager's maker for "activity": the Binder that carries a new service. */
    public static IBinder newBinder() {
        return new LocalActivityManagerService();
    }

    /** The process's service, which ServiceManager's "activity" carries. */
    static LocalActivityManagerService instance() {
        return (LocalActivityManagerService) ServiceManager.getService(Context.ACTIVITY_SERVICE);
    }

    @Override
    public Intent registerReceiverWithFeature(IApplicationThread caller, String callerPackage, String callingFeatureId,
            String receiverId, IIntentReceiver receiver, IntentFilter filter, String requiredPermission, int userId,
            int flags) {
        // Whether every action of the filter is a protected broadcast; a filter without one is not.
        boolean onlyProtectedBroadcasts = true;
        Iterator<String> actions = filter.actionsIterator();
        if (actions == null) {
            onlyProtectedBroadcasts = false;
        } else {
            while (actions.hasNext() && onlyProtectedBroadcasts) {
                onlyProtectedBroadcasts = ManifestPackage.isProtectedBroadcast(actions.next());
            }
        }
        boolean explicitExportStateDefined =
                (flags & (Context.RECEIVER_EXPORTED | Context.RECEIVER_NOT_EXPORTED)) != 0;
        if ((flags & Context.RECEIVER_EXPORTED) != 0 && (flags & Context.RECEIVER_NOT_EXPORTED) != 0) {
            throw new IllegalArgumentException(
                    "Receiver can't specify both RECEIVER_EXPORTED and RECEIVER_NOT_EXPORTED" + "flag");
        }
        boolean requireExplicitFlagForDynamicReceivers = targetSdk() >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE;
        if ((flags & Context.RECEIVER_VISIBLE_TO_INSTANT_APPS) != 0 && (flags & Context.RECEIVER_NOT_EXPORTED) != 0
                && requireExplicitFlagForDynamicReceivers) {
            throw new IllegalArgumentException(
                    "Receiver can't specify both RECEIVER_VISIBLE_TO_INSTANT_APPS and " + "RECEIVER_NOT_EXPORTED flag");
        }
        if (!onlyProtectedBroadcasts && receiver != null && requireExplicitFlagForDynamicReceivers
                && !explicitExportStateDefined) {
            throw new SecurityException(callerPackage + ": One of RECEIVER_EXPORTED or "
                    + "RECEIVER_NOT_EXPORTED should be specified when a receiver "
                    + "isn't being registered exclusively for system broadcasts");
        }
        if (filter.hasAction(Intent.ACTION_BATTERY_CHANGED)) {
            LocalBatteryService.prepareSticky();
        }
        // ConnectivityManager's deprecated CONNECTIVITY_ACTION and WifiManager's WIFI_STATE_CHANGED_ACTION.
        if (filter.hasAction("android.net.conn.CONNECTIVITY_CHANGE")
                || filter.hasAction(android.net.wifi.WifiManager.WIFI_STATE_CHANGED_ACTION)) {
            com.android.internal.minato.LocalNetwork.prepareSticky();
        }
        Intent sticky;
        synchronized (mLock) {
            // The matching sticky broadcasts, of which registration answers the first.
            List<Intent> stickies = new ArrayList<>();
            for (Intent kept : mStickies) {
                if (filter.match(kept.getAction(), kept.getType(), kept.getScheme(), kept.getData(),
                        kept.getCategories(), TAG) >= 0) {
                    stickies.add(kept);
                }
            }
            sticky = stickies.isEmpty() ? null : new Intent(stickies.get(0));
            if (receiver == null) {
                return sticky;
            }
            ReceiverList list = mReceivers.get(receiver.asBinder());
            if (list == null) {
                if (mReceivers.size() >= MAX_RECEIVERS_ALLOWED_PER_APP) {
                    throw new IllegalStateException("Too many receivers, total of " + mReceivers.size()
                            + ", registered for pid: " + Process.myPid() + ", callerPackage: " + callerPackage);
                }
                list = new ReceiverList(receiver);
                mReceivers.put(receiver.asBinder(), list);
            }
            for (BroadcastFilter registered : list.mFilters) {
                if (IntentFilter.filterEquals(registered.mFilter, filter)) {
                    Slog.w(TAG, "Receiver with filter " + filter + " already registered for pid " + Process.myPid()
                            + ", callerPackage is " + callerPackage);
                    return sticky;
                }
            }
            BroadcastFilter added = new BroadcastFilter(filter, list, requiredPermission);
            list.mFilters.add(added);
            mFilters.add(added);
            for (Intent kept : stickies) {
                Broadcast initial = new Broadcast(kept, List.of(added), false, null, -1, null, null);
                initial.mInitialSticky = true;
                mQueue.addLast(initial);
            }
            if (stickies.isEmpty()) {
                return sticky;
            }
        }
        dispatch();
        return sticky;
    }

    /**
     * @hide A broadcast of the system's (LocalBatteryService): as the app's, with the flag ActivityManagerService adds
     * to every broadcast.
     */
    void broadcastFromSystem(Intent intent) {
        Intent sent = new Intent(intent);
        sent.addFlags(Intent.FLAG_EXCLUDE_STOPPED_PACKAGES);
        broadcastIntentWithFeature(null, null, sent, null, null, -1, null, null, null, null, null, -1, null, false,
                false, UserHandle.myUserId());
    }

    /** @hide A sticky broadcast of the system's: kept in place of the one it filterEquals, then sent. */
    void broadcastStickyFromSystem(Intent intent) {
        Intent sent = new Intent(intent);
        sent.addFlags(Intent.FLAG_EXCLUDE_STOPPED_PACKAGES);
        synchronized (mLock) {
            mStickies.removeIf(sent::filterEquals);
            mStickies.add(sent);
        }
        broadcastIntentWithFeature(null, null, sent, null, null, -1, null, null, null, null, null, -1, null, false,
                false, UserHandle.myUserId());
    }

    /**
     * Removes the receiver and its filters, and nothing more: a broadcast skips it at its turn (dispatch), and one that
     * waits for it waits on, as at android-15.0.0_r1, whose queue never sets the ReceiverList.curBroadcast that
     * unregisterReceiverTraced finishes through. The delivery already posted to it acknowledges itself: LoadedApk's Args
     * finds its dispatcher forgotten, InnerReceiver its dispatcher collected.
     */
    @Override
    public void unregisterReceiver(IIntentReceiver receiver) {
        synchronized (mLock) {
            ReceiverList list = mReceivers.remove(receiver.asBinder());
            if (list != null) {
                mFilters.removeAll(list.mFilters);
            }
        }
    }

    @Override
    public int broadcastIntentWithFeature(IApplicationThread caller, String callingFeatureId, Intent intent,
            String resolvedType, IIntentReceiver resultTo, int resultCode, String resultData, Bundle map,
            String[] requiredPermissions, String[] excludePermissions, String[] excludePackages, int appOp,
            Bundle options, boolean serialized, boolean sticky, int userId) {
        if (sticky) {
            Unwritten.log("android.app.IActivityManager.broadcastIntentWithFeature for a sticky broadcast",
                    "an unsticky broadcast");
        }
        Intent sent = new Intent(intent);
        List<BroadcastFilter> receivers = new ArrayList<>();
        synchronized (mLock) {
            // Registered receivers get no explicit broadcast, and one for another package reaches none of the app's.
            String target = sent.getPackage();
            if (sent.getComponent() == null && (target == null || target.equals(packageName()))
                    && delivered(requiredPermissions, excludePermissions, excludePackages)) {
                for (BroadcastFilter filter : mFilters) {
                    if (filter.mFilter.match(sent.getAction(), resolvedType, sent.getScheme(), sent.getData(),
                            sent.getCategories(), TAG) >= 0 && (filter.mPermission == null || held(filter.mPermission))) {
                        receivers.add(filter);
                    }
                }
            }
            // By priority; a stable sort keeps the registration order of a tie.
            receivers.sort(Comparator.comparingInt((BroadcastFilter filter) -> filter.mFilter.getPriority()).reversed());
            mQueue.addLast(new Broadcast(sent, receivers, serialized, resultTo, resultCode, resultData, map));
        }
        dispatch();
        return ActivityManager.BROADCAST_SUCCESS;
    }

    @Override
    public void finishReceiver(IBinder who, int resultCode, String resultData, Bundle map, boolean abortBroadcast,
            int flags) {
        synchronized (mLock) {
            Broadcast head = mQueue.peekFirst();
            if (head == null || head.mWaitingFor == null) {
                Slog.w(TAG, "finishReceiver called but no pending broadcasts");
                return;
            }
            head.mWaitingFor = null;
            head.mResultCode = resultCode;
            head.mResultData = resultData;
            head.mResultExtras = map;
            if (abortBroadcast && (head.mIntent.getFlags() & Intent.FLAG_RECEIVER_NO_ABORT) == 0) {
                head.mAborted = true;
            }
        }
        dispatch();
    }

    /**
     * Delivers what the queue lets through now: an unordered broadcast to all its receivers at once, then its final
     * receiver; an ordered one's next receiver, which it then waits for, or, at its end or after an abort, its final
     * receiver. The receivers' handlers run them; only the posts happen here, outside the lock.
     */
    private void dispatch() {
        List<Runnable> deliveries = new ArrayList<>();
        synchronized (mLock) {
            while (!mQueue.isEmpty()) {
                Broadcast broadcast = mQueue.peekFirst();
                if (broadcast.mWaitingFor != null) {
                    break;
                }
                if (broadcast.mOrdered && !broadcast.mAborted) {
                    BroadcastFilter next = null;
                    while (next == null && broadcast.mNext < broadcast.mReceivers.size()) {
                        BroadcastFilter filter = broadcast.mReceivers.get(broadcast.mNext++);
                        if (mReceivers.get(filter.mList.mReceiver.asBinder()) == filter.mList
                                && filter.mList.mFilters.contains(filter)) {
                            next = filter;
                        }
                    }
                    if (next != null) {
                        broadcast.mWaitingFor = next.mList;
                        deliveries.add(delivery(next.mList.mReceiver, broadcast, true, false));
                        break;
                    }
                } else if (!broadcast.mOrdered) {
                    for (BroadcastFilter filter : broadcast.mReceivers) {
                        if (mReceivers.get(filter.mList.mReceiver.asBinder()) == filter.mList) {
                            deliveries.add(delivery(filter.mList.mReceiver, broadcast, false, true));
                        }
                    }
                }
                if (broadcast.mResultTo != null) {
                    deliveries.add(delivery(broadcast.mResultTo, broadcast, false, true));
                }
                mQueue.pollFirst();
            }
        }
        for (Runnable delivery : deliveries) {
            delivery.run();
        }
    }

    /** The delivery of `broadcast` to `receiver` with copies of its intent and its result as they are now. */
    private static Runnable delivery(IIntentReceiver receiver, Broadcast broadcast, boolean ordered,
            boolean assumeDelivered) {
        Intent intent = new Intent(broadcast.mIntent);
        int code = broadcast.mResultCode;
        String data = broadcast.mResultData;
        Bundle extras = broadcast.mResultExtras != null ? new Bundle(broadcast.mResultExtras) : null;
        boolean sticky = broadcast.mInitialSticky;
        String sender = packageName();
        return () -> {
            try {
                if (receiver instanceof LoadedApk.ReceiverDispatcher.InnerReceiver) {
                    ((LoadedApk.ReceiverDispatcher.InnerReceiver) receiver).performReceive(intent, code, data, extras,
                            ordered, sticky, assumeDelivered, UserHandle.myUserId(), Process.myUid(), sender);
                } else {
                    receiver.performReceive(intent, code, data, extras, ordered, sticky, UserHandle.myUserId());
                }
            } catch (RemoteException e) {
                Slog.w(TAG, "Failure sending broadcast " + intent, e);
            }
        };
    }

    /** Whether the app receives a broadcast that requires and excludes these permissions and packages. */
    private static boolean delivered(String[] requiredPermissions, String[] excludePermissions,
            String[] excludePackages) {
        if (requiredPermissions != null) {
            for (String permission : requiredPermissions) {
                if (!held(permission)) {
                    return false;
                }
            }
        }
        if (excludePermissions != null) {
            for (String permission : excludePermissions) {
                if (held(permission)) {
                    return false;
                }
            }
        }
        if (excludePackages != null) {
            for (String excluded : excludePackages) {
                if (excluded.equals(packageName())) {
                    return false;
                }
            }
        }
        return true;
    }

    private static boolean held(String permission) {
        Application app = ActivityThread.currentApplication();
        return app != null && app.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED;
    }

    private static String packageName() {
        Application app = ActivityThread.currentApplication();
        return app != null ? app.getPackageName() : null;
    }

    private static int targetSdk() {
        Application app = ActivityThread.currentApplication();
        return app != null ? app.getApplicationInfo().targetSdkVersion : Build.VERSION_CODES.CUR_DEVELOPMENT;
    }

    // PendingIntents (step d3): LocalPendingIntents', as PendingIntentController's.

    @Override
    public IIntentSender getIntentSenderWithFeature(int type, String packageName, String featureId, IBinder token,
            String resultWho, int requestCode, Intent[] intents, String[] resolvedTypes, int flags, Bundle options,
            int userId) {
        return mPendingIntents.get(type, packageName, featureId, token, resultWho, requestCode, intents, resolvedTypes,
                flags, userId);
    }

    @Override
    public void cancelIntentSender(IIntentSender sender) {
        mPendingIntents.cancel(sender);
    }

    @Override
    public ActivityManager.PendingIntentInfo getInfoForIntentSender(IIntentSender sender) {
        return mPendingIntents.info(sender);
    }

    @Override
    public boolean registerIntentSenderCancelListenerEx(IIntentSender sender, IResultReceiver receiver) {
        return mPendingIntents.registerCancelListener(sender, receiver);
    }

    @Override
    public void unregisterIntentSenderCancelListener(IIntentSender sender, IResultReceiver receiver) {
        mPendingIntents.unregisterCancelListener(sender, receiver);
    }

    @Override
    public int sendIntentSender(IApplicationThread caller, IIntentSender target, IBinder whitelistToken, int code,
            Intent intent, String resolvedType, IIntentReceiver finishedReceiver, String requiredPermission,
            Bundle options) {
        return mPendingIntents.send(target, code, intent, resolvedType, finishedReceiver, requiredPermission, options);
    }

    @Override
    public boolean isIntentSenderTargetedToPackage(IIntentSender sender) {
        Unwritten.log("android.app.IActivityManager.isIntentSenderTargetedToPackage (ADR-0045 step d3)", "false");
        return false;
    }

    @Override
    public Intent getIntentForIntentSender(IIntentSender sender) {
        Unwritten.log("android.app.IActivityManager.getIntentForIntentSender (ADR-0045 step d3)", "null");
        return null;
    }

    @Override
    public String getTagForIntentSender(IIntentSender sender, String prefix) {
        Unwritten.log("android.app.IActivityManager.getTagForIntentSender (ADR-0045 step d3)", "null");
        return null;
    }

    @Override
    public ParceledListSlice<ResolveInfo> queryIntentComponentsForIntentSender(IIntentSender sender, int matchFlags) {
        Unwritten.log("android.app.IActivityManager.queryIntentComponentsForIntentSender (ADR-0045 step d3)", "null");
        return null;
    }

    // What the rest of framework.jar asks: until their steps, a line once and Android's default.

    @Override
    public void handleApplicationStrictModeViolation(IBinder app, int penaltyMask,
            StrictMode.ViolationInfo crashInfo) {
        Unwritten.log("android.app.IActivityManager.handleApplicationStrictModeViolation", null);
    }

    @Override
    public void registerStrictModeCallback(IBinder binder) {
        Unwritten.log("android.app.IActivityManager.registerStrictModeCallback", null);
    }

    @Override
    public ComponentName startService(IApplicationThread caller, Intent service, String resolvedType,
            boolean requireForeground, String callingPackage, String callingFeatureId, int userId) {
        return mServices.startService(service, resolvedType, requireForeground);
    }

    @Override
    public int stopService(IApplicationThread caller, Intent service, String resolvedType, int userId) {
        return mServices.stopService(service, resolvedType);
    }

    @Override
    public void serviceDoneExecuting(IBinder token, int type, int startId, int res, Intent intent) {
        mServices.serviceDoneExecuting(token, type, startId, res);
    }

    @Override
    public int bindServiceInstance(IApplicationThread caller, IBinder token, Intent service, String resolvedType,
            IServiceConnection connection, long flags, String instanceName, String callingPackage, int userId) {
        return mServices.bindService(token, service, resolvedType, connection, flags, instanceName);
    }

    @Override
    public boolean unbindService(IServiceConnection connection) {
        return mServices.unbindService(connection);
    }

    @Override
    public void publishService(IBinder token, Intent intent, IBinder service) {
        mServices.publishService(token, intent, service);
    }

    @Override
    public void unbindFinished(IBinder token, Intent service, boolean doRebind) {
        mServices.unbindFinished(token, service, doRebind);
    }

    @Override
    public boolean stopServiceToken(ComponentName className, IBinder token, int startId) {
        return mServices.stopServiceToken(className, token, startId);
    }

    @Override
    public void setServiceForeground(ComponentName className, IBinder token, int id, Notification notification,
            int flags, int foregroundServiceType) {
        mServices.setServiceForeground(className, token, id, notification, flags, foregroundServiceType);
    }

    @Override
    public int getForegroundServiceType(ComponentName className, IBinder token) {
        return mServices.getForegroundServiceType(className, token);
    }

    @Override
    public boolean shouldServiceTimeOut(ComponentName className, IBinder token) {
        Unwritten.log("android.app.IActivityManager.shouldServiceTimeOut", "false");
        return false;
    }

    @Override
    public boolean hasServiceTimeLimitExceeded(ComponentName className, IBinder token) {
        Unwritten.log("android.app.IActivityManager.hasServiceTimeLimitExceeded", "false");
        return false;
    }

    @Override
    public IBinder peekService(Intent service, String resolvedType, String callingPackage) {
        return mServices.peekService(service, resolvedType);
    }

    @Override
    public void closeSystemDialogs(String reason) {
        Unwritten.log("android.app.IActivityManager.closeSystemDialogs", null);
    }

    @Override
    public void setActivityLocusContext(ComponentName activity, LocusId locusId, IBinder appToken) {
        Unwritten.log("android.app.IActivityManager.setActivityLocusContext", null);
    }

    @Override
    public void addInstrumentationResults(IApplicationThread target, Bundle results) {
        Unwritten.log("android.app.IActivityManager.addInstrumentationResults", null);
    }

    @Override
    public void getMimeTypeFilterAsync(Uri uri, int userId, RemoteCallback resultCallback) {
        Unwritten.log("android.app.IActivityManager.getMimeTypeFilterAsync", "no type");
        resultCallback.sendResult(new Bundle());
    }

    /** As ActivityManagerService for Minato's one user, user 0, which is running; any other user does not exist. */
    @Override
    public boolean isUserRunning(int userid, int flags) {
        return userid == UserHandle.USER_SYSTEM;
    }
}
