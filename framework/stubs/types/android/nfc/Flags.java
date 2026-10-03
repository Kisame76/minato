package android.nfc;

/**
 * Compile-only stub (ADR-0039 b3.5a), never in framework.jar: PackageManager's @FlaggedApi names this flag of the NFC
 * module, which Minato does not build.
 */
public final class Flags {
    public static final String FLAG_ENABLE_NFC_CHARGING = "android.nfc.enable_nfc_charging";
}
