package android.os;

/** Compile-only stub (ADR-0043 step d)), never in framework.jar: how a handwriting gesture's cancellation reaches an input method. */
public class CancellationSignalBeamer {
    public static class Sender {
        public static IBinder beamFromScope(CancellationSignal cs) {
            throw new RuntimeException("stub");
        }
    }

    public static class Receiver {
        public CancellationSignal unbeam(IBinder token) {
            throw new RuntimeException("stub");
        }

        public void forget(IBinder token) {
            throw new RuntimeException("stub");
        }
    }
}
