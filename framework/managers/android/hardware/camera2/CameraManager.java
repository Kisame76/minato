// The written part of CameraManager (the Unity test APK probe's gap sort, category 2; Unity's Camera2Wrapper), which
// tools/managergen joins with the API's methods and constants. Minato answers as a device without cameras, as
// ADR-0045 decision 4 has it for the features (no android.hardware.camera): Android 15's CameraManager there, whose
// cameraserver lists no camera [verified: CameraManager.java at android-15.0.0_r1, webview-camera-test on
// apk-runner-35-bare]. The ID list and the concurrent sets are empty; a camera's characteristics, its opening (which
// asks the characteristics first) and the torch refuse every ID with cameraserver's IllegalArgumentException, its
// message as the service writes it; availability and torch callbacks are taken and never called, as no camera comes
// or goes. Every other method is the generated one, logged once with its default.
import android.content.Context;
import android.hardware.camera2.CameraDevice;
import android.os.Handler;
import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.Executor;

class CameraManager {
    private final Context mContext;

    /** @hide AOSP's: what CameraCharacteristics listens with for the device's fold state (ADR-0073 step a). */
    public interface DeviceStateListener {
        void onDeviceStateChanged(boolean folded);
    }

    /** @hide SystemServiceRegistry's, with the context it builds the manager for. */
    public CameraManager(Context context) {
        mContext = context;
    }

    public String[] getCameraIdList() {
        return new String[0];
    }

    public Set<Set<String>> getConcurrentCameraIds() {
        return new HashSet<>();
    }

    /** @hide AOSP's asks the cameras cameraserver lists for a hidden physical camera; there are no cameras. */
    public static boolean isHiddenPhysicalCamera(String cameraId) {
        return false;
    }

    private static IllegalArgumentException unknownCharacteristics(String cameraId) {
        return new IllegalArgumentException("getCameraCharacteristics:1329: Unable to retrieve camera characteristics"
                + " for unknown device " + cameraId + ": No such file or directory (-2)");
    }

    public CameraCharacteristics getCameraCharacteristics(String cameraId) throws CameraAccessException {
        throw unknownCharacteristics(cameraId);
    }

    public void openCamera(String cameraId, CameraDevice.StateCallback callback, Handler handler)
            throws CameraAccessException {
        open(cameraId, callback);
    }

    public void openCamera(String cameraId, Executor executor, CameraDevice.StateCallback callback)
            throws CameraAccessException {
        if (executor == null) {
            throw new IllegalArgumentException("executor was null");
        }
        open(cameraId, callback);
    }

    /** As openCameraForUid's checks, then the characteristics that its connection asks first. */
    private static void open(String cameraId, CameraDevice.StateCallback callback) {
        if (cameraId == null) {
            throw new IllegalArgumentException("cameraId was null");
        } else if (callback == null) {
            throw new IllegalArgumentException("callback was null");
        }
        throw unknownCharacteristics(cameraId);
    }

    public void setTorchMode(String cameraId, boolean enabled) throws CameraAccessException {
        if (cameraId == null) {
            throw new IllegalArgumentException("cameraId was null");
        }
        throw new IllegalArgumentException("setTorchMode:2996: Camera ID \"" + cameraId
                + "\" is a not valid camera ID");
    }

    public void registerAvailabilityCallback(AvailabilityCallback callback, Handler handler) {
    }

    public void registerAvailabilityCallback(Executor executor, AvailabilityCallback callback) {
    }

    public void unregisterAvailabilityCallback(AvailabilityCallback callback) {
    }

    public void registerTorchCallback(TorchCallback callback, Handler handler) {
    }

    public void registerTorchCallback(Executor executor, TorchCallback callback) {
    }

    public void unregisterTorchCallback(TorchCallback callback) {
    }
}
