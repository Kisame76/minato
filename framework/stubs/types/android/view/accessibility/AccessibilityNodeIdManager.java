package android.view.accessibility;

import android.view.View;

/**
 * Compile-only stub (ADR-0039 step c2c2), never in framework.jar: View registers its accessibility ID at attach;
 * accessibility is off (ADR-0041).
 */
public final class AccessibilityNodeIdManager {
    public static synchronized AccessibilityNodeIdManager getInstance() {
        throw new RuntimeException("stub");
    }

    public void registerViewWithId(View view, int id) {
        throw new RuntimeException("stub");
    }

    public void unregisterViewWithId(int id) {
        throw new RuntimeException("stub");
    }
}
