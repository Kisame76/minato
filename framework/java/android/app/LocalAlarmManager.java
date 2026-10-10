/*
 * The alarm manager's service (ADR-0045: jobs and alarms kept, none run; batch D 3): IAlarmManager of system_server,
 * local in the app's process, which ServiceManager's table gives as "alarm" and AOSP's AlarmManager asks. It answers
 * as Android 15's AlarmManagerService does for an app [verified: alarm-test on apk-runner-35]:
 *   - an alarm's type must be one of the four, and an alarm names a PendingIntent or a listener, else it is dropped;
 *   - an exact alarm with a PendingIntent and an alarm clock need SCHEDULE_EXACT_ALARM or USE_EXACT_ALARM from an app
 *     that targets Android 12 or later, and are refused with Android's message without; an exact alarm with a listener
 *     needs neither;
 *   - an alarm is kept by its PendingIntent (Intent.filterEquals, through PendingIntent.equals) or its listener, and
 *     the same one replaces it; an app may keep 500, and the next is refused with Android's message;
 *   - cancel by PendingIntent or listener, cancelAll for the app's package, and the next alarm clock are answered from
 *     what is kept;
 *   - setTime and setTimeZone need SET_TIME and SET_TIME_ZONE, which no app holds.
 * The alarms live as long as the process and none goes off; going off while the app is in front is the compatibility
 * plan's decision 6, as for jobs. Not checked: the quotas of allow-while-idle alarms, the minimum window and interval,
 * which only change when Android delivers, and work sources, which only the system names. Every other call is AIDL's
 * Default answer. Not under com.android.minato, whose classes tools/framework-host-check.sh compiles on the host without
 * android.*.
 */
package android.app;

import android.Manifest;
import android.content.pm.PackageManager;
import android.os.Binder;
import android.os.Build;
import android.os.IBinder;
import android.os.WorkSource;

import java.util.ArrayList;
import java.util.List;
import java.util.Objects;

public final class LocalAlarmManager extends IAlarmManager.Default {
    /** AlarmManagerService's default MAX_ALARMS_PER_UID: the count before a new alarm is added must stay below it. */
    private static final int MAX_ALARMS_PER_UID = 500;

    /** An alarm kept: its package, type, trigger time, and its PendingIntent or listener. */
    private static final class Alarm {
        final String packageName;
        final int type;
        final long triggerAtTime;
        final PendingIntent operation;
        final IAlarmListener listener;
        final AlarmManager.AlarmClockInfo alarmClock;

        Alarm(String packageName, int type, long triggerAtTime, PendingIntent operation, IAlarmListener listener,
                AlarmManager.AlarmClockInfo alarmClock) {
            this.packageName = packageName;
            this.type = type;
            this.triggerAtTime = triggerAtTime;
            this.operation = operation;
            this.listener = listener;
            this.alarmClock = alarmClock;
        }

        boolean matches(PendingIntent otherOperation, IAlarmListener otherListener) {
            return operation != null ? operation.equals(otherOperation)
                    : otherListener != null && listener.asBinder() == otherListener.asBinder();
        }
    }

    private final Binder mBinder = new Binder();
    /** The app's alarms; under the service's lock. */
    private final List<Alarm> mAlarms = new ArrayList<>();

    private LocalAlarmManager() {
        mBinder.attachInterface(this, IAlarmManager.Stub.DESCRIPTOR);
    }

    /** ServiceManager's maker for "alarm": the Binder that carries a new service. */
    public static IBinder newBinder() {
        return new LocalAlarmManager().asBinder();
    }

    @Override
    public IBinder asBinder() {
        return mBinder;
    }

    @Override
    public void set(String callingPackage, int type, long triggerAtTime, long windowLength, long interval, int flags,
            PendingIntent operation, IAlarmListener listener, String listenerTag, WorkSource workSource,
            AlarmManager.AlarmClockInfo alarmClock) {
        if ((operation == null) == (listener == null)) {
            // As AlarmManagerService's set: logged and dropped.
            return;
        }
        if (type < AlarmManager.RTC_WAKEUP || type > AlarmManager.ELAPSED_REALTIME) {
            throw new IllegalArgumentException("Invalid alarm type " + type);
        }
        boolean exact = windowLength == AlarmManager.WINDOW_EXACT;
        if ((alarmClock != null || (exact && operation != null)) && !canScheduleExact(callingPackage)) {
            throw new SecurityException("Caller " + callingPackage + " needs to hold "
                    + Manifest.permission.SCHEDULE_EXACT_ALARM + " or " + Manifest.permission.USE_EXACT_ALARM
                    + " to set exact alarms.");
        }
        synchronized (this) {
            mAlarms.removeIf(alarm -> alarm.matches(operation, listener));
            if (mAlarms.size() >= MAX_ALARMS_PER_UID) {
                throw new IllegalStateException("Maximum limit of concurrent alarms " + MAX_ALARMS_PER_UID
                        + " reached for uid: " + android.os.UserHandle.formatUid(Binder.getCallingUid())
                        + ", callingPackage: " + callingPackage);
            }
            mAlarms.add(new Alarm(callingPackage, type, Math.max(triggerAtTime, 0), operation, listener,
                    alarmClock));
        }
    }

    @Override
    public boolean setTime(long millis) {
        throw new SecurityException("Access denied, requires: " + Manifest.permission.SET_TIME);
    }

    @Override
    public void setTimeZone(String zone) {
        throw new SecurityException("Access denied, requires: " + Manifest.permission.SET_TIME_ZONE);
    }

    @Override
    public void remove(PendingIntent operation, IAlarmListener listener) {
        if (operation == null && listener == null) {
            return;
        }
        synchronized (this) {
            mAlarms.removeIf(alarm -> alarm.matches(operation, listener));
        }
    }

    @Override
    public void removeAll(String packageName) {
        synchronized (this) {
            mAlarms.removeIf(alarm -> Objects.equals(alarm.packageName, packageName));
        }
    }

    @Override
    public AlarmManager.AlarmClockInfo getNextAlarmClock(int userId) {
        synchronized (this) {
            AlarmManager.AlarmClockInfo next = null;
            for (Alarm alarm : mAlarms) {
                if (alarm.alarmClock != null && (next == null
                        || alarm.alarmClock.getTriggerTime() < next.getTriggerTime())) {
                    next = alarm.alarmClock;
                }
            }
            return next;
        }
    }

    @Override
    public long getNextWakeFromIdleTime() {
        return Long.MAX_VALUE;
    }

    @Override
    public boolean canScheduleExactAlarms(String packageName) {
        return canScheduleExact(packageName);
    }

    @Override
    public boolean hasScheduleExactAlarm(String packageName, int userId) {
        return hasPermission(Manifest.permission.SCHEDULE_EXACT_ALARM) && targetSdk() < Build.VERSION_CODES.TIRAMISU;
    }

    /**
     * As AlarmManagerService's canScheduleExactAlarms for an app: USE_EXACT_ALARM, which an app that declares it holds,
     * or SCHEDULE_EXACT_ALARM, whose app op Android 14 and later deny by default to an app that targets Android 13 or
     * later [inferred], or a target before Android 12, where exact alarms needed nothing.
     */
    private static boolean canScheduleExact(String packageName) {
        return targetSdk() < Build.VERSION_CODES.S || hasPermission(Manifest.permission.USE_EXACT_ALARM)
                || (hasPermission(Manifest.permission.SCHEDULE_EXACT_ALARM)
                        && targetSdk() < Build.VERSION_CODES.TIRAMISU);
    }

    private static boolean hasPermission(String permission) {
        Application app = ActivityThread.currentApplication();
        return app != null && app.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED;
    }

    private static int targetSdk() {
        Application app = ActivityThread.currentApplication();
        return app != null ? app.getApplicationInfo().targetSdkVersion : Build.VERSION.SDK_INT;
    }
}
