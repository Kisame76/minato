package android.hardware;

import com.android.minato.services.Unwritten;

/**
 * Placeholder (App, the Unity test APK retest 11): our class under AOSP's
 * hidden name, which only has to load, since a class apps reflect over names it; every body logs once and throws
 * UnsupportedOperationException. Before, a compile-only stub (ADR-0045, the start's sensors): SensorManager's helper
 * for the SensorListener API, deprecated since API 3, which needs the window manager's rotation watcher.
 */
@SuppressWarnings("deprecation") // SensorListener, the API it serves, is deprecated since API 3
final class LegacySensorManager {
    LegacySensorManager(SensorManager sensorManager) {
        throw Unwritten.placeholder("android.hardware.LegacySensorManager(SensorManager)");
    }

    public int getSensors() {
        throw Unwritten.placeholder("android.hardware.LegacySensorManager.getSensors()");
    }

    public boolean registerListener(SensorListener listener, int sensors, int rate) {
        throw Unwritten.placeholder("android.hardware.LegacySensorManager.registerListener(SensorListener,int,int)");
    }

    public void unregisterListener(SensorListener listener, int sensors) {
        throw Unwritten.placeholder("android.hardware.LegacySensorManager.unregisterListener(SensorListener,int)");
    }
}
