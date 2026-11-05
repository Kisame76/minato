/*
 * The USB service (gap 1 of the compatibility plan, batch A): IUsbManager of system_server, local in the app's process,
 * which ServiceManager's table gives as "usb" and AOSP's UsbManager asks. It answers as a phone does that has nothing
 * plugged in and no USB host or accessory mode in use [read: UsbService, android-15.0.0_r1]: the device list is empty,
 * there are no accessories and no ports, and no device is connected. Every other call is AIDL's Default answer. Not
 * under com.android.minato, whose classes tools/framework-host-check.sh compiles on the host without android.*.
 */
package android.hardware.usb;

import android.os.Binder;
import android.os.IBinder;

public final class LocalUsbManager extends IUsbManager.Default {
    private final Binder mBinder = new Binder();

    private LocalUsbManager() {
        mBinder.attachInterface(this, IUsbManager.Stub.DESCRIPTOR);
    }

    /** ServiceManager's maker for "usb": the Binder that carries a new service. */
    public static IBinder newBinder() {
        return new LocalUsbManager().asBinder();
    }

    @Override
    public IBinder asBinder() {
        return mBinder;
    }
}
