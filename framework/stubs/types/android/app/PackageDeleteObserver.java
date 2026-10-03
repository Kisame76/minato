package android.app;

import android.content.Intent;
import android.content.pm.IPackageDeleteObserver2;

/**
 * Compile-only stub (ADR-0039 b3.5a), never in framework.jar: PackageManager's LegacyPackageDeleteObserver extends it,
 * for deletions, which no app of Minato makes. framework/stub-supertypes.txt lists that class.
 */
public class PackageDeleteObserver {
    public PackageDeleteObserver() {
        throw new RuntimeException("stub");
    }

    public IPackageDeleteObserver2 getBinder() {
        throw new RuntimeException("stub");
    }

    public void onUserActionRequired(Intent intent) {
        throw new RuntimeException("stub");
    }

    public void onPackageDeleted(String basePackageName, int returnCode, String msg) {
        throw new RuntimeException("stub");
    }
}
