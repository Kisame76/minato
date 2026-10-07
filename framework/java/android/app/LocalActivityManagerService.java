/*
 * The activity manager service of Minato's one app process (ADR-0045 decision 5, step d2): what system_server's
 * ActivityManagerService answers an app's receivers and broadcasts with, answered in the process, as ServiceManager's
 * "activity". Registration follows registerReceiverWithFeatureTraced at android-15.0.0_r1: both export flags throw
 * IllegalArgumentException; a receiver of a target SDK of 34 or later must name one of them unless every action of its
 * filter is a protected broadcast of framework-res's manifest, else SecurityException, as the compat change
 * DYNAMIC_RECEIVER_EXPLICIT_EXPORT_REQUIRED (@EnabledSince UPSIDE_DOWN_CAKE) has it; a null receiver only looks up the
 * sticky broadcasts, of which there are none yet. A broadcast reaches the registered filters that match it, by
 * priority and then in registration order, each receiver with its own copy of the intent and of the result: an
 * unordered one at once and assumed delivered, as Android 14's queue does for registered receivers; an ordered one
 * receiver by receiver, the next after the one before finishes (finishReceiver, from the app thread, as
 * BroadcastReceiver.PendingResult sends it), until the end or an abort, then its final receiver, also when nothing
 * matched. Broadcasts reach the process one after the other, as Android's per-process queue delivers them: one waits
 * while an ordered one before it waits for a receiver. The one app is sender and receiver alike, so exported or not,
 * every filter matches it, and permissions are the app's own. Not yet written, each answered as the generated
 * methods are, with a line once and Android's default: sticky broadcasts, broadcast options and app ops, manifest
 * receivers (ADR-0045 decision 5), PendingIntents (step d3), and the services, StrictMode's dropbox, instrumentation
 * and locus contexts that framework.jar also asks the activity manager about.
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
    /** The registered receivers by their IIntentReceiver's Binder; under mLock, as the two below. */
    private final HashMap<IBinder, ReceiverList> mReceivers = new HashMap<>();
    /** Every registered filter in registration order, which ties of priority keep. */
    private final ArrayList<BroadcastFilter> mFilters = new ArrayList<>();
    /** The broadcasts for the process, the first delivering. */
    private final ArrayDeque<Broadcast> mQueue = new ArrayDeque<>();

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
        // The first matching sticky broadcast: none, as no sticky broadcast is kept yet.
        Intent sticky = null;
        if (receiver == null) {
            return sticky;
        }
        synchronized (mLock) {
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
        }
        return sticky;
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
        String sender = packageName();
        return () -> {
            try {
                if (receiver instanceof LoadedApk.ReceiverDispatcher.InnerReceiver) {
                    ((LoadedApk.ReceiverDispatcher.InnerReceiver) receiver).performReceive(intent, code, data, extras,
                            ordered, false, assumeDelivered, UserHandle.myUserId(), Process.myUid(), sender);
                } else {
                    receiver.performReceive(intent, code, data, extras, ordered, false, UserHandle.myUserId());
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

    // PendingIntents come with step d3: until then, as the generated methods, a line once and Android's default.

    @Override
    public IIntentSender getIntentSenderWithFeature(int type, String packageName, String featureId, IBinder token,
            String resultWho, int requestCode, Intent[] intents, String[] resolvedTypes, int flags, Bundle options,
            int userId) {
        Unwritten.log("android.app.IActivityManager.getIntentSenderWithFeature (ADR-0045 step d3)", "null");
        return null;
    }

    @Override
    public void cancelIntentSender(IIntentSender sender) {
        Unwritten.log("android.app.IActivityManager.cancelIntentSender (ADR-0045 step d3)", null);
    }

    @Override
    public ActivityManager.PendingIntentInfo getInfoForIntentSender(IIntentSender sender) {
        Unwritten.log("android.app.IActivityManager.getInfoForIntentSender (ADR-0045 step d3)", "null");
        return null;
    }

    @Override
    public boolean registerIntentSenderCancelListenerEx(IIntentSender sender, IResultReceiver receiver) {
        Unwritten.log("android.app.IActivityManager.registerIntentSenderCancelListenerEx (ADR-0045 step d3)",
                "false");
        return false;
    }

    @Override
    public void unregisterIntentSenderCancelListener(IIntentSender sender, IResultReceiver receiver) {
        Unwritten.log("android.app.IActivityManager.unregisterIntentSenderCancelListener (ADR-0045 step d3)", null);
    }

    @Override
    public int sendIntentSender(IApplicationThread caller, IIntentSender target, IBinder whitelistToken, int code,
            Intent intent, String resolvedType, IIntentReceiver finishedReceiver, String requiredPermission,
            Bundle options) {
        Unwritten.log("android.app.IActivityManager.sendIntentSender (ADR-0045 step d3)", "START_CANCELED");
        return ActivityManager.START_CANCELED;
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
    public boolean stopServiceToken(ComponentName className, IBinder token, int startId) {
        Unwritten.log("android.app.IActivityManager.stopServiceToken", "false");
        return false;
    }

    @Override
    public void setServiceForeground(ComponentName className, IBinder token, int id, Notification notification,
            int flags, int foregroundServiceType) {
        Unwritten.log("android.app.IActivityManager.setServiceForeground", null);
    }

    @Override
    public int getForegroundServiceType(ComponentName className, IBinder token) {
        Unwritten.log("android.app.IActivityManager.getForegroundServiceType", "0");
        return 0;
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
        Unwritten.log("android.app.IActivityManager.peekService", "null");
        return null;
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
}
