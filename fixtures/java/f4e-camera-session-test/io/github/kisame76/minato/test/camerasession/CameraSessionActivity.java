package io.github.kisame76.minato.test.camerasession;

import android.app.Activity;
import android.graphics.SurfaceTexture;
import android.hardware.camera2.CameraExtensionCharacteristics;
import android.hardware.camera2.CameraExtensionSession;
import android.hardware.camera2.params.DynamicRangeProfiles;
import android.hardware.camera2.params.ExtensionSessionConfiguration;
import android.hardware.camera2.params.OutputConfiguration;
import android.os.Handler;
import android.os.Looper;
import android.os.Parcel;
import android.util.Log;
import android.util.Size;
import java.lang.reflect.Modifier;
import java.util.ArrayList;
import java.util.List;
import java.util.TreeSet;

/**
 * The test of framework breadth batch F4e: camera2's session data as an app builds it on a phone without a camera:
 * output configurations, dynamic range profiles and an extension session's configuration, and the types that only
 * CameraCharacteristics makes, which must load. It runs on the main thread when the window first has the focus and
 * logs under the tag f4e-camera-session-test; tools/android-reference.sh takes the log from Android 15, and
 * tools/f4e-camera-session-check.sh compares Minato's with it.
 */
public class CameraSessionActivity extends Activity {
    static final String TAG = "f4e-camera-session-test";
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mStarted;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private interface Part {
        void run() throws Exception;
    }

    private static void part(String name, Part part) {
        try {
            part.run();
        } catch (Throwable t) {
            StringBuilder line = new StringBuilder(name);
            for (Throwable c = t; c != null; c = c.getCause()) {
                line.append(": ").append(c.getClass().getName());
            }
            log(line.toString());
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus || mStarted) {
            return;
        }
        mStarted = true;
        mHandler.post(() -> {
            part("dynamicRange", this::dynamicRange);
            part("output", this::output);
            part("extension", this::extension);
            part("loaded", this::loaded);
            log("done");
            finish();
        });
    }

    private void dynamicRange() {
        long[] elements = {DynamicRangeProfiles.HLG10, DynamicRangeProfiles.HLG10 | DynamicRangeProfiles.STANDARD, 0,
            DynamicRangeProfiles.HDR10, 0, DynamicRangeProfiles.HDR10};
        DynamicRangeProfiles profiles = new DynamicRangeProfiles(elements);
        log("dynamicRange: supported=" + new TreeSet<>(profiles.getSupportedProfiles())
                + " hlg10 constraints=" + new TreeSet<>(profiles.getProfileCaptureRequestConstraints(
                DynamicRangeProfiles.HLG10))
                + " hdr10 constraints=" + new TreeSet<>(profiles.getProfileCaptureRequestConstraints(
                DynamicRangeProfiles.HDR10))
                + " standard constraints=" + new TreeSet<>(profiles.getProfileCaptureRequestConstraints(
                DynamicRangeProfiles.STANDARD))
                + " hlg10 latency=" + profiles.isExtraLatencyPresent(DynamicRangeProfiles.HLG10)
                + " hdr10 latency=" + profiles.isExtraLatencyPresent(DynamicRangeProfiles.HDR10));
        log("dynamicRange constants: " + DynamicRangeProfiles.STANDARD + " " + DynamicRangeProfiles.HLG10 + " "
                + DynamicRangeProfiles.HDR10 + " " + DynamicRangeProfiles.HDR10_PLUS + " "
                + DynamicRangeProfiles.DOLBY_VISION_10B_HDR_REF + " "
                + DynamicRangeProfiles.DOLBY_VISION_8B_HDR_OEM_PO);
        try {
            new DynamicRangeProfiles(new long[] {DynamicRangeProfiles.STANDARD, 0, 0});
        } catch (IllegalArgumentException e) {
            log("dynamicRange standard entry: IllegalArgumentException");
        }
        try {
            new DynamicRangeProfiles(new long[] {1, 2});
        } catch (IllegalArgumentException e) {
            log("dynamicRange length: IllegalArgumentException");
        }
    }

    private void output() {
        OutputConfiguration config = new OutputConfiguration(new Size(640, 480), SurfaceTexture.class);
        log("output: group=" + config.getSurfaceGroupId() + " surface=" + config.getSurface() + " surfaces="
                + config.getSurfaces().size() + " maxShared=" + config.getMaxSharedSurfaceCount() + " range="
                + config.getDynamicRangeProfile() + " mirror=" + config.getMirrorMode() + " timestampBase="
                + config.getTimestampBase() + " useCase=" + config.getStreamUseCase() + " readout="
                + config.isReadoutTimestampEnabled());
        config.setDynamicRangeProfile(DynamicRangeProfiles.HLG10);
        config.setMirrorMode(OutputConfiguration.MIRROR_MODE_H);
        config.setTimestampBase(OutputConfiguration.TIMESTAMP_BASE_SENSOR);
        config.setStreamUseCase(1);
        config.setPhysicalCameraId("2");
        config.setReadoutTimestampEnabled(false);
        log("output set: range=" + config.getDynamicRangeProfile() + " mirror=" + config.getMirrorMode()
                + " timestampBase=" + config.getTimestampBase() + " useCase=" + config.getStreamUseCase()
                + " readout=" + config.isReadoutTimestampEnabled());
        OutputConfiguration same = new OutputConfiguration(new Size(640, 480), SurfaceTexture.class);
        log("output equals: " + config.equals(same) + " " + same.equals(new OutputConfiguration(
                new Size(640, 480), SurfaceTexture.class)));
        Parcel parcel = Parcel.obtain();
        config.writeToParcel(parcel, 0);
        parcel.setDataPosition(0);
        OutputConfiguration back = OutputConfiguration.CREATOR.createFromParcel(parcel);
        parcel.recycle();
        log("output parcel: range=" + back.getDynamicRangeProfile() + " mirror=" + back.getMirrorMode()
                + " equal=" + back.equals(config) + " describe=" + back.describeContents());
        log("output constants: " + OutputConfiguration.SURFACE_GROUP_ID_NONE + " "
                + OutputConfiguration.MIRROR_MODE_AUTO + " " + OutputConfiguration.MIRROR_MODE_NONE + " "
                + OutputConfiguration.MIRROR_MODE_V + " "
                + OutputConfiguration.TIMESTAMP_BASE_DEFAULT + " "
                + OutputConfiguration.TIMESTAMP_BASE_CHOREOGRAPHER_SYNCED);
    }

    private void extension() {
        List<OutputConfiguration> outputs = new ArrayList<>();
        outputs.add(new OutputConfiguration(new Size(1280, 720), SurfaceTexture.class));
        ExtensionSessionConfiguration config = new ExtensionSessionConfiguration(
                CameraExtensionCharacteristics.EXTENSION_NIGHT, outputs, Runnable::run,
                new CameraExtensionSession.StateCallback() {
                    @Override
                    public void onConfigured(CameraExtensionSession session) {
                    }

                    @Override
                    public void onConfigureFailed(CameraExtensionSession session) {
                    }

                    @Override
                    public void onClosed(CameraExtensionSession session) {
                    }
                });
        log("extension: extension=" + config.getExtension() + " outputs=" + config.getOutputConfigurations().size()
                + " postview=" + config.getPostviewOutputConfiguration() + " callback="
                + (config.getStateCallback() != null) + " executor=" + (config.getExecutor() != null));
        config.setPostviewOutputConfiguration(new OutputConfiguration(new Size(320, 240), SurfaceTexture.class));
        log("extension postview: " + (config.getPostviewOutputConfiguration() != null));
        log("extension constants: " + CameraExtensionCharacteristics.EXTENSION_AUTOMATIC + " "
                + CameraExtensionCharacteristics.EXTENSION_BOKEH + " " + CameraExtensionCharacteristics.EXTENSION_HDR
                + " " + CameraExtensionCharacteristics.EXTENSION_NIGHT);
    }

    /** The types only CameraCharacteristics and the camera service make: they load, final or not as AOSP's are. */
    private void loaded() throws Exception {
        String[] names = {"android.hardware.camera2.params.ColorSpaceProfiles",
            "android.hardware.camera2.params.LensIntrinsicsSample",
            "android.hardware.camera2.params.MandatoryStreamCombination",
            "android.hardware.camera2.params.MultiResolutionStreamConfigurationMap",
            "android.hardware.camera2.params.StreamConfigurationMap"};
        for (String name : names) {
            Class<?> c = Class.forName(name);
            log("loaded: " + c.getSimpleName() + " final=" + Modifier.isFinal(c.getModifiers()) + " public="
                    + Modifier.isPublic(c.getModifiers()));
        }
    }
}
