package android.hardware.radio;

/**
 * Compile-only stub, never in framework.jar: the radio HAL's constants that AccessNetworkConstants takes into its own
 * (AIDL generates AccessNetwork, which is not among Minato's sources). The values are the ones the API gives
 * AccessNetworkConstants.AccessNetworkType; javac inlines them, so nothing of this class remains
 * in AccessNetworkConstants.
 */
public final class AccessNetwork {
    private AccessNetwork() {
    }

    public static final int UNKNOWN = 0;
    public static final int GERAN = 1;
    public static final int UTRAN = 2;
    public static final int EUTRAN = 3;
    public static final int CDMA2000 = 4;
    public static final int IWLAN = 5;
    public static final int NGRAN = 6;
}
