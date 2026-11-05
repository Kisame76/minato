package android.window;

import com.android.minato.services.Unwritten;

/**
 * Placeholder (the Unity test APK retest 11; replaced by ADR-0058 b) / category 2): our class under AOSP's
 * hidden name, which only has to load, since a class apps reflect over names it; every body logs once and throws
 * UnsupportedOperationException. Before, a compile-only stub (ADR-0039 step c2c3): WindowManager's register and
 * unregisterTaskFpsCallback take it.
 */
public abstract class TaskFpsCallback {
    TaskFpsCallback() {
        throw Unwritten.placeholder("android.window.TaskFpsCallback()");
    }

}
