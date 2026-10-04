package android.app;

/**
 * Compile-only stub (ADR-0039 step c1), never in framework.jar: Context's hidden getServiceDispatcher and bindService
 * forms name this AIDL interface of a bound service's connection through system_server.
 */
public interface IServiceConnection extends android.os.IInterface {
}
