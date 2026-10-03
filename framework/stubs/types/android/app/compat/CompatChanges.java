package android.app.compat;

import android.os.UserHandle;

/**
 * Compile-only stub (ADR-0039 b3.5a), never in framework.jar: ApplicationInfo.isChangeEnabled asks it, which no
 * taken class calls yet. App compatibility changes come when an app needs them.
 */
public final class CompatChanges {
    public static boolean isChangeEnabled(long changeId, String packageName, UserHandle user) {
        throw new RuntimeException("stub");
    }
}
