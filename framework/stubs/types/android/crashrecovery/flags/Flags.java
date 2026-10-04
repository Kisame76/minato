package android.crashrecovery.flags;

/**
 * Compile-only stub (ADR-0039 step c1), never in framework.jar: Environment's @FlaggedApi names this flag of the
 * crash recovery module, whose aconfig declarations tools/aconfig-flags.py does not read.
 */
public final class Flags {
    public static final String FLAG_ENABLE_CRASHRECOVERY = "android.crashrecovery.flags.enable_crashrecovery";
}
