package io.github.kisame76.minato.test.audiopatch;

import android.app.Activity;
import android.content.Context;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioRecordingConfiguration;
import android.media.AudioTrack;
import android.media.MediaRecorder;
import android.os.Handler;
import android.os.Looper;
import android.os.Parcel;
import android.util.Log;
import java.util.List;

/**
 * The test of framework breadth batch F4g (ADR-0064 step 1): what a recording app reads of the audio system's
 * recordings and ports. The audio port classes load; no recording is active and the recording callbacks come and go;
 * a recording configuration is made from a parcel in AOSP's layout, read back and sent through a parcel again. It runs
 * on the main thread when the window first has the focus and logs under the tag f4g-audio-patch-test;
 * tools/android-reference.sh takes the log from Android 15, and tools/f4g-audio-patch-check.sh compares Minato's.
 */
public class AudioPatchActivity extends Activity {
    static final String TAG = "f4g-audio-patch-test";
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
            part("classes", this::classes);
            part("recordings", this::recordings);
            part("callbacks", this::callbacks);
            part("configuration", this::configuration);
            log("done");
            finish();
        });
    }

    private void classes() throws Exception {
        String[] names = {"AudioPatch", "AudioPortConfig", "AudioPort", "AudioDevicePort", "AudioMixPort", "AudioGain",
                "AudioGainConfig", "AudioMixPortConfig", "AudioDevicePortConfig", "AudioHandle", "MicrophoneInfo",
                "AudioRecordingConfiguration", "AudioRecordingMonitor", "AudioRecordingMonitorClient"};
        StringBuilder line = new StringBuilder("classes:");
        for (String name : names) {
            Class<?> type = Class.forName("android.media." + name);
            line.append(' ').append(name).append(type.isInterface() ? "(interface)" : "");
        }
        log(line.toString());
        log("encapsulation: none=" + AudioTrack.ENCAPSULATION_MODE_NONE + " elementary="
                + AudioTrack.ENCAPSULATION_MODE_ELEMENTARY_STREAM);
    }

    private void recordings() {
        AudioManager manager = (AudioManager) getSystemService(Context.AUDIO_SERVICE);
        List<AudioRecordingConfiguration> active = manager.getActiveRecordingConfigurations();
        log("recordings: non-null=" + (active != null) + " active=" + active.size());
    }

    private void callbacks() {
        AudioManager manager = (AudioManager) getSystemService(Context.AUDIO_SERVICE);
        AudioManager.AudioRecordingCallback callback = new AudioManager.AudioRecordingCallback() { };
        manager.registerAudioRecordingCallback(callback, mHandler);
        manager.registerAudioRecordingCallback(callback, mHandler);   // a second time is logged and kept once
        manager.unregisterAudioRecordingCallback(callback);
        manager.unregisterAudioRecordingCallback(callback);           // already gone: logged
        log("callbacks: registered twice, unregistered twice");
        try {
            manager.registerAudioRecordingCallback(null, mHandler);
        } catch (IllegalArgumentException e) {
            log("callbacks: null " + e.getMessage());
        }
        try {
            manager.unregisterAudioRecordingCallback(null);
        } catch (IllegalArgumentException e) {
            log("callbacks: null " + e.getMessage());
        }
    }

    private static void writeConfiguration(Parcel parcel, AudioFormat client, AudioFormat device) {
        parcel.writeInt(7);                                  // session
        parcel.writeInt(MediaRecorder.AudioSource.MIC);      // source
        client.writeToParcel(parcel, 0);
        device.writeToParcel(parcel, 0);
        parcel.writeInt(12345);                              // patch handle, no patch has it
        parcel.writeString("com.example.recorder");
        parcel.writeInt(10123);                              // uid
        parcel.writeInt(42);                                 // port id
        parcel.writeBoolean(false);                          // silenced
        parcel.writeInt(MediaRecorder.AudioSource.MIC);      // device source
        parcel.writeInt(0);                                  // client effects
        parcel.writeInt(0);                                  // device effects
    }

    private void configuration() {
        AudioFormat client = new AudioFormat.Builder().setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setSampleRate(44100).setChannelMask(AudioFormat.CHANNEL_IN_MONO).build();
        AudioFormat device = new AudioFormat.Builder().setEncoding(AudioFormat.ENCODING_PCM_FLOAT)
                .setSampleRate(48000).setChannelMask(AudioFormat.CHANNEL_IN_STEREO).build();
        Parcel parcel = Parcel.obtain();
        writeConfiguration(parcel, client, device);
        parcel.setDataPosition(0);
        AudioRecordingConfiguration config = AudioRecordingConfiguration.CREATOR.createFromParcel(parcel);
        parcel.recycle();
        log("configuration: session=" + config.getClientAudioSessionId() + " source=" + config.getClientAudioSource()
                + " silenced=" + config.isClientSilenced() + " client=" + config.getClientFormat().getSampleRate() + "/"
                + config.getClientFormat().getEncoding() + "/" + config.getClientFormat().getChannelMask()
                + " device=" + config.getFormat().getSampleRate() + "/" + config.getFormat().getEncoding() + "/"
                + config.getFormat().getChannelMask() + " audioDevice=" + config.getAudioDevice());
        Parcel again = Parcel.obtain();
        config.writeToParcel(again, 0);
        again.setDataPosition(0);
        AudioRecordingConfiguration back = AudioRecordingConfiguration.CREATOR.createFromParcel(again);
        again.recycle();
        log("configuration parcel: equal=" + back.equals(config) + " samehash=" + (back.hashCode() == config.hashCode())
                + " describe=" + config.describeContents());
        log("configuration array: " + AudioRecordingConfiguration.CREATOR.newArray(3).length);
    }
}
