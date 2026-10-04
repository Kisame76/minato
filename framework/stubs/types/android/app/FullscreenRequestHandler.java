package android.app;

import android.content.res.Configuration;
import android.os.IBinder;
import android.os.OutcomeReceiver;

/** Compile-only stub (ADR-0039 step c2c3), never in framework.jar: Activity.requestFullscreenMode. */
public class FullscreenRequestHandler {
    static void requestFullscreenMode(int request, OutcomeReceiver<Void, Throwable> approvalCallback,
            Configuration config, IBinder token) {
        throw new RuntimeException("stub");
    }
}
