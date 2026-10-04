package android.hardware.input;

/**
 * Compile-only stub (ADR-0045, the start's sensors and the input service), never in framework.jar: Sensor's ctor for
 * an input device's sensor, which only InputManager makes, and the parcelable that IInputManager's Proxy reads.
 */
public final class InputSensorInfo implements android.os.Parcelable {
    public static final android.os.Parcelable.Creator<InputSensorInfo> CREATOR = null;

    public int describeContents() {
        throw new RuntimeException("stub");
    }

    public void writeToParcel(android.os.Parcel dest, int flags) {
        throw new RuntimeException("stub");
    }

    public String getName() {
        throw new RuntimeException("stub");
    }

    public String getVendor() {
        throw new RuntimeException("stub");
    }

    public int getVersion() {
        throw new RuntimeException("stub");
    }

    public int getHandle() {
        throw new RuntimeException("stub");
    }

    public int getType() {
        throw new RuntimeException("stub");
    }

    public float getMaxRange() {
        throw new RuntimeException("stub");
    }

    public float getResolution() {
        throw new RuntimeException("stub");
    }

    public float getPower() {
        throw new RuntimeException("stub");
    }

    public int getMinDelay() {
        throw new RuntimeException("stub");
    }

    public int getFifoReservedEventCount() {
        throw new RuntimeException("stub");
    }

    public int getFifoMaxEventCount() {
        throw new RuntimeException("stub");
    }

    public String getStringType() {
        throw new RuntimeException("stub");
    }

    public String getRequiredPermission() {
        throw new RuntimeException("stub");
    }

    public int getMaxDelay() {
        throw new RuntimeException("stub");
    }

    public int getFlags() {
        throw new RuntimeException("stub");
    }

    public int getId() {
        throw new RuntimeException("stub");
    }
}
