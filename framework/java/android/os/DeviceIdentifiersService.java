/*
 * The device identifiers policy service (ADR-0039 step c2d3; ADR-0045 decisions 2 and 3): the
 * IDeviceIdentifiersPolicyService of system_server, local in the app's process, which ServiceManager's table gives as
 * "device_identifiers" and Build.getSerial asks. It answers an app as Android 15's answers one that does not hold
 * READ_PRIVILEGED_PHONE_STATE, which no app does: both calls throw SecurityException with TelephonyPermissions'
 * text, which names the caller's uid [verified: strictmode-test on the AVD, target SDK 35]. A named gap: Android gives
 * an app of target SDK below 29 that holds READ_PHONE_STATE "unknown" instead [read: TelephonyPermissions at the tag].
 * It is AIDL's Default class, as the display service is. Not under com.android.minato, whose classes
 * tools/framework-host-check.sh compiles on the host without android.*.
 */
package android.os;

public final class DeviceIdentifiersService extends IDeviceIdentifiersPolicyService.Default {
    private final Binder mBinder = new Binder();

    private DeviceIdentifiersService() {
        mBinder.attachInterface(this, IDeviceIdentifiersPolicyService.Stub.DESCRIPTOR);
    }

    /** ServiceManager's maker for "device_identifiers": the Binder that carries a new service. */
    public static IBinder newBinder() {
        return new DeviceIdentifiersService().asBinder();
    }

    @Override
    public IBinder asBinder() {
        return mBinder;
    }

    @Override
    public String getSerial() {
        throw denied();
    }

    @Override
    public String getSerialForPackage(String callingPackage, String callingFeatureId) {
        throw denied();
    }

    private static SecurityException denied() {
        return new SecurityException("getSerial: The uid " + Binder.getCallingUid()
                + " does not meet the requirements to access device identifiers.");
    }
}
