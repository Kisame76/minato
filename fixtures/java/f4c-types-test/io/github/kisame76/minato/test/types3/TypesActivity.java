package io.github.kisame76.minato.test.types3;

import android.app.Activity;
import android.graphics.drawable.Icon;
import android.hardware.camera2.params.ColorSpaceTransform;
import android.hardware.camera2.params.InputConfiguration;
import android.hardware.camera2.params.RggbChannelVector;
import android.hardware.camera2.params.TonemapCurve;
import android.media.MicrophoneDirection;
import android.media.SyncParams;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelUuid;
import android.os.Parcel;
import android.telecom.CallEndpoint;
import android.telecom.CallException;
import android.telecom.StatusHints;
import android.telecom.VideoProfile;
import android.util.Log;
import android.util.Rational;
import java.lang.reflect.Modifier;
import java.util.Arrays;
import java.util.UUID;

/**
 * The test of framework breadth batch F4c: the data types that SDKs test with instanceof or read from the lists the
 * managers return, as an app builds and reads them: camera2's parameter classes, telecom's call data and
 * android.media's sync parameters. It runs on the main thread when the window first has the focus and logs under the
 * tag f4c-types-test; tools/android-reference.sh takes the log from Android 15, and tools/f4c-types-check.sh compares
 * Minato's with it.
 */
public class TypesActivity extends Activity {
    static final String TAG = "f4c-types-test";
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
                line.append(": ").append(c.getClass().getName()).append(": ").append(c.getMessage());
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
            part("cameraParams", this::cameraParams);
            part("telecom", this::telecom);
            part("mediaTypes", this::mediaTypes);
            part("hiddenConstructors", this::declared);
            log("done");
            finish();
        });
    }

    private void cameraParams() {
        RggbChannelVector vector = new RggbChannelVector(1f, 0.5f, 0.25f, 2f);
        log("rggb: " + vector.getRed() + " " + vector.getGreenEven() + " " + vector.getGreenOdd() + " "
                + vector.getBlue() + " " + vector.equals(new RggbChannelVector(1f, 0.5f, 0.25f, 2f)));
        float[] curve = {0f, 0f, 0.5f, 0.25f, 1f, 1f};
        TonemapCurve tone = new TonemapCurve(curve, curve, curve);
        float[] copy = new float[curve.length];
        tone.copyColorCurve(TonemapCurve.CHANNEL_RED, copy, 0);
        log("tonemap: points=" + tone.getPointCount(TonemapCurve.CHANNEL_GREEN) + " copy="
                + Arrays.toString(copy) + " point=" + tone.getPoint(TonemapCurve.CHANNEL_BLUE, 1));
        Rational[] elements = new Rational[9];
        for (int i = 0; i < elements.length; i++) {
            elements[i] = new Rational(i, 4);
        }
        ColorSpaceTransform transform = new ColorSpaceTransform(elements);
        log("colorSpaceTransform: element=" + transform.getElement(2, 1) + " equal="
                + transform.equals(new ColorSpaceTransform(elements)));
        InputConfiguration input = new InputConfiguration(640, 480, 35);
        log("inputConfiguration: " + input.getWidth() + "x" + input.getHeight() + " format=" + input.getFormat()
                + " multiResolution=" + input.isMultiResolution());
    }

    private void telecom() {
        VideoProfile profile = new VideoProfile(VideoProfile.STATE_BIDIRECTIONAL, VideoProfile.QUALITY_HIGH);
        log("videoProfile: state=" + VideoProfile.videoStateToString(profile.getVideoState()) + " quality="
                + profile.getQuality() + " audioOnly=" + VideoProfile.isAudioOnly(profile.getVideoState())
                + " transmitting=" + VideoProfile.isTransmissionEnabled(profile.getVideoState()));
        Parcel parcel = Parcel.obtain();
        profile.writeToParcel(parcel, 0);
        parcel.setDataPosition(0);
        VideoProfile back = VideoProfile.CREATOR.createFromParcel(parcel);
        parcel.recycle();
        log("videoProfile parcel: state=" + back.getVideoState() + " quality=" + back.getQuality());
        Bundle extras = new Bundle();
        extras.putString("key", "value");
        StatusHints hints = new StatusHints("label", Icon.createWithData(new byte[] {1, 2, 3}, 0, 3), extras);
        log("statusHints: label=" + hints.getLabel() + " extras=" + hints.getExtras().getString("key"));
        CallEndpoint endpoint = new CallEndpoint("speaker", CallEndpoint.TYPE_SPEAKER,
                new ParcelUuid(new UUID(1, 2)));
        log("callEndpoint: name=" + endpoint.getEndpointName() + " type=" + endpoint.getEndpointType() + " id="
                + endpoint.getIdentifier());
        CallException exception = new CallException("no call", CallException.CODE_CALL_NOT_PERMITTED_AT_PRESENT_TIME);
        log("callException: " + exception.getMessage() + " code=" + exception.getCode());
    }

    private void mediaTypes() {
        SyncParams params = new SyncParams();
        params.setSyncSource(SyncParams.SYNC_SOURCE_AUDIO);
        params.setAudioAdjustMode(SyncParams.AUDIO_ADJUST_MODE_RESAMPLE);
        params.setTolerance(0.25f);
        params.setFrameRate(30f);
        log("syncParams: source=" + params.getSyncSource() + " audioAdjust=" + params.getAudioAdjustMode()
                + " tolerance=" + params.getTolerance() + " frameRate=" + params.getFrameRate()
                + " allowDefaults=" + params.allowDefaults().getSyncSource());
        log("microphoneDirection: " + MicrophoneDirection.MIC_DIRECTION_UNSPECIFIED + " "
                + MicrophoneDirection.MIC_DIRECTION_TOWARDS_USER + " "
                + MicrophoneDirection.MIC_DIRECTION_AWAY_FROM_USER
                + " " + MicrophoneDirection.MIC_DIRECTION_EXTERNAL);
    }

    /** The types whose constructors the system keeps: they load, and are final or not as AOSP's are. */
    private void declared() throws Exception {
        String[] names = {"android.hardware.camera2.params.BlackLevelPattern",
            "android.hardware.camera2.params.Capability", "android.hardware.camera2.params.Face",
            "android.hardware.camera2.params.LensShadingMap", "android.hardware.camera2.params.OisSample",
            "android.hardware.camera2.params.DeviceStateSensorOrientationMap",
            "android.hardware.camera2.params.RecommendedStreamConfigurationMap", "android.media.SubtitleData",
            "android.media.TimedMetaData", "android.media.TimedText", "android.telecom.GatewayInfo",
            "android.telecom.PhoneAccountSuggestion", "android.telecom.Conferenceable",
            "android.telecom.QueryLocationException", "android.telecom.CallEndpointException"};
        for (String name : names) {
            Class<?> type = Class.forName(name);
            log("declared " + type.getSimpleName() + ": final=" + Modifier.isFinal(type.getModifiers())
                    + " interface=" + type.isInterface() + " super=" + type.getSuperclass());
        }
    }
}
