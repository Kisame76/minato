package android.hardware;

/**
 * Compile-only stub (ADR-0045, the start's sensors), never in framework.jar: SensorManager's helper for the
 * SensorListener API, deprecated since API 3, which needs the window manager's rotation watcher.
 */
final class LegacySensorManager {
    LegacySensorManager(SensorManager sensorManager) {
        throw new RuntimeException("stub");
    }

    public int getSensors() {
        throw new RuntimeException("stub");
    }

    public boolean registerListener(SensorListener listener, int sensors, int rate) {
        throw new RuntimeException("stub");
    }

    public void unregisterListener(SensorListener listener, int sensors) {
        throw new RuntimeException("stub");
    }
}
