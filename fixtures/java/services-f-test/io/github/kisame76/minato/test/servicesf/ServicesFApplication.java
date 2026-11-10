package io.github.kisame76.minato.test.servicesf;

import android.app.Application;
import android.app.StatusBarManager;
import android.app.usage.NetworkStatsManager;
import android.content.Context;
import android.os.HardwarePropertiesManager;
import android.os.PerformanceHintManager;
import android.os.Process;
import android.os.health.HealthStats;
import android.os.health.SystemHealthManager;
import android.util.Log;
import android.view.textservice.TextServicesManager;

/**
 * Services bundle A of the system services gap: what an app reads from PerformanceHintManager, SystemHealthManager,
 * HardwarePropertiesManager, NetworkStatsManager, StatusBarManager and TextServicesManager on a phone without the
 * hardware or the permissions they need. Each section logs its lines on its own, so that one that fails does not
 * hide the others; a Throwable shows as its class name.
 */
public class ServicesFApplication extends Application {
    private static final String TAG = "services-f-test";

    @Override
    public void onCreate() {
        super.onCreate();
        performanceHint();
        systemHealth();
        hardwareProperties();
        networkStats();
        statusBar();
        textServices();
        Log.i(TAG, "done");
    }

    private static void failed(String section, Throwable t) {
        Log.i(TAG, section + ": " + t.getClass().getName());
    }

    private void performanceHint() {
        try {
            final PerformanceHintManager manager = getSystemService(PerformanceHintManager.class);
            Log.i(TAG, "performance hint manager: " + (manager != null));
            Log.i(TAG, "performance hint preferred rate: " + (manager.getPreferredUpdateRateNanos() > 0
                    ? "positive" : String.valueOf(manager.getPreferredUpdateRateNanos())));
            final PerformanceHintManager.Session session = manager.createHintSession(new int[] {Process.myTid()},
                    10_000_000L);
            Log.i(TAG, "performance hint session: " + (session != null));
            if (session != null) {
                session.updateTargetWorkDuration(8_000_000L);
                session.reportActualWorkDuration(7_000_000L);
                session.close();
                Log.i(TAG, "performance hint session used and closed");
            }
        } catch (Throwable t) {
            failed("performance hint", t);
        }
    }

    private void systemHealth() {
        try {
            final SystemHealthManager manager = getSystemService(SystemHealthManager.class);
            Log.i(TAG, "system health manager: " + (manager != null));
            final HealthStats stats = manager.takeMyUidSnapshot();
            Log.i(TAG, "system health my snapshot: " + (stats != null) + ", uid snapshot data type "
                    + (stats == null ? "none" : String.valueOf(stats.getDataType())));
        } catch (Throwable t) {
            failed("system health", t);
        }
    }

    private void hardwareProperties() {
        try {
            final HardwarePropertiesManager manager = getSystemService(HardwarePropertiesManager.class);
            Log.i(TAG, "hardware properties manager: " + (manager != null));
            manager.getCpuUsages();
            Log.i(TAG, "hardware properties cpu usages: allowed");
        } catch (Throwable t) {
            failed("hardware properties cpu usages", t);
        }
        try {
            getSystemService(HardwarePropertiesManager.class).getDeviceTemperatures(
                    HardwarePropertiesManager.DEVICE_TEMPERATURE_CPU, HardwarePropertiesManager.TEMPERATURE_CURRENT);
            Log.i(TAG, "hardware properties temperatures: allowed");
        } catch (Throwable t) {
            failed("hardware properties temperatures", t);
        }
    }

    private void networkStats() {
        try {
            final NetworkStatsManager manager = getSystemService(NetworkStatsManager.class);
            Log.i(TAG, "network stats manager: " + (manager != null));
            manager.querySummaryForDevice(android.net.ConnectivityManager.TYPE_WIFI, null, 0L, 1L);
            Log.i(TAG, "network stats summary: allowed");
        } catch (Throwable t) {
            failed("network stats summary", t);
        }
    }

    private void statusBar() {
        try {
            final Object manager = getSystemService(Context.STATUS_BAR_SERVICE);
            Log.i(TAG, "status bar manager: " + (manager instanceof StatusBarManager));
        } catch (Throwable t) {
            failed("status bar", t);
        }
    }

    private void textServices() {
        try {
            final TextServicesManager manager = getSystemService(TextServicesManager.class);
            Log.i(TAG, "text services manager: " + (manager != null));
        } catch (Throwable t) {
            failed("text services", t);
        }
    }
}
