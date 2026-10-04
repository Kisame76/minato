package android.app;

import android.os.ResultReceiver;
import android.transition.Transition;
import android.view.View;
import android.view.ViewGroup;
import java.util.ArrayList;

/** Compile-only stub (ADR-0039 step c2c3), never in framework.jar: ActivityTransitionState's scene transitions. */
class EnterTransitionCoordinator {
    EnterTransitionCoordinator(Activity activity, ResultReceiver resultReceiver, ArrayList<String> sharedElementNames,
            boolean isReturning, boolean isCrossTask) {
        throw new RuntimeException("stub");
    }

    public boolean cancelEnter() { throw new RuntimeException("stub"); }
    public void forceViewsToAppear() { throw new RuntimeException("stub"); }
    public ViewGroup getDecor() { throw new RuntimeException("stub"); }
    public Transition getEnterViewsTransition() { throw new RuntimeException("stub"); }
    public ArrayList<String> getPendingExitSharedElementNames() { throw new RuntimeException("stub"); }
    boolean isCrossTask() { throw new RuntimeException("stub"); }
    public boolean isReturning() { throw new RuntimeException("stub"); }
    public boolean isTransitionRunning() { throw new RuntimeException("stub"); }
    public boolean isWaitingForRemoteExit() { throw new RuntimeException("stub"); }
    public void namedViewsReady(ArrayList<String> accepted,
            ArrayList<String> localNames) { throw new RuntimeException("stub"); }
    public void runAfterTransitionsComplete(Runnable onTransitionComplete) { throw new RuntimeException("stub"); }
    public void stop() { throw new RuntimeException("stub"); }
    public void viewInstancesReady(ArrayList<String> accepted, ArrayList<String> localNames,
            ArrayList<View> localViews) {
        throw new RuntimeException("stub");
    }
}
