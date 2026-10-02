package android.app;

/**
 * Compile-only stub (ADR-0039 b3.4), never in framework.jar: ResourcesManager tells it of a configuration change.
 * Our ApplicationPackageManager comes with b3.5 (decision 7).
 */
public class ApplicationPackageManager {
    public static void configurationChanged() {
        throw new RuntimeException("stub");
    }
}
