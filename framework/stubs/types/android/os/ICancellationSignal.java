package android.os;

/**
 * Compile-only stub (ADR-0039 step c2c1), never in framework.jar: the remote half of a CancellationSignal, which
 * ContentProvider's transport hands out; no other process calls a provider on Minato.
 */
public interface ICancellationSignal extends android.os.IInterface {
    void cancel() throws android.os.RemoteException;

    abstract class Stub extends android.os.Binder implements ICancellationSignal {
        public static ICancellationSignal asInterface(android.os.IBinder obj) {
            throw new RuntimeException("stub");
        }

        public android.os.IBinder asBinder() {
            throw new RuntimeException("stub");
        }
    }
}
