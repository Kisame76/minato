package android.hardware.usb.gadget;

/** Compile-only stub, never in framework.jar: the USB gadget HAL's function bits, which UsbManager only names. */
public final class GadgetFunction {
    public static final long NONE = 0;
    public static final long ADB = 1;
    public static final long ACCESSORY = 2;
    public static final long MTP = 4;
    public static final long MIDI = 8;
    public static final long PTP = 16;
    public static final long RNDIS = 32;
    public static final long AUDIO_SOURCE = 64;
    public static final long NCM = 1024;
}
