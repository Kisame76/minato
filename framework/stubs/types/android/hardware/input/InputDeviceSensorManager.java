package android.hardware.input;

/**
 * Compile-only stub (ADR-0045, the input service), never in framework.jar: an input device's sensors; Minato's devices
 * have none.
 */
public final class InputDeviceSensorManager {
    public InputDeviceSensorManager(InputManagerGlobal inputManagerGlobal) {
        throw new RuntimeException("stub");
    }

    public android.hardware.SensorManager getSensorManager(int deviceId) {
        throw new RuntimeException("stub");
    }

    public void onInputDeviceAdded(int deviceId) {
        throw new RuntimeException("stub");
    }

    public void onInputDeviceRemoved(int deviceId) {
        throw new RuntimeException("stub");
    }

    public void onInputDeviceChanged(int deviceId) {
        throw new RuntimeException("stub");
    }
}
