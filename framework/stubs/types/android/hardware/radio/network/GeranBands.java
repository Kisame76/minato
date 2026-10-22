package android.hardware.radio.network;

/**
 * Compile-only stub, never in framework.jar: the radio HAL's constants that AccessNetworkConstants takes into its own
 * (AIDL generates GeranBands, which is not among Minato's sources). The values are the ones the API gives
 * AccessNetworkConstants.GeranBand; javac inlines them, so nothing of this class remains
 * in AccessNetworkConstants.
 */
public final class GeranBands {
    private GeranBands() {
    }

    public static final int BAND_T380 = 1;
    public static final int BAND_T410 = 2;
    public static final int BAND_450 = 3;
    public static final int BAND_480 = 4;
    public static final int BAND_710 = 5;
    public static final int BAND_750 = 6;
    public static final int BAND_T810 = 7;
    public static final int BAND_850 = 8;
    public static final int BAND_P900 = 9;
    public static final int BAND_E900 = 10;
    public static final int BAND_R900 = 11;
    public static final int BAND_DCS1800 = 12;
    public static final int BAND_PCS1900 = 13;
    public static final int BAND_ER900 = 14;
}
