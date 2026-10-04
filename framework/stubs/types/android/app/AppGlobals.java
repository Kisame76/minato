package android.app;

/**
 * Compile-only stub (ADR-0039 step c1), never in framework.jar: FileUtils.convertToModernFd asks it for
 * MediaProvider's transcoding, which no Minato process has; Environment.isExternalStorageLegacy asks it for the app;
 * Intent and IContentProvider's defaults ask its package service (step c2c1).
 */
public class AppGlobals {
    public static String getInitialPackage() {
        throw new RuntimeException("stub");
    }

    public static Application getInitialApplication() {
        throw new RuntimeException("stub");
    }

    public static android.content.pm.IPackageManager getPackageManager() {
        throw new RuntimeException("stub");
    }
}
