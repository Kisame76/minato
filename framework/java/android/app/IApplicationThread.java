package android.app;

/**
 * Our IApplicationThread (ADR-0039 step c2c3): the process's binder type, which Activity hands to Instrumentation's
 * execStartActivity. AOSP's is the AIDL interface system_server calls to drive the process; Minato has no
 * system_server, so nothing calls it and it has no methods.
 */
public interface IApplicationThread extends android.os.IInterface {
    abstract class Stub extends android.os.Binder implements IApplicationThread {
        @Override
        public android.os.IBinder asBinder() {
            return this;
        }

        /** As AOSP's for a binder of this process: the interface itself, null for none (nothing is remote). */
        public static IApplicationThread asInterface(android.os.IBinder obj) {
            return obj instanceof IApplicationThread ? (IApplicationThread) obj : null;
        }
    }
}
