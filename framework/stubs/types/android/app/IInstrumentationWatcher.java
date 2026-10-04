package android.app;

/** Compile-only stub (ADR-0039 step c2c3), never in framework.jar: Instrumentation reports test status to it. */
public interface IInstrumentationWatcher extends android.os.IInterface {

    void instrumentationStatus(android.content.ComponentName name, int resultCode, android.os.Bundle results)
            throws android.os.RemoteException;
}
