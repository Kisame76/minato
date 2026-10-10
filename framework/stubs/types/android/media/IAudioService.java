package android.media;

import android.os.IBinder;
import android.os.RemoteException;

/**
 * Compile-only stub (the Unity test APK probe's batch C), never in framework.jar: the audio service's AIDL interface, which the
 * legacy Camera asks whether the camera's shutter sound is forced, for a camera that is open or described, and Minato
 * has no camera.
 */
public interface IAudioService extends android.os.IInterface {
    boolean isCameraSoundForced() throws RemoteException;

    abstract class Stub extends android.os.Binder implements IAudioService {
        public static IAudioService asInterface(IBinder obj) {
            throw new RuntimeException("stub");
        }
    }
}
