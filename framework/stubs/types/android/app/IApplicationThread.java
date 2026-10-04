package android.app;

/**
 * Compile-only stub (ADR-0039 step c1), never in framework.jar: Context's hidden getIApplicationThread names this AIDL
 * interface of the process's connection to system_server, which an app process of Minato has none of.
 */
public interface IApplicationThread extends android.os.IInterface {
}
