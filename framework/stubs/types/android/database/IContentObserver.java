package android.database;

/**
 * Compile-only stub (ADR-0039 step c2c1), never in framework.jar: a remote content observer, which
 * ContentProviderNative unparcels; no other process calls a provider on Minato.
 */
public interface IContentObserver extends android.os.IInterface {
    abstract class Stub extends android.os.Binder implements IContentObserver {
        public static IContentObserver asInterface(android.os.IBinder obj) {
            throw new RuntimeException("stub");
        }
    }
}
