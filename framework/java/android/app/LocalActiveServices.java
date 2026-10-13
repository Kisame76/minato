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

        ServiceRecord(ComponentName name, ServiceInfo serviceInfo, Intent.FilterComparison intent) {
            this.name = name;
            this.serviceInfo = serviceInfo;
            this.intent = intent;
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
                r.created = true;
                ActivityThread.currentActivityThread().scheduleCreateService(r, r.serviceInfo, new Intent(service));
            }
            sendServiceArgsLocked(r, r.makeNextStartId(), new Intent(service));
            return r.name;
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

    /** As bringDownServiceIfNeededLocked: a service no start needs any more goes. */
    private void bringDownServiceIfNeededLocked(ServiceRecord r) {
        if (r.startRequested) {
            return;
        }
        bringDownServiceLocked(r);
    }

    /**
     * As bringDownServiceLocked: a foreground service still owing its startForeground crashes the app, the record goes,
     * its notification with it, and the main thread destroys the service.
     */
    private void bringDownServiceLocked(ServiceRecord r) {
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
        ActivityThread.currentActivityThread().scheduleStopService(r);
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

    /** As getForegroundServiceTypeLocked. */
    @SuppressWarnings("deprecation") // FOREGROUND_SERVICE_TYPE_NONE, which ActiveServices answers.
    int getForegroundServiceType(ComponentName className, IBinder token) {
        synchronized (mLock) {
            ServiceRecord r = findServiceLocked(className, token);
            return r != null ? r.foregroundServiceType : ServiceInfo.FOREGROUND_SERVICE_TYPE_NONE;
        }
    }
}
