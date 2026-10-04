// The written part of AccessibilityManager (ADR-0045 decision 3, ADR-0039 step c2c2), which tools/managergen joins
// with the API's methods: the hidden getInstance that View and ViewGroup ask whether accessibility is on, one manager
// per process as AOSP's sInstance, and accessibility off, as Minato runs no accessibility service (ADR-0041).
import android.content.Context;

class AccessibilityManager {
    private static AccessibilityManager sInstance;

    private AccessibilityManager() {
    }

    /** @hide The process's manager, made once as AOSP's keeps sInstance, so each method logs once. */
    public static AccessibilityManager getInstance(Context context) {
        synchronized (AccessibilityManager.class) {
            if (sInstance == null) {
                sInstance = new AccessibilityManager();
            }
            return sInstance;
        }
    }

    /** No accessibility service runs on Minato. */
    public boolean isEnabled() {
        return false;
    }

    /** No accessibility service runs on Minato, so no touch exploration either. */
    public boolean isTouchExplorationEnabled() {
        return false;
    }
}
