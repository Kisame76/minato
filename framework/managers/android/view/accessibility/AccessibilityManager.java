// The written part of AccessibilityManager (ADR-0045 decision 3, ADR-0039 step c2c2, framework breadth F3), which
// tools/managergen joins with the API's methods: the hidden getInstance that View and ViewGroup ask whether
// accessibility is on, one manager per process as AOSP's sInstance, and the answers of Android 15's manager when no
// accessibility service is enabled [verified: date-time-widgets-test on apk-runner-35]. Minato runs no accessibility
// service (ADR-0041) and has no IAccessibilityManager: the state never changes, so listeners are kept and never
// called; no service is installed, so the lists are empty (the emulator image lists one installed service, a
// departure); events cannot be sent while accessibility is off, as on Android (an exception on the main thread, a
// log line elsewhere).
import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.util.ArrayMap;
import android.util.Log;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Executor;

class AccessibilityManager {
    private static AccessibilityManager sInstance;

    private final ArrayMap<AccessibilityStateChangeListener, Handler> mStateListeners = new ArrayMap<>();
    private final ArrayMap<TouchExplorationStateChangeListener, Handler> mTouchListeners = new ArrayMap<>();
    private final ArrayMap<AccessibilityServicesStateChangeListener, Executor> mServicesListeners = new ArrayMap<>();
    private final ArrayMap<AudioDescriptionRequestedChangeListener, Executor> mAudioListeners = new ArrayMap<>();
    private final int mFocusStrokeWidth;
    private final int mFocusColor;

    private AccessibilityManager(Context context) {
        // AOSP's initialFocusAppearanceLocked: the framework resources, or the values hard coded for TalkBack.
        int width;
        int color;
        try {
            width = context.getResources().getDimensionPixelSize(
                    com.android.internal.R.dimen.accessibility_focus_highlight_stroke_width);
            color = context.getResources().getColor(com.android.internal.R.color.accessibility_focus_highlight_color);
        } catch (android.content.res.Resources.NotFoundException e) {
            width = (int) (4 * context.getResources().getDisplayMetrics().density);
            color = 0xbf39b500;
        }
        mFocusStrokeWidth = width;
        mFocusColor = color;
    }

    /** @hide The process's manager, made once as AOSP's keeps sInstance, so each method logs once. */
    public static AccessibilityManager getInstance(Context context) {
        synchronized (AccessibilityManager.class) {
            if (sInstance == null) {
                sInstance = new AccessibilityManager(context);
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

    public boolean isAudioDescriptionRequested() {
        return false;
    }

    public boolean isRequestFromAccessibilityTool() {
        return false;
    }

    /** Accessibility is off: AOSP's check, which throws on the main thread and logs elsewhere. */
    public void interrupt() {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            throw new IllegalStateException("Accessibility off. Did you forget to check that?");
        }
        Log.e("AccessibilityManager", "Interrupt called with accessibility disabled");
    }

    /** Accessibility is off: AOSP's check, which throws on the main thread and logs elsewhere. */
    public void sendAccessibilityEvent(AccessibilityEvent event) {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            throw new IllegalStateException("Accessibility off. Did you forget to check that?");
        }
        Log.e("AccessibilityManager", "AccessibilityEvent sent with accessibility disabled");
    }

    public List<android.accessibilityservice.AccessibilityServiceInfo> getEnabledAccessibilityServiceList(
            int feedbackTypeFlags) {
        return new ArrayList<>();
    }

    public List<android.accessibilityservice.AccessibilityServiceInfo> getInstalledAccessibilityServiceList() {
        return new ArrayList<>();
    }

    @Deprecated
    public List<android.content.pm.ServiceInfo> getAccessibilityServiceList() {
        return new ArrayList<>();
    }

    /** No service sets a UI timeout, so the original one stays. */
    public int getRecommendedTimeoutMillis(int originalTimeout, int uiContentFlags) {
        return originalTimeout;
    }

    public int getAccessibilityFocusStrokeWidth() {
        return mFocusStrokeWidth;
    }

    public int getAccessibilityFocusColor() {
        return mFocusColor;
    }

    public static boolean isAccessibilityButtonSupported() {
        return android.content.res.Resources.getSystem().getBoolean(
                com.android.internal.R.bool.config_showNavigationBar);
    }

    public boolean addAccessibilityStateChangeListener(AccessibilityStateChangeListener listener) {
        addAccessibilityStateChangeListener(listener, null);
        return true;
    }

    public void addAccessibilityStateChangeListener(AccessibilityStateChangeListener listener, Handler handler) {
        synchronized (mStateListeners) {
            mStateListeners.put(listener, handler);
        }
    }

    public boolean removeAccessibilityStateChangeListener(AccessibilityStateChangeListener listener) {
        synchronized (mStateListeners) {
            int index = mStateListeners.indexOfKey(listener);
            mStateListeners.remove(listener);
            return index >= 0;
        }
    }

    public boolean addTouchExplorationStateChangeListener(TouchExplorationStateChangeListener listener) {
        addTouchExplorationStateChangeListener(listener, null);
        return true;
    }

    public void addTouchExplorationStateChangeListener(TouchExplorationStateChangeListener listener, Handler handler) {
        synchronized (mTouchListeners) {
            mTouchListeners.put(listener, handler);
        }
    }

    public boolean removeTouchExplorationStateChangeListener(TouchExplorationStateChangeListener listener) {
        synchronized (mTouchListeners) {
            int index = mTouchListeners.indexOfKey(listener);
            mTouchListeners.remove(listener);
            return index >= 0;
        }
    }

    public void addAccessibilityServicesStateChangeListener(Executor executor,
            AccessibilityServicesStateChangeListener listener) {
        synchronized (mServicesListeners) {
            mServicesListeners.put(listener, executor);
        }
    }

    public void addAccessibilityServicesStateChangeListener(AccessibilityServicesStateChangeListener listener) {
        addAccessibilityServicesStateChangeListener(null, listener);
    }

    public boolean removeAccessibilityServicesStateChangeListener(AccessibilityServicesStateChangeListener listener) {
        synchronized (mServicesListeners) {
            int index = mServicesListeners.indexOfKey(listener);
            mServicesListeners.remove(listener);
            return index >= 0;
        }
    }

    public void addAudioDescriptionRequestedChangeListener(Executor executor,
            AudioDescriptionRequestedChangeListener listener) {
        synchronized (mAudioListeners) {
            mAudioListeners.put(listener, executor);
        }
    }

    public boolean removeAudioDescriptionRequestedChangeListener(AudioDescriptionRequestedChangeListener listener) {
        synchronized (mAudioListeners) {
            int index = mAudioListeners.indexOfKey(listener);
            mAudioListeners.remove(listener);
            return index >= 0;
        }
    }
}
