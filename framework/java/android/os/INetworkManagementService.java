/*
 * The network management service's interface (ADR-0039 step c2d3): our class under AOSP's name, with the one method
 * of INetworkManagementService.aidl that framework.jar calls, StrictMode.setVmPolicy's cleartext policy for the app's
 * uid, and a Stub whose asInterface is AIDL's for it. AOSP's AIDL would bring five network types that framework.jar
 * lacks for nothing else. Minato has no netd: ServiceManager's table answers "network_management" with null, so
 * asInterface gives null, as for a missing service, and StrictMode logs its warning only for a cleartext policy,
 * which Minato does not enforce.
 */
package android.os;

public interface INetworkManagementService extends IInterface {
    /** The rules for the uid's cleartext traffic: StrictMode's NETWORK_POLICY_ACCEPT, _LOG or _REJECT. */
    void setUidCleartextNetworkPolicy(int uid, int policy) throws RemoteException;

    /** As AIDL's: the service carried by a Binder under the interface's descriptor. */
    abstract class Stub extends Binder implements INetworkManagementService {
        public static final String DESCRIPTOR = "android.os.INetworkManagementService";

        public Stub() {
            attachInterface(this, DESCRIPTOR);
        }

        /** As AIDL's: the local service in {@code binder}, null for none; Minato has no other process to proxy. */
        public static INetworkManagementService asInterface(IBinder binder) {
            if (binder == null) {
                return null;
            }
            IInterface local = binder.queryLocalInterface(DESCRIPTOR);
            return local instanceof INetworkManagementService ? (INetworkManagementService) local : null;
        }

        @Override
        public IBinder asBinder() {
            return this;
        }
    }
}
