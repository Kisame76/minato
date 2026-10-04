package android.app;

import android.view.View;
import android.view.Window;
import java.util.ArrayList;

/** Compile-only stub (ADR-0039 step c2c3), never in framework.jar: ActivityTransitionState's scene transitions. */
public class ExitTransitionCoordinator {
    public ExitTransitionCoordinator(ExitTransitionCallbacks exitCallbacks, Window window,
            SharedElementCallback listener,
            ArrayList<String> names, ArrayList<String> accepted, ArrayList<View> mapped, boolean isReturning) {
        throw new RuntimeException("stub");
    }

    public void resetViews() { throw new RuntimeException("stub"); }
    public void startExit() { throw new RuntimeException("stub"); }
    public void startExit(Activity activity) { throw new RuntimeException("stub"); }
    public void stop(Activity activity) { throw new RuntimeException("stub"); }
    public ArrayList<String> getAcceptedNames() { throw new RuntimeException("stub"); }
    public ArrayList<String> getMappedNames() { throw new RuntimeException("stub"); }
    public ArrayList<View> copyMappedViews() { throw new RuntimeException("stub"); }
    public boolean isTransitionRunning() { throw new RuntimeException("stub"); }

    public interface ExitTransitionCallbacks {
    }

    public static class ActivityExitTransitionCallbacks implements ExitTransitionCallbacks {
        ActivityExitTransitionCallbacks(Activity activity) { throw new RuntimeException("stub"); }
    }
}
