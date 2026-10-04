package android.app;

/**
 * Compile-only stub (ADR-0039 step c2c3), never in framework.jar: Instrumentation.execStartActivityAsCaller's app
 * tasks.
 */
public interface IAppTask extends android.os.IInterface {

    int startActivity(android.os.IBinder whoThread, String callingPackage, String callingFeatureId,
            android.content.Intent intent, String resolvedType,
                    android.os.Bundle options) throws android.os.RemoteException;
}
