package io.github.kisame76.minato.test.alarm;

import android.app.AlarmManager;
import android.app.Application;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.os.Parcel;
import android.util.Log;

/**
 * The alarm manager (ADR-0045: alarms kept, none run; batch D 3), as an app without SCHEDULE_EXACT_ALARM or
 * USE_EXACT_ALARM meets it: the inexact alarms of every kind, with a PendingIntent and with a listener, are taken; the
 * exact ones and the alarm clock are refused; the checks of AlarmManager itself; cancelling; the next alarm clock; the
 * clock and time zone, which need system permissions; AlarmClockInfo and the constants. Every alarm lies a day ahead
 * and is cancelled at the end, so that none goes off on Android either.
 */
public class AlarmApplication extends Application {
    static final String TAG = "alarm-test";
    static final long DAY = AlarmManager.INTERVAL_DAY;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private interface Action {
        Object run() throws Exception;
    }

    private static String outcome(Action action) {
        try {
            Object result = action.run();
            return result == null ? "ok" : String.valueOf(result);
        } catch (Throwable e) {
            return e.getClass().getName() + ": " + e.getMessage();
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        Object service = getSystemService(Context.ALARM_SERVICE);
        log("service: " + (service == null ? "null" : service.getClass().getName()));
        AlarmManager alarms = (AlarmManager) service;
        log("same again: " + (getSystemService(AlarmManager.class) == alarms));
        log("can schedule exact alarms: " + alarms.canScheduleExactAlarms());

        PendingIntent operation = PendingIntent.getBroadcast(this, 1,
                new Intent("io.github.kisame76.minato.test.alarm.ALARM").setPackage(getPackageName()),
                PendingIntent.FLAG_IMMUTABLE);
        PendingIntent other = PendingIntent.getBroadcast(this, 2,
                new Intent("io.github.kisame76.minato.test.alarm.OTHER").setPackage(getPackageName()),
                PendingIntent.FLAG_IMMUTABLE);
        AlarmManager.OnAlarmListener listener = () -> log("the listener's alarm went off");
        long wall = System.currentTimeMillis() + DAY;
        long elapsed = android.os.SystemClock.elapsedRealtime() + DAY;

        log("set RTC_WAKEUP: " + outcome(() -> {
            alarms.set(AlarmManager.RTC_WAKEUP, wall, operation);
            return null;
        }));
        log("set ELAPSED_REALTIME with a listener: " + outcome(() -> {
            alarms.set(AlarmManager.ELAPSED_REALTIME, elapsed, "alarm-test", listener, null);
            return null;
        }));
        log("setWindow: " + outcome(() -> {
            alarms.setWindow(AlarmManager.RTC, wall, 60_000L, other);
            return null;
        }));
        log("setRepeating: " + outcome(() -> {
            alarms.setRepeating(AlarmManager.ELAPSED_REALTIME_WAKEUP, elapsed, AlarmManager.INTERVAL_HOUR, other);
            return null;
        }));
        log("setInexactRepeating: " + outcome(() -> {
            alarms.setInexactRepeating(AlarmManager.RTC, wall, AlarmManager.INTERVAL_HALF_DAY, other);
            return null;
        }));
        log("setAndAllowWhileIdle: " + outcome(() -> {
            alarms.setAndAllowWhileIdle(AlarmManager.ELAPSED_REALTIME_WAKEUP, elapsed, other);
            return null;
        }));
        log("a negative trigger time: " + outcome(() -> {
            alarms.set(AlarmManager.RTC, -5L, other);
            return null;
        }));
        log("an unknown type: " + outcome(() -> {
            alarms.set(7, wall, other);
            return null;
        }));
        log("no operation and no listener: " + outcome(() -> {
            alarms.set(AlarmManager.RTC, wall, (PendingIntent) null);
            return null;
        }));

        log("setExact: " + outcome(() -> {
            alarms.setExact(AlarmManager.RTC_WAKEUP, wall, operation);
            return null;
        }));
        log("setExact with a listener: " + outcome(() -> {
            alarms.setExact(AlarmManager.ELAPSED_REALTIME, elapsed, "alarm-test", listener, null);
            return null;
        }));
        log("setExactAndAllowWhileIdle: " + outcome(() -> {
            alarms.setExactAndAllowWhileIdle(AlarmManager.RTC_WAKEUP, wall, operation);
            return null;
        }));
        log("setAlarmClock: " + outcome(() -> {
            alarms.setAlarmClock(new AlarmManager.AlarmClockInfo(wall, null), operation);
            return null;
        }));
        log("next alarm clock: " + alarms.getNextAlarmClock());

        log("cancel a null PendingIntent: " + outcome(() -> {
            alarms.cancel((PendingIntent) null);
            return null;
        }));
        log("cancel a null listener: " + outcome(() -> {
            alarms.cancel((AlarmManager.OnAlarmListener) null);
            return null;
        }));
        log("cancel the operation: " + outcome(() -> {
            alarms.cancel(operation);
            return null;
        }));
        log("cancel the listener: " + outcome(() -> {
            alarms.cancel(listener);
            return null;
        }));
        log("cancel a listener never set: " + outcome(() -> {
            alarms.cancel(() -> { });
            return null;
        }));
        log("cancelAll: " + outcome(() -> {
            alarms.cancelAll();
            return null;
        }));

        log("setTime: " + outcome(() -> {
            alarms.setTime(0L);
            return null;
        }).replaceAll("uid \\d+", "uid <app>"));
        log("setTimeZone of an empty ID: " + outcome(() -> {
            alarms.setTimeZone("");
            return null;
        }));
        log("setTimeZone of no Olson ID: " + outcome(() -> {
            alarms.setTimeZone("Nowhere/Nothing");
            return null;
        }));
        log("setTimeZone: " + outcome(() -> {
            alarms.setTimeZone("Europe/Berlin");
            return null;
        }).replaceAll("uid \\d+", "uid <app>"));

        AlarmManager.AlarmClockInfo info = new AlarmManager.AlarmClockInfo(1234L, operation);
        Parcel parcel = Parcel.obtain();
        info.writeToParcel(parcel, 0);
        parcel.setDataPosition(0);
        AlarmManager.AlarmClockInfo copy = AlarmManager.AlarmClockInfo.CREATOR.createFromParcel(parcel);
        parcel.recycle();
        log("AlarmClockInfo: trigger " + info.getTriggerTime() + ", show intent " + (info.getShowIntent() == operation)
                + ", copy's trigger " + copy.getTriggerTime() + ", copy's intent equal " + operation.equals(
                copy.getShowIntent()) + ", contents " + info.describeContents());
        log("types: " + AlarmManager.RTC_WAKEUP + " " + AlarmManager.RTC + " " + AlarmManager.ELAPSED_REALTIME_WAKEUP
                + " " + AlarmManager.ELAPSED_REALTIME);
        log("intervals: " + AlarmManager.INTERVAL_FIFTEEN_MINUTES + " " + AlarmManager.INTERVAL_HALF_HOUR + " "
                + AlarmManager.INTERVAL_HOUR + " " + AlarmManager.INTERVAL_HALF_DAY + " " + AlarmManager.INTERVAL_DAY);
        log("actions: " + AlarmManager.ACTION_NEXT_ALARM_CLOCK_CHANGED + " "
                + AlarmManager.ACTION_SCHEDULE_EXACT_ALARM_PERMISSION_STATE_CHANGED);
        log("done");
    }
}
