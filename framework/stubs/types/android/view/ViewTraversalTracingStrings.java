package android.view;

/**
 * Compile-only stub (ADR-0039 step c2c2), never in framework.jar: View's trace section names, only when view tracing is
 * on.
 */
public class ViewTraversalTracingStrings {
    public String onMeasure;

    public String onLayout;

    public String classSimpleName;

    public String requestLayoutStacktracePrefix;

    public String onMeasureBeforeLayout;

    ViewTraversalTracingStrings(View v) {
        throw new RuntimeException("stub");
    }
}
