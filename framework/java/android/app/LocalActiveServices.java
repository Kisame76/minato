/*
 * The app's started services (ADR-0045 step SV2): what system_server's ActiveServices keeps for them, kept in the one
 * app process for LocalActivityManagerService, as ActiveServices does at android-15.0.0_r1.
 *
 * A start finds its service (explicit or by the package's filters, through ManifestPackage's queries with the
 * component states the app set) and answers at once: the component, null for none, as startServiceLocked does. The
 * service's record then asks the main thread to make the service (once) and to give it each start's intent with the
 * next start ID, in that order, as realStartServiceLocked and sendServiceArgsLocked do; the main thread's
 * ActivityThread makes it through the app's AppComponentFactory. A stop by intent, stopSelf without a start ID and
 * stopSelf with the last start ID end the record and ask the main thread to destroy the service, as stopServiceLocked,
 * stopServiceTokenLocked and bringDownServiceLocked do; a stop of none answers 0. A record found again by its
 * component or by its first intent's filter is the same service.
 *
 * Foreground services as setServiceForegroundInnerLocked has them: startForeground needs FOREGROUND_SERVICE from a
 * target SDK of 28, the type asked for must be a subset of the manifest's, and a target SDK of 34 or later needs a
 * type (MissingForegroundServiceTypeException, as the type policy's FGS_TYPE_NONE_DISABLED_CHANGE_ID); a service
 * started with startForegroundService must call it within 30 s, and not be stopped before, else the app crashes
 * with ForegroundServiceDidNotStartInTimeException, as serviceForegroundTimeout and bringDownServiceLocked crash it.
 *
 * Named limits: the type policy's permissions of each type (FOREGROUND_SERVICE_DATA_SYNC and the others) are not
 * checked yet; the app counts as in the foreground, so no start is refused as a background start ("?") and none is
 * delayed; a service of another android:process runs in the app's one process; the foreground notification goes to
 * the notification service, which drops it while the app has no POST_NOTIFICATIONS, as Android hides it; START_STICKY
 * and the redelivery of START_REDELIVER_INTENT matter only when Android restarts a killed process, which Minato never
 * does; the ANR that follows Android's timeout crash is not shown.
 */
package android.app;

import android.Manifest;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.os.Binder;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Process;
import android.os.RemoteException;
import android.os.UserHandle;
import android.util.ArrayMap;
import android.util.Slog;

import java.util.ArrayList;
import java.util.List;

final class LocalActiveServices {
    private static final String TAG = "ActivityManager";
    /** ActivityManagerConstants' DEFAULT_SERVICE_START_FOREGROUND_TIMEOUT_MS. */
    private static final long START_FOREGROUND_TIMEOUT_MS = 30 * 1000;
    /** ActivityManagerService's STOCK_PM_FLAGS, with which ActiveServices resolves a service. */
    private static final long STOCK_PM_FLAGS = PackageManager.GET_SHARED_LIBRARY_FILES;

    private final Object mLock = new Object();
    /** The services by component, as ServiceMap's mServicesByInstanceName; under mLock, as the map below. */
    private final ArrayMap<ComponentName, ServiceRecord> mByName = new ArrayMap<>();
    /** The services by the filter of the intent that made each, as ServiceMap's mServicesByIntent. */
    private final ArrayMap<Intent.FilterComparison, ServiceRecord> mByIntent = new ArrayMap<>();
    private Handler mHandler;

    /** A service, as ServiceRecord, which is also the token its Service holds. */
    static final class ServiceRecord extends Binder {
        final ComponentName name;
        final ServiceInfo serviceInfo;
        final Intent.FilterComparison intent;
        /** Whether a start asked for the service and no stop ended it. */
        boolean startRequested;
        /** Whether startForegroundService asked for it and startForeground has not been called. */
        boolean fgRequired;
        /** Whether the 30 s for startForeground run. */
        boolean fgWaiting;
        boolean isForeground;
        /** Whether the main thread was asked to make the service. */
        boolean created;
        boolean destroying;
        int foregroundId;
        int foregroundServiceType;
        Notification foregroundNoti;
        int lastStartId;
        /** The start IDs given to the service and not yet done with, oldest first. */
        final ArrayList<Integer> deliveredStarts = new ArrayList<>();
        /** The service's bindings by the filter of their intents, as ServiceRecord's bindings (step SV3). */
        final ArrayMap<Intent.FilterComparison, IntentBindRecord> bindings = new ArrayMap<>();
        /** Its connections by their IServiceConnection's binder, as ServiceRecord's connections. */
        final ArrayMap<IBinder, ArrayList<ConnectionRecord>> connections = new ArrayMap<>();

        ServiceRecord(ComponentName name, ServiceInfo serviceInfo, Intent.FilterComparison intent) {
            this.name = name;
            this.serviceInfo = serviceInfo;
            this.intent = intent;
        }

        /** As hasAutoCreateConnections: whether a connection with BIND_AUTO_CREATE needs the service. */
        boolean hasAutoCreateConnections() {
            for (int i = connections.size() - 1; i >= 0; i--) {
                for (ConnectionRecord c : connections.valueAt(i)) {
                    if ((c.flags & Context.BIND_AUTO_CREATE) != 0) {
                        return true;
                    }
                }
            }
            return false;
        }

        int makeNextStartId() {
            lastStartId++;
            if (lastStartId < 1) {
                lastStartId = 1;
            }
            return lastStartId;
        }

        /** As ServiceRecord's: the record and its short component name for user 0. */
        @Override
        public String toString() {
            return "ServiceRecord{" + Integer.toHexString(System.identityHashCode(this)) + " u0 "
                    + name.flattenToShortString() + "}";
        }
    }

    /**
     * A binding of one intent's filter, as IntentBindRecord: the binder its onBind gave, and the app's connections to
     * it, of which the one app process is the only client.
     */
    static final class IntentBindRecord {
        final Intent intent;
        final ArrayList<ConnectionRecord> connections = new ArrayList<>();
        IBinder binder;
        /** Whether onBind was asked for, whether its binder came, whether the service is bound, and whether onUnbind
         *  asked for onRebind at the next binding. */
        boolean requested;
        boolean received;
        boolean hasBound;
        boolean doRebind;

        IntentBindRecord(Intent intent) {
            this.intent = intent;
        }
    }

    /** A ServiceConnection's binding to a service, as ConnectionRecord. */
    static final class ConnectionRecord {
        final ServiceRecord service;
        final IntentBindRecord binding;
        final IServiceConnection conn;
        final long flags;
        /** Whether the service went under the connection, which then unbinds without an onUnbind. */
        boolean serviceDead;

        ConnectionRecord(ServiceRecord service, IntentBindRecord binding, IServiceConnection conn, long flags) {
            this.service = service;
            this.binding = binding;
            this.conn = conn;
            this.flags = flags;
        }
    }

    /** Every connection by its IServiceConnection's binder, as ActiveServices' mServiceConnections; under mLock. */
    private final ArrayMap<IBinder, ArrayList<ConnectionRecord>> mServiceConnections = new ArrayMap<>();

    private Handler handler() {
        synchronized (mLock) {
            if (mHandler == null) {
                mHandler = new Handler(Looper.getMainLooper());
            }
            return mHandler;
        }
    }

    /**
     * As retrieveServiceLocked for the app's own services: the record of the intent's component, else of its filter,
     * else of the service the package manager resolves, which is a new one only when {@code create}; null for none.
     */
    private ServiceRecord retrieveLocked(Intent service, String resolvedType, boolean create) {
        ComponentName comp = service.getComponent();
        ServiceRecord r = comp != null ? mByName.get(comp) : null;
        if (r == null) {
            r = mByIntent.get(new Intent.FilterComparison(service));
        }
        if (r != null) {
            return r;
        }
        ActivityThread thread = ActivityThread.currentActivityThread();
        ManifestPackage pkg = thread != null ? thread.getManifestPackage() : null;
        if (pkg == null) {
            return null;
        }
        List<ResolveInfo> found = pkg.query(ManifestPackage.QUERY_SERVICES, service, resolvedType, STOCK_PM_FLAGS,
                ApplicationPackageManager::componentState);
        ServiceInfo info = found.isEmpty() ? null : found.get(0).serviceInfo;
        if (info == null) {
            Slog.w(TAG, "Unable to start service " + service + " U=" + UserHandle.myUserId() + ": not found");
            return null;
        }
        ComponentName name = new ComponentName(info.applicationInfo.packageName, info.name);
        r = mByName.get(name);
        if (r == null && create) {
            Intent.FilterComparison filter = new Intent.FilterComparison(service.cloneFilter());
            r = new ServiceRecord(name, info, filter);
            mByName.put(name, r);
            mByIntent.put(filter, r);
        }
        return r;
    }

    /** As startServiceLocked and startServiceInnerLocked: the service's component, null for none. */
    ComponentName startService(Intent service, String resolvedType, boolean fgRequired) {
        synchronized (mLock) {
            ServiceRecord r = retrieveLocked(service, resolvedType, true);
            if (r == null) {
                return null;
            }
            if (r.serviceInfo.applicationInfo.targetSdkVersion < Build.VERSION_CODES.O && fgRequired) {
                fgRequired = false;
            }
            r.startRequested = true;
            r.fgRequired = fgRequired;
            if (!r.created) {
                bringUpServiceLocked(r, service);
            }
            sendServiceArgsLocked(r, r.makeNextStartId(), new Intent(service));
            return r.name;
        }
    }

    /**
     * As bringUpServiceLocked and realStartServiceLocked for a service not yet made: the main thread makes it, and then
     * binds each binding that has a connection, before any start's command.
     */
    private void bringUpServiceLocked(ServiceRecord r, Intent intent) {
        r.created = true;
        ActivityThread.currentActivityThread().scheduleCreateService(r, r.serviceInfo, new Intent(intent));
        for (int i = 0; i < r.bindings.size(); i++) {
            requestServiceBindingLocked(r, r.bindings.valueAt(i), false);
        }
    }

    /** As requestServiceBindingLocked: onBind (or onRebind) for a binding with a connection, once the service is made. */
    private void requestServiceBindingLocked(ServiceRecord r, IntentBindRecord b, boolean rebind) {
        if (!r.created) {
            return;
        }
        if ((!b.requested || rebind) && !b.connections.isEmpty()) {
            ActivityThread.currentActivityThread().scheduleBindService(r, new Intent(b.intent), rebind);
            if (!rebind) {
                b.requested = true;
            }
            b.hasBound = true;
            b.doRebind = false;
        }
    }

    /** As sendServiceArgsLocked for one start: a foreground service's 30 s begin with its first start's delivery. */
    private void sendServiceArgsLocked(ServiceRecord r, int startId, Intent intent) {
        r.deliveredStarts.add(startId);
        if (r.fgRequired && !r.fgWaiting) {
            if (!r.isForeground) {
                r.fgWaiting = true;
                handler().postDelayed(() -> serviceForegroundTimeout(r), START_FOREGROUND_TIMEOUT_MS);
            } else {
                r.fgRequired = false;
            }
        }
        ActivityThread.currentActivityThread().scheduleServiceArgs(r, startId, 0, intent);
    }

    /** As stopServiceLocked by intent: 1 when the service was running and is stopped, 0 for none. */
    int stopService(Intent service, String resolvedType) {
        synchronized (mLock) {
            ServiceRecord r = retrieveLocked(service, resolvedType, false);
            if (r == null) {
                return 0;
            }
            r.startRequested = false;
            bringDownServiceIfNeededLocked(r);
            return 1;
        }
    }

    /**
     * As stopServiceTokenLocked, Service.stopSelf's: with a start ID, the starts up to it are done, and only the last
     * one stops the service; false when it did not stop.
     */
    boolean stopServiceToken(ComponentName className, IBinder token, int startId) {
        synchronized (mLock) {
            ServiceRecord r = findServiceLocked(className, token);
            if (r == null) {
                return false;
            }
            if (startId >= 0) {
                int index = r.deliveredStarts.indexOf(startId);
                if (index >= 0) {
                    r.deliveredStarts.subList(0, index + 1).clear();
                }
                if (r.lastStartId != startId) {
                    return false;
                }
                if (!r.deliveredStarts.isEmpty()) {
                    Slog.w(TAG, "stopServiceToken startId " + startId + " is last, but have "
                            + r.deliveredStarts.size() + " remaining args");
                }
            }
            r.startRequested = false;
            bringDownServiceIfNeededLocked(r);
            return true;
        }
    }

    /** As findServiceLocked: the running record of the component whose token it is. */
    private ServiceRecord findServiceLocked(ComponentName className, IBinder token) {
        ServiceRecord r = className != null ? mByName.get(className) : null;
        return r == token ? r : null;
    }

    /** As bringDownServiceIfNeededLocked: a service that no start and no BIND_AUTO_CREATE connection needs goes. */
    private void bringDownServiceIfNeededLocked(ServiceRecord r) {
        if (r.startRequested || r.hasAutoCreateConnections()) {
            return;
        }
        bringDownServiceLocked(r);
    }

    /**
     * As bringDownServiceLocked: a foreground service still owing its startForeground crashes the app, the record goes,
     * its notification with it, and the main thread destroys the service.
     */
    private void bringDownServiceLocked(ServiceRecord r) {
        // Its connections hear of its death; its bound bindings are unbound before it is destroyed.
        for (int i = r.connections.size() - 1; i >= 0; i--) {
            for (ConnectionRecord c : r.connections.valueAt(i)) {
                c.serviceDead = true;
                try {
                    c.conn.connected(r.name, null, true);
                } catch (RemoteException e) {
                    Slog.w(TAG, "Failure disconnecting service " + r.name.flattenToShortString(), e);
                }
            }
        }
        if (r.created) {
            for (int i = r.bindings.size() - 1; i >= 0; i--) {
                IntentBindRecord b = r.bindings.valueAt(i);
                if (b.hasBound) {
                    b.hasBound = false;
                    b.requested = false;
                    ActivityThread.currentActivityThread().scheduleUnbindService(r, new Intent(b.intent));
                }
            }
        }
        if (r.fgRequired) {
            Slog.w(TAG, "Bringing down service while still waiting for start foreground: " + r);
            r.fgRequired = false;
            r.fgWaiting = false;
            final String record = r.toString();
            final String className = r.name.getClassName();
            handler().post(() -> serviceForegroundCrash(record, className));
        }
        r.destroying = true;
        mByName.remove(r.name);
        mByIntent.remove(r.intent);
        cancelForegroundNotificationLocked(r);
        r.isForeground = false;
        r.foregroundServiceType = 0;
        if (r.created) {
            ActivityThread.currentActivityThread().scheduleStopService(r);
        }
    }

    /**
     * As serviceForegroundTimeout: a service that did not call startForeground within 30 s of its start's delivery is
     * stopped, which crashes the app.
     */
    private void serviceForegroundTimeout(ServiceRecord r) {
        synchronized (mLock) {
            if (!r.fgRequired || !r.fgWaiting || r.destroying) {
                return;
            }
            r.fgWaiting = false;
            r.startRequested = false;
            bringDownServiceIfNeededLocked(r);
        }
    }

    /** As serviceForegroundCrash and ActivityThread.throwRemoteServiceException: the app's main thread throws. */
    private static void serviceForegroundCrash(String record, String className) {
        throw new RemoteServiceException.ForegroundServiceDidNotStartInTimeException(
                "Context.startForegroundService() did not then call Service.startForeground(): " + record,
                Service.getStartForegroundServiceStackTrace(className));
    }

    /** As serviceDoneExecutingLocked for a start: the start ID is done with unless the intent is to be redelivered. */
    void serviceDoneExecuting(IBinder token, int type, int startId, int res) {
        if (type != ActivityThread.SERVICE_DONE_EXECUTING_START || !(token instanceof ServiceRecord)) {
            return;
        }
        ServiceRecord r = (ServiceRecord) token;
        synchronized (mLock) {
            switch (res) {
                case Service.START_STICKY_COMPATIBILITY:
                case Service.START_STICKY:
                case Service.START_NOT_STICKY:
                case Service.START_TASK_REMOVED_COMPLETE:
                    r.deliveredStarts.remove(Integer.valueOf(startId));
                    break;
                case Service.START_REDELIVER_INTENT:
                    break;
                default:
                    throw new IllegalArgumentException("Unknown service start result: " + res);
            }
        }
    }

    /** As setServiceForegroundLocked: startForeground with an ID, stopForeground with 0. */
    void setServiceForeground(ComponentName className, IBinder token, int id, Notification notification, int flags,
            int foregroundServiceType) {
        synchronized (mLock) {
            ServiceRecord r = findServiceLocked(className, token);
            if (r == null) {
                return;
            }
            if (id != 0) {
                startForegroundLocked(r, id, notification, foregroundServiceType);
            } else if (r.isForeground) {
                if ((flags & Service.STOP_FOREGROUND_REMOVE) != 0) {
                    cancelForegroundNotificationLocked(r);
                    r.foregroundId = 0;
                    r.foregroundNoti = null;
                } else if ((flags & Service.STOP_FOREGROUND_DETACH) != 0) {
                    r.foregroundId = 0;
                    r.foregroundNoti = null;
                }
                r.isForeground = false;
                r.foregroundServiceType = 0;
            }
        }
    }

    /** setServiceForegroundInnerLocked's checks for the app's own service, then the foreground state. */
    @SuppressWarnings("deprecation") // FOREGROUND_SERVICE_TYPE_NONE, which ActiveServices compares with.
    private void startForegroundLocked(ServiceRecord r, int id, Notification notification, int foregroundServiceType) {
        if (notification == null) {
            throw new IllegalArgumentException("null notification");
        }
        final ApplicationInfo app = r.serviceInfo.applicationInfo;
        final Application application = ActivityThread.currentApplication();
        if (app.targetSdkVersion >= Build.VERSION_CODES.P && (application == null
                || application.checkPermission(Manifest.permission.FOREGROUND_SERVICE, Process.myPid(),
                        Process.myUid()) != PackageManager.PERMISSION_GRANTED)) {
            throw new SecurityException("Permission Denial: startForeground from pid=" + Process.myPid() + ", uid="
                    + Process.myUid() + " requires " + Manifest.permission.FOREGROUND_SERVICE);
        }
        final int startType = foregroundServiceType;
        final int manifestType = r.serviceInfo.getForegroundServiceType();
        if (foregroundServiceType == ServiceInfo.FOREGROUND_SERVICE_TYPE_MANIFEST) {
            foregroundServiceType = manifestType;
        }
        if ((foregroundServiceType & manifestType) != foregroundServiceType) {
            throw new IllegalArgumentException("foregroundServiceType "
                    + String.format("0x%08X", foregroundServiceType)
                    + " is not a subset of foregroundServiceType attribute "
                    + String.format("0x%08X", manifestType) + " in service element of manifest file");
        }
        if ((foregroundServiceType & ServiceInfo.FOREGROUND_SERVICE_TYPE_SHORT_SERVICE) != 0
                && foregroundServiceType != ServiceInfo.FOREGROUND_SERVICE_TYPE_SHORT_SERVICE) {
            Slog.w(TAG, "startForeground(): FOREGROUND_SERVICE_TYPE_SHORT_SERVICE"
                    + " is combined with other types. SHORT_SERVICE will be ignored.");
            foregroundServiceType &= ~ServiceInfo.FOREGROUND_SERVICE_TYPE_SHORT_SERVICE;
        }
        if (r.fgRequired) {
            r.fgRequired = false;
            r.fgWaiting = false;
        }
        if (foregroundServiceType == ServiceInfo.FOREGROUND_SERVICE_TYPE_SHORT_SERVICE && !r.startRequested) {
            throw new StartForegroundCalledOnStoppedServiceException(
                    "startForeground(SHORT_SERVICE) called on a service that's not started.");
        }
        // The type policy's FGS_TYPE_NONE_DISABLED_CHANGE_ID, enabled after Android 13: no type from Android 14 on.
        if (foregroundServiceType == ServiceInfo.FOREGROUND_SERVICE_TYPE_NONE
                && app.targetSdkVersion > Build.VERSION_CODES.TIRAMISU) {
            final String caller = "ProcessRecord{" + Integer.toHexString(System.identityHashCode(this)) + " "
                    + Process.myPid() + ":" + app.processName + "/" + uidName(app.uid) + "}";
            if (startType == ServiceInfo.FOREGROUND_SERVICE_TYPE_MANIFEST) {
                throw new MissingForegroundServiceTypeException("Starting FGS without a type "
                        + " callerApp=" + caller + " targetSDK=" + app.targetSdkVersion);
            }
            throw new InvalidForegroundServiceTypeException("Starting FGS with type "
                    + ServiceInfo.foregroundServiceTypeToLabel(foregroundServiceType) + " callerApp=" + caller
                    + " targetSDK=" + app.targetSdkVersion + " has been prohibited");
        }
        if (r.foregroundId != id) {
            cancelForegroundNotificationLocked(r);
            r.foregroundId = id;
        }
        notification.flags |= Notification.FLAG_FOREGROUND_SERVICE;
        r.foregroundNoti = notification;
        r.foregroundServiceType = foregroundServiceType;
        r.isForeground = true;
        postNotificationLocked(r);
    }

    /** As UserHandle.formatUid: an app's uid for user 0, such as u0a123. */
    private static String uidName(int uid) {
        StringBuilder sb = new StringBuilder();
        UserHandle.formatUid(sb, uid);
        return sb.toString();
    }

    /** As ServiceRecord.postNotification: the foreground notification through the notification service. */
    private void postNotificationLocked(ServiceRecord r) {
        final Application application = ActivityThread.currentApplication();
        if (application == null || r.foregroundNoti == null) {
            return;
        }
        final int id = r.foregroundId;
        final Notification notification = r.foregroundNoti;
        handler().post(() -> application.getSystemService(NotificationManager.class).notify(id, notification));
    }

    /** As cancelForegroundNotificationLocked. */
    private void cancelForegroundNotificationLocked(ServiceRecord r) {
        final Application application = ActivityThread.currentApplication();
        if (application == null || r.foregroundId == 0) {
            return;
        }
        final int id = r.foregroundId;
        handler().post(() -> application.getSystemService(NotificationManager.class).cancel(id));
    }

    /**
     * As bindServiceLocked: 1 once the connection is bound to the service, 0 for none; the service is made for a
     * BIND_AUTO_CREATE connection, the connection gets the binder at once when the binding has one, and else onBind is
     * asked for (step SV3). An activity's token is not checked: the activity's context unbinds its connections when it
     * goes (LoadedApk.removeContextRegistrations).
     */
    int bindService(IBinder token, Intent service, String resolvedType, IServiceConnection connection, long flags,
            String instanceName) {
        if (instanceName != null) {
            // bindIsolatedService's, for an isolated process, which Minato has none of.
            throw new IllegalArgumentException("Can't use instance name '" + instanceName
                    + "' with non-isolated non-sdk sandbox service '" + service.getComponent() + "'");
        }
        synchronized (mLock) {
            ServiceRecord r = retrieveLocked(service, resolvedType, true);
            if (r == null) {
                return 0;
            }
            Intent.FilterComparison filter = new Intent.FilterComparison(service);
            IntentBindRecord b = r.bindings.get(filter);
            if (b == null) {
                b = new IntentBindRecord(new Intent(service));
                r.bindings.put(filter, b);
            }
            ConnectionRecord c = new ConnectionRecord(r, b, connection, flags);
            IBinder binder = connection.asBinder();
            r.connections.computeIfAbsent(binder, key -> new ArrayList<>()).add(c);
            b.connections.add(c);
            mServiceConnections.computeIfAbsent(binder, key -> new ArrayList<>()).add(c);
            if ((flags & Context.BIND_AUTO_CREATE) != 0 && !r.created) {
                bringUpServiceLocked(r, service);
            }
            if (r.created && b.received) {
                try {
                    c.conn.connected(r.name, b.binder, false);
                } catch (RemoteException e) {
                    Slog.w(TAG, "Failure sending service " + r.name.flattenToShortString() + " to connection", e);
                }
                if (b.connections.size() == 1 && b.doRebind) {
                    requestServiceBindingLocked(r, b, true);
                }
            } else if (!b.requested) {
                requestServiceBindingLocked(r, b, false);
            }
            return 1;
        }
    }

    /** As publishServiceLocked: onBind's binder for its binding's connections, once. */
    void publishService(IBinder token, Intent intent, IBinder service) {
        synchronized (mLock) {
            if (!(token instanceof ServiceRecord)) {
                return;
            }
            ServiceRecord r = (ServiceRecord) token;
            Intent.FilterComparison filter = new Intent.FilterComparison(intent);
            IntentBindRecord b = r.bindings.get(filter);
            if (b == null || b.received) {
                return;
            }
            b.binder = service;
            b.requested = true;
            b.received = true;
            for (int i = r.connections.size() - 1; i >= 0; i--) {
                for (ConnectionRecord c : r.connections.valueAt(i)) {
                    if (!filter.equals(new Intent.FilterComparison(c.binding.intent))) {
                        continue;
                    }
                    try {
                        c.conn.connected(r.name, service, false);
                    } catch (RemoteException e) {
                        Slog.w(TAG, "Failure sending service " + r.name.flattenToShortString() + " to connection",
                                e);
                    }
                }
            }
        }
    }

    /** As unbindServiceLocked: each of the connection's bindings goes; false for a connection bound to nothing. */
    boolean unbindService(IServiceConnection connection) {
        synchronized (mLock) {
            ArrayList<ConnectionRecord> clist = mServiceConnections.get(connection.asBinder());
            if (clist == null) {
                Slog.w(TAG, "Unbind failed: could not find connection for " + connection.asBinder());
                return false;
            }
            while (!clist.isEmpty()) {
                ConnectionRecord c = clist.get(0);
                removeConnectionLocked(c);
                if (!clist.isEmpty() && clist.get(0) == c) {
                    clist.remove(0);
                }
            }
            return true;
        }
    }

    /**
     * As removeConnectionLocked: the connection leaves its service and binding; the last one of a bound binding has
     * the service unbound, and a service that no longer needs to run goes.
     */
    private void removeConnectionLocked(ConnectionRecord c) {
        IBinder binder = c.conn.asBinder();
        ServiceRecord s = c.service;
        IntentBindRecord b = c.binding;
        ArrayList<ConnectionRecord> clist = s.connections.get(binder);
        if (clist != null) {
            clist.remove(c);
            if (clist.isEmpty()) {
                s.connections.remove(binder);
            }
        }
        b.connections.remove(c);
        clist = mServiceConnections.get(binder);
        if (clist != null) {
            clist.remove(c);
            if (clist.isEmpty()) {
                mServiceConnections.remove(binder);
            }
        }
        if (!c.serviceDead) {
            if (s.created && b.connections.isEmpty() && b.hasBound) {
                b.hasBound = false;
                b.doRebind = false;
                ActivityThread.currentActivityThread().scheduleUnbindService(s, new Intent(b.intent));
            }
            if ((c.flags & Context.BIND_AUTO_CREATE) != 0) {
                bringDownServiceIfNeededLocked(s);
            }
        }
    }

    /** As unbindFinishedLocked: onUnbind asked for onRebind, which comes now if a connection is there, else later. */
    void unbindFinished(IBinder token, Intent intent, boolean doRebind) {
        synchronized (mLock) {
            if (!(token instanceof ServiceRecord)) {
                return;
            }
            ServiceRecord r = (ServiceRecord) token;
            IntentBindRecord b = r.bindings.get(new Intent.FilterComparison(intent));
            if (b == null) {
                return;
            }
            if (!b.connections.isEmpty() && !r.destroying) {
                requestServiceBindingLocked(r, b, true);
            } else {
                b.doRebind = true;
            }
        }
    }

    /** As peekServiceLocked: the binder of the running service's binding of this intent, null for none. */
    IBinder peekService(Intent service, String resolvedType) {
        synchronized (mLock) {
            ServiceRecord r = retrieveLocked(service, resolvedType, false);
            if (r == null) {
                return null;
            }
            IntentBindRecord b = r.bindings.get(r.intent);
            return b != null ? b.binder : null;
        }
    }

    /** As getForegroundServiceTypeLocked. */
    @SuppressWarnings("deprecation") // FOREGROUND_SERVICE_TYPE_NONE, which ActiveServices answers.
    int getForegroundServiceType(ComponentName className, IBinder token) {
        synchronized (mLock) {
            ServiceRecord r = findServiceLocked(className, token);
            return r != null ? r.foregroundServiceType : ServiceInfo.FOREGROUND_SERVICE_TYPE_NONE;
        }
    }
}
