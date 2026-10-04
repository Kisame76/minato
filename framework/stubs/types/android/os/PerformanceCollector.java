package android.os;

/** Compile-only stub (ADR-0039 step c2c3), never in framework.jar: Instrumentation's test performance snapshots. */
public class PerformanceCollector {

    public void beginSnapshot(String label) {
        throw new RuntimeException("stub");
    }

    public android.os.Bundle endSnapshot() {
        throw new RuntimeException("stub");
    }
}
