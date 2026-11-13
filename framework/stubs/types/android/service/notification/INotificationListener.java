package android.service.notification;

import android.os.IBinder;

/**
 * Compile-only stub (ADR-0045), never in framework.jar: a notification listener's AIDL interface, which
 * INotificationManager's Stub and Proxy pass; no app on Minato listens to other apps' notifications.
 */
public interface INotificationListener extends android.os.IInterface {
    abstract class Stub {
        public static INotificationListener asInterface(IBinder obj) {
            throw new RuntimeException("stub");
        }
    }
}
