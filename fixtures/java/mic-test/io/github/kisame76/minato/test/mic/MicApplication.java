package io.github.kisame76.minato.test.mic;

import android.app.Application;
import android.content.pm.PackageManager;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaRecorder;
import android.util.Log;

import java.nio.ByteBuffer;

/**
 * Microphone input (ADR-0064): the RECORD_AUDIO grant, AudioRecord's minimum sizes and refusals, and a recorder's
 * checks, states and reads of each kind. What the microphone hears is not compared: only how many frames come and how
 * the calls answer. What the device decides (the sizes) goes to the tag mic-test-device. A recorder is built only
 * where the permission is granted (the twin mic-test-granted); without it the lines say what the constructor does.
 */
public class MicApplication extends Application {
    /** The application's label, mic-test or its twin mic-test-granted: the tag the harness looks for. */
    static String TAG = "mic-test";

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private static void device(String line) {
        Log.i(TAG + "-device", line);
    }

    @Override
    public void onCreate() {
        super.onCreate();
        TAG = getApplicationInfo().loadLabel(getPackageManager()).toString();
        new Thread(() -> {
            boolean granted = checkSelfPermission("android.permission.RECORD_AUDIO")
                    == PackageManager.PERMISSION_GRANTED;
            log("RECORD_AUDIO granted " + granted);
            minimumSizes();
            if (granted) {
                recorder();
            } else {
                denied();
            }
            log("done");
        }, "mic-test").start();
    }

    private static void minimumSizes() {
        int[][] cases = {
            {16000, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, 2},
            {44100, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, 2},
            {48000, AudioFormat.CHANNEL_IN_STEREO, AudioFormat.ENCODING_PCM_16BIT, 4},
            {48000, AudioFormat.CHANNEL_IN_STEREO, AudioFormat.ENCODING_PCM_FLOAT, 8},
            {8000, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_8BIT, 1},
        };
        for (int[] c : cases) {
            int size = AudioRecord.getMinBufferSize(c[0], c[1], c[2]);
            log("min buffer " + c[0] + " " + c[1] + " " + c[2] + ": positive " + (size > 0) + ", whole frames "
                    + (size % c[3] == 0));
            device("min buffer " + c[0] + " " + c[1] + " " + c[2] + ": " + size);
        }
        log("min buffer, rate 1000: " + AudioRecord.getMinBufferSize(1000, AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT));
        log("min buffer, rate 0: " + AudioRecord.getMinBufferSize(0, AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT));
        log("min buffer, encoding 99: " + AudioRecord.getMinBufferSize(16000, AudioFormat.CHANNEL_IN_MONO, 99));
        log("min buffer, channels 0: " + AudioRecord.getMinBufferSize(16000, 0, AudioFormat.ENCODING_PCM_16BIT));
    }

    private interface Action {
        Object run() throws Exception;
    }

    private static String outcome(Action action) {
        try {
            return String.valueOf(action.run());
        } catch (Exception e) {
            return e.getClass().getName() + ": " + e.getMessage();
        }
    }

    /** Without the grant: what the constructor does, as Android 15 does it. */
    private static void denied() {
        log("ctor without grant: " + outcome(() -> {
            AudioRecord r = new AudioRecord(MediaRecorder.AudioSource.MIC, 16000, AudioFormat.CHANNEL_IN_MONO,
                    AudioFormat.ENCODING_PCM_16BIT, 4096);
            String s = "state " + r.getState();
            r.release();
            return s;
        }));
    }

    /** The peak and the frequency, from the zero crossings, to the nearest 10 Hz: 0 Hz for silence. */
    private static String describe(short[] samples, int rate) {
        int peak = 0;
        int crossings = 0;
        for (int i = 0; i < samples.length; i++) {
            peak = Math.max(peak, Math.abs(samples[i]));
            if (i > 0 && (samples[i - 1] < 0) != (samples[i] < 0)) {
                crossings++;
            }
        }
        double hertz = peak < 100 ? 0 : crossings / 2.0 / ((double) samples.length / rate);
        return "peak " + (peak < 100 ? "none" : peak > 1000 ? "loud" : "quiet") + ", about "
                + Math.round(hertz / 10) * 10 + " Hz";
    }

    private static void recorder() {
        int size = AudioRecord.getMinBufferSize(16000, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT);
        log("bad rate: " + outcome(() -> new AudioRecord(MediaRecorder.AudioSource.MIC, 1000,
                AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, 4096)));
        log("bad source: " + outcome(() -> new AudioRecord(99, 16000, AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT, 4096)));
        log("buffer below the minimum: " + outcome(() -> {
            AudioRecord small = new AudioRecord(MediaRecorder.AudioSource.MIC, 16000, AudioFormat.CHANNEL_IN_MONO,
                    AudioFormat.ENCODING_PCM_16BIT, 16);
            String state = "state " + small.getState();
            small.release();
            return state;
        }));
        AudioRecord r = new AudioRecord(MediaRecorder.AudioSource.MIC, 16000, AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT, size * 2);
        log("state " + r.getState() + ", recording state " + r.getRecordingState());
        log("rate " + r.getSampleRate() + ", channels " + r.getChannelCount() + ", format " + r.getAudioFormat()
                + ", source " + r.getAudioSource());
        log("buffer frames at least minimum " + (r.getBufferSizeInFrames() >= size / 2));
        log("read before start: " + r.read(new short[160], 0, 160));
        r.startRecording();
        log("recording state " + r.getRecordingState());
        short[] shorts = new short[1600];
        log("read shorts blocking: " + r.read(shorts, 0, shorts.length));
        device("what the microphone heard, 16 kHz: " + describe(shorts, 16000));
        byte[] bytes = new byte[3200];
        log("read bytes blocking: " + r.read(bytes, 0, bytes.length));
        float[] floats = new float[800];
        log("read floats (16-bit recorder): " + r.read(floats, 0, floats.length, AudioRecord.READ_BLOCKING));
        ByteBuffer direct = ByteBuffer.allocateDirect(1600);
        log("read direct buffer: " + r.read(direct, 1600));
        log("read negative size: " + r.read(shorts, 0, -1));
        log("read past the array: " + r.read(shorts, 1000, 1000));
        r.stop();
        log("recording state after stop " + r.getRecordingState());
        r.release();
        log("state after release " + r.getState());
        log("read after release: " + r.read(shorts, 0, 160));
        AudioRecord f = new AudioRecord(MediaRecorder.AudioSource.MIC, 48000, AudioFormat.CHANNEL_IN_STEREO,
                AudioFormat.ENCODING_PCM_FLOAT, AudioRecord.getMinBufferSize(48000, AudioFormat.CHANNEL_IN_STEREO,
                AudioFormat.ENCODING_PCM_FLOAT) * 2);
        log("float stereo: state " + f.getState() + ", channels " + f.getChannelCount() + ", format "
                + f.getAudioFormat());
        f.startRecording();
        float[] stereo = new float[960];
        log("read floats: " + f.read(stereo, 0, stereo.length, AudioRecord.READ_BLOCKING));
        f.release();
    }
}
