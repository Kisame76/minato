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
 * The alarms live as long as the process and go off while it lives (the compatibility plan's decision 6): a daemon
 * thread waits for the earliest, and fires the PendingIntent or calls the listener. Android's service raises every
 * alarm due in less than 5 s (MIN_FUTURITY) to 5 s [measured: run-test on apk-runner-35, an exact listener alarm
 * at +3 s went off at +5 s with the inexact ones], and a repeating one repeats after its interval, 60 s at least
 * [read: AlarmManagerService MIN_INTERVAL]. Nothing goes off while iOS suspends the app or after it ends. Not checked:
 * the quotas of allow-while-idle alarms, the minimum window and interval, which only change when Android delivers, and
 * work sources, which only the system names. Every other call is AIDL's Default answer. Not under
 * com.android.minato, whose classes tools/framework-host-check.sh compiles on the host without android.*.
 */
package android.app;

import android.Manifest;
import android.content.pm.PackageManager;
import android.os.Binder;
import android.os.Build;
import android.os.IBinder;
import android.os.SystemClock;
import android.os.WorkSource;
import android.util.Log;

import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;
import java.util.Objects;

public final class LocalAlarmManager extends IAlarmManager.Default {
    /** AlarmManagerService's default MAX_ALARMS_PER_UID: the count before a new alarm is added must stay below it. */
    private static final int MAX_ALARMS_PER_UID = 500;
    /** AlarmManagerService's MIN_FUTURITY: an alarm due sooner is raised to this many milliseconds from now. */
    private static final long MIN_FUTURITY = 5_000;
    /** AlarmManagerService's MIN_INTERVAL: a repeating alarm repeats after at least this many milliseconds. */
    private static final long MIN_INTERVAL = 60_000;
    private static final String TAG = "LocalAlarmManager";

    /** An alarm kept: its package, type, trigger time, and its PendingIntent or listener. */
    private static final class Alarm {
        final String packageName;
        final int type;
        final long triggerAtTime;
        final PendingIntent operation;
        final IAlarmListener listener;
        final AlarmManager.AlarmClockInfo alarmClock;
        /** The time it goes off, in the type's clock (elapsed or wall); a repeating alarm moves it on. */
        long effective;
        /** The repeat interval in milliseconds, 0 for a one-shot. */
        final long interval;

        Alarm(String packageName, int type, long triggerAtTime, PendingIntent operation, IAlarmListener listener,
                AlarmManager.AlarmClockInfo alarmClock, long effective, long interval) {
            this.packageName = packageName;
            this.type = type;
            this.triggerAtTime = triggerAtTime;
            this.operation = operation;
            this.listener = listener;
            this.alarmClock = alarmClock;
            this.effective = effective;
            this.interval = interval;
        }

        /** Whether the type counts the wall clock (RTC) and not the time since boot. */
        boolean wall() {
            return type == AlarmManager.RTC_WAKEUP || type == AlarmManager.RTC;
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
        final Thread scheduler = new Thread(this::run, "AlarmScheduler");
        scheduler.setDaemon(true);
        scheduler.start();
    }

    /** The scheduler: waits for the earliest alarm, fires what is due, repeats what repeats. */
    private void run() {
        while (true) {
            final List<Alarm> due = new ArrayList<>();
            synchronized (this) {
                final long elapsed = SystemClock.elapsedRealtime();
                final long wall = System.currentTimeMillis();
                long wait = 1_000;  // the wall clock may be set: look again at least every second
                for (Iterator<Alarm> iterator = mAlarms.iterator(); iterator.hasNext();) {
                    final Alarm alarm = iterator.next();
                    final long now = alarm.wall() ? wall : elapsed;
                    if (alarm.effective > now) {
                        wait = Math.min(wait, alarm.effective - now);
                        continue;
                    }
                    due.add(alarm);
                    if (alarm.interval > 0) {
                        alarm.effective += alarm.interval * (1 + (now - alarm.effective) / alarm.interval);
                    } else {
                        iterator.remove();
                    }
                }
                if (due.isEmpty()) {
                    try {
                        wait(Math.max(wait, 1));
                    } catch (InterruptedException e) {
                        return;
                    }
                    continue;
                }
            }
            for (Alarm alarm : due) {
                fire(alarm);
            }
        }
    }

    /** Goes off: the PendingIntent is sent (one that was cancelled drops the alarm), the listener is called. */
    private void fire(Alarm alarm) {
        try {
            if (alarm.operation != null) {
                alarm.operation.send();
            } else {
                alarm.listener.doAlarm(new IAlarmCompleteListener.Stub() {
                    @Override
                    public void alarmComplete(IBinder who) {
                    }
                });
            }
        } catch (PendingIntent.CanceledException | android.os.RemoteException e) {
            Log.w(TAG, "alarm dropped: " + e);
            synchronized (this) {
                mAlarms.remove(alarm);
            }
        }
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
            final boolean wall = type == AlarmManager.RTC_WAKEUP || type == AlarmManager.RTC;
            final long now = wall ? System.currentTimeMillis() : SystemClock.elapsedRealtime();
            final long period = interval > 0 ? Math.max(interval, MIN_INTERVAL) : 0;
            mAlarms.add(new Alarm(callingPackage, type, Math.max(triggerAtTime, 0), operation, listener, alarmClock,
                    Math.max(triggerAtTime, now + MIN_FUTURITY), period));
            notifyAll();
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
            notifyAll();
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
