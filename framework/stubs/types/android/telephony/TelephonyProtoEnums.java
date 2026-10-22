package android.telephony;

/**
 * Compile-only stub, never in framework.jar: the signal levels that CellSignalStrength takes into its own constants
 * (protoc generates TelephonyProtoEnums from the stats logging protos, which are not among Minato's sources). The
 * values are the ones the API gives CellSignalStrength's SIGNAL_STRENGTH_* fields; javac inlines them.
 */
public final class TelephonyProtoEnums {
    private TelephonyProtoEnums() {
    }

    public static final int SIGNAL_STRENGTH_NONE_OR_UNKNOWN = 0;
    public static final int SIGNAL_STRENGTH_POOR = 1;
    public static final int SIGNAL_STRENGTH_MODERATE = 2;
    public static final int SIGNAL_STRENGTH_GOOD = 3;
    public static final int SIGNAL_STRENGTH_GREAT = 4;
}
