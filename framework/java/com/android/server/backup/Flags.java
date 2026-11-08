package com.android.server.backup;

/**
 * The backup service's feature flags (services/backup/flags.aconfig, which the fetched sources do not hold) with the
 * values Android 15's release build (ap3a) gives them: all enabled. BackupAgent and BackupRestoreEventLogger ask two of
 * them.
 */
public final class Flags {
    private Flags() {
    }

    public static boolean enableClearPipeAfterRestoreFile() {
        return true;
    }

    public static boolean enableIncreaseDatatypesForAgentLogging() {
        return true;
    }

    public static boolean enableIncreasedBmmLoggingForRestoreAtInstall() {
        return true;
    }

    public static boolean enableMaxSizeWritesToPipes() {
        return true;
    }

    public static boolean enableMetricsSystemBackupAgents() {
        return true;
    }
}
