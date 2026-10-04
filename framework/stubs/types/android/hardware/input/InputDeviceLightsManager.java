package android.hardware.input;

/**
 * Compile-only stub (ADR-0045, the input service), never in framework.jar: an input device's lights; Minato's devices
 * have none.
 */
public final class InputDeviceLightsManager extends android.hardware.lights.LightsManager {
    public InputDeviceLightsManager(int deviceId) {
        throw new RuntimeException("stub");
    }

    public java.util.List<android.hardware.lights.Light> getLights() {
        throw new RuntimeException("stub");
    }

    public android.hardware.lights.LightState getLightState(android.hardware.lights.Light light) {
        throw new RuntimeException("stub");
    }

    public android.hardware.lights.LightsManager.LightsSession openSession() {
        throw new RuntimeException("stub");
    }
}
