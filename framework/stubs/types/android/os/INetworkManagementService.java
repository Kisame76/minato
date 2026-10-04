package android.os;

/**
 * Compile-only stub (ADR-0039 step c1), never in framework.jar: the network management service's AIDL
 * interface, which StrictMode asks for detectCleartextNetwork's per-uid policy.
 */
public interface INetworkManagementService {
    void setUidCleartextNetworkPolicy(int uid, int policy) throws RemoteException;

    abstract class Stub {
        public static INetworkManagementService asInterface(IBinder obj) {
            throw new RuntimeException("stub");
        }
    }
}
