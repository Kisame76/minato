package android.app;

import android.os.IBinder;
import java.util.concurrent.Executor;

/** Compile-only stub (ADR-0039 step c2c3), never in framework.jar: Activity's screen capture callbacks. */
public class ScreenCaptureCallbackHandler {
    public ScreenCaptureCallbackHandler(IBinder activityToken) {
        throw new RuntimeException("stub");
    }

    public void registerScreenCaptureCallback(Executor executor, Activity.ScreenCaptureCallback callback) {
        throw new RuntimeException("stub");
    }

    public void unregisterScreenCaptureCallback(Activity.ScreenCaptureCallback callback) {
        throw new RuntimeException("stub");
    }
}
