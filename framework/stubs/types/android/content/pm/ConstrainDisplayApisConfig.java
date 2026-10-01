package android.content.pm;

/** Compile-only stub (ADR-0039 b3.1), never in framework.jar: ActivityInfo's display API sandboxing names it. */
public class ConstrainDisplayApisConfig {
    public boolean getNeverConstrainDisplayApis(ApplicationInfo applicationInfo) {
        throw new RuntimeException("stub");
    }

    public boolean getAlwaysConstrainDisplayApis(ApplicationInfo applicationInfo) {
        throw new RuntimeException("stub");
    }
}
