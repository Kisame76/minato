package android.hardware.usb.V1_0;

/**
 * Compile-only stub, never in framework.jar: the constants of the USB HAL 1.0 (a HIDL type Minato does not have) that
 * UsbPort reads. javac inlines them; no USB port exists in Minato, so no value reaches a run (gap 1, batch A).
 */
public final class Constants {
    public static final class PortDataRole {
        public static final int NONE = 0;
        public static final int HOST = 1;
        public static final int DEVICE = 2;
        public static final int NUM_DATA_ROLES = 3;
    }

    public static final class PortPowerRole {
        public static final int NONE = 0;
        public static final int SOURCE = 1;
        public static final int SINK = 2;
        public static final int NUM_POWER_ROLES = 3;
    }

    public static final class PortMode {
        public static final int NONE = 0;
        public static final int UFP = 1;
        public static final int DFP = 2;
        public static final int DRP = 3;
        public static final int NUM_MODES = 4;
    }
}
