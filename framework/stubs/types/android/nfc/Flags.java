package android.nfc;

/**
 * Compile-only stub (ADR-0039 b3.5a), never in framework.jar: PackageManager's and UserManager's @FlaggedApi name
 * these flags of the NFC module, which Minato does not build.
 */
public final class Flags {
    public static final String FLAG_ENABLE_NFC_CHARGING = "android.nfc.enable_nfc_charging";

    public static final String FLAG_ENABLE_NFC_MAINLINE = "android.nfc.enable_nfc_mainline";

    public static final String FLAG_ENABLE_NFC_USER_RESTRICTION = "android.nfc.enable_nfc_user_restriction";
}
