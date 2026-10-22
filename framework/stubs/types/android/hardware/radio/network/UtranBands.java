package android.hardware.radio.network;

/**
 * Compile-only stub, never in framework.jar: the radio HAL's constants that AccessNetworkConstants takes into its own
 * (AIDL generates UtranBands, which is not among Minato's sources). The values are the ones the API gives
 * AccessNetworkConstants.UtranBand; javac inlines them, so nothing of this class remains
 * in AccessNetworkConstants.
 */
public final class UtranBands {
    private UtranBands() {
    }

    public static final int BAND_1 = 1;
    public static final int BAND_2 = 2;
    public static final int BAND_3 = 3;
    public static final int BAND_4 = 4;
    public static final int BAND_5 = 5;
    public static final int BAND_6 = 6;
    public static final int BAND_7 = 7;
    public static final int BAND_8 = 8;
    public static final int BAND_9 = 9;
    public static final int BAND_10 = 10;
    public static final int BAND_11 = 11;
    public static final int BAND_12 = 12;
    public static final int BAND_13 = 13;
    public static final int BAND_14 = 14;
    public static final int BAND_19 = 19;
    public static final int BAND_20 = 20;
    public static final int BAND_21 = 21;
    public static final int BAND_22 = 22;
    public static final int BAND_25 = 25;
    public static final int BAND_26 = 26;
    public static final int BAND_A = 101;
    public static final int BAND_B = 102;
    public static final int BAND_C = 103;
    public static final int BAND_D = 104;
    public static final int BAND_E = 105;
    public static final int BAND_F = 106;
}
