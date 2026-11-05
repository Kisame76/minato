package io.github.kisame76.minato.test.visualizer;

import android.app.Application;
import android.content.pm.PackageManager;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.media.audiofx.Visualizer;
import android.util.Log;

/**
 * android.media.audiofx.Visualizer (the audio effects plan): without RECORD_AUDIO's grant what the constructor does;
 * with it the capture size range and rates, the defaults and what the setters take, the enabled state, the waveform,
 * the FFT and the peak and RMS measurement on a track that plays silence, the periodic capture's callbacks, a second
 * Visualizer, and the access after the release. The sound is not compared: the data's shapes and the return codes are.
 * What the device decides (the rates and ranges) goes to the tag visualizer-test-device. The lines carry the app's
 * label as their tag, so that the twin visualizer-test-granted logs under its own name.
 */
public class VisualizerApplication extends Application {
    static String TAG = "visualizer-test";

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private static void device(String line) {
        Log.i(TAG + "-device", line);
    }

    private interface Action {
        Object run() throws Exception;
    }

    private static String outcome(Action action) {
        try {
            return String.valueOf(action.run());
        } catch (Exception e) {
            return e.getClass().getName();
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        TAG = getApplicationInfo().loadLabel(getPackageManager()).toString();
        new Thread(() -> {
            boolean granted = checkSelfPermission("android.permission.RECORD_AUDIO")
                    == PackageManager.PERMISSION_GRANTED;
            log("RECORD_AUDIO granted " + granted);
            AudioTrack track = new AudioTrack(AudioManager.STREAM_MUSIC, 44100, AudioFormat.CHANNEL_OUT_STEREO,
                    AudioFormat.ENCODING_PCM_16BIT, 8192, AudioTrack.MODE_STREAM);
            track.play();
            track.write(new byte[8192], 0, 8192);
            int session = track.getAudioSessionId();
            if (!granted) {
                log("Visualizer without the grant: " + outcome(() -> {
                    Visualizer v = new Visualizer(session);
                    String s = "built, enabled " + v.getEnabled();
                    v.release();
                    return s;
                }));
            } else {
                try {
                    visualizer(session, track);
                } catch (Throwable t) {
                    log("failed: " + t.getClass().getName());
                }
            }
            track.release();
            log("done");
        }, "visualizer-test").start();
    }

    private void visualizer(int session, AudioTrack track) {
        int[] range = Visualizer.getCaptureSizeRange();
        log("capture size range: " + range.length + " values, min power of two " + (Integer.bitCount(range[0]) == 1)
                + ", max power of two " + (Integer.bitCount(range[1]) == 1));
        device("capture size range " + range[0] + ".." + range[1] + ", max capture rate "
                + Visualizer.getMaxCaptureRate());
        final Visualizer v;
        try {
            v = new Visualizer(session);
        } catch (Exception e) {
            log("Visualizer: " + e.getClass().getName());
            return;
        }
        log("built, enabled " + v.getEnabled() + ", capture size " + v.getCaptureSize());
        device("sampling rate " + v.getSamplingRate() + ", scaling mode " + v.getScalingMode() + ", measurement mode "
                + v.getMeasurementMode());
        log("setCaptureSize(256): " + outcome(() -> v.setCaptureSize(256)) + ", now " + v.getCaptureSize());
        log("setCaptureSize(3): " + outcome(() -> v.setCaptureSize(3)) + ", now " + v.getCaptureSize());
        log("setCaptureSize(range max + 1): " + outcome(() -> v.setCaptureSize(range[1] + 1)));
        log("setScalingMode(NORMALIZED): " + outcome(() -> v.setScalingMode(Visualizer.SCALING_MODE_NORMALIZED))
                + ", now " + v.getScalingMode());
        log("setScalingMode(AS_PLAYED): " + outcome(() -> v.setScalingMode(Visualizer.SCALING_MODE_AS_PLAYED))
                + ", now " + v.getScalingMode());
        log("setScalingMode(7): " + outcome(() -> v.setScalingMode(7)));
        log("setMeasurementMode(PEAK_RMS): " + outcome(() -> v.setMeasurementMode(Visualizer.MEASUREMENT_MODE_PEAK_RMS))
                + ", now " + v.getMeasurementMode());
        v.setCaptureSize(512);
        byte[] wave = new byte[512];
        byte[] fft = new byte[512];
        log("getWaveForm while disabled: " + outcome(() -> v.getWaveForm(wave)));
        log("getFft while disabled: " + outcome(() -> v.getFft(fft)));
        log("setEnabled(true): " + outcome(() -> v.setEnabled(true)) + ", enabled " + v.getEnabled());
        log("setCaptureSize while enabled: " + outcome(() -> v.setCaptureSize(256)));
        v.setEnabled(false);
        v.setCaptureSize(512);
        v.setEnabled(true);
        sleep(300);
        log("getWaveForm: " + outcome(() -> v.getWaveForm(wave)));
        boolean silent = true;
        for (byte b : wave) {
            silent &= (b & 0xff) == 128;
        }
        log("the waveform of silence is 128s: " + silent);
        log("getFft: " + outcome(() -> v.getFft(fft)));
        Visualizer.MeasurementPeakRms peakRms = new Visualizer.MeasurementPeakRms();
        log("getMeasurementPeakRms: " + outcome(() -> v.getMeasurementPeakRms(peakRms)) + ", peak " + peakRms.mPeak
                + ", rms " + peakRms.mRms);
        log("getWaveForm with a short array: " + outcome(() -> v.getWaveForm(new byte[16])));
        final int[] captures = {0, 0};
        final Visualizer.OnDataCaptureListener listener = new Visualizer.OnDataCaptureListener() {
            @Override
            public void onWaveFormDataCapture(Visualizer visualizer, byte[] waveform, int samplingRate) {
                captures[0]++;
            }

            @Override
            public void onFftDataCapture(Visualizer visualizer, byte[] fft, int samplingRate) {
                captures[1]++;
            }
        };
        log("setDataCaptureListener while enabled: "
                + outcome(() -> v.setDataCaptureListener(listener, Visualizer.getMaxCaptureRate() / 2, true, true)));
        v.setEnabled(false);
        // The listener is set from a thread with a looper of its own, so that the callbacks run there and not on the
        // main thread, as an app that captures on a background thread does.
        final android.os.HandlerThread callbackThread = new android.os.HandlerThread("visualizer-callbacks");
        callbackThread.start();
        final String[] disabledStatus = new String[1];
        final java.util.concurrent.CountDownLatch set = new java.util.concurrent.CountDownLatch(1);
        new android.os.Handler(callbackThread.getLooper()).post(() -> {
            disabledStatus[0] = outcome(() -> v.setDataCaptureListener(listener,
                    Visualizer.getMaxCaptureRate() / 2, true, true));
            set.countDown();
        });
        try {
            set.await();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        log("setDataCaptureListener while disabled: " + disabledStatus[0]);
        v.setEnabled(true);
        for (int wait = 0; wait < 60 && (captures[0] == 0 || captures[1] == 0); wait++) {
            sleep(100);
        }
        log("waveform callbacks came: " + (captures[0] > 0) + ", fft callbacks came: " + (captures[1] > 0));
        tone(v, track);
        log("setEnabled(false): " + outcome(() -> v.setEnabled(false)));
        callbackThread.quit();
        final Visualizer second = new Visualizer(session);
        log("a second Visualizer on the session: " + second.getEnabled());
        second.release();
        v.release();
        log("after release: " + outcome(() -> v.getEnabled()));
        log("on the output mix: " + outcome(() -> {
            Visualizer mix = new Visualizer(0);
            String s = "built, capture size " + mix.getCaptureSize();
            mix.release();
            return s;
        }));
    }

    /**
     * A 1 kHz tone of half scale on the track: the Visualizer's waveform moves round 128, its FFT peaks at the bin of
     * 1 kHz (n * 1000 / 48000), and the measured peak and RMS are above silence's -9600 mB. The emulator shows the tone
     * at a small level (the stream volume's attenuation is in it), so the lines say only that the tone is there and
     * where it peaks; the levels go to the tag visualizer-test-device.
     */
    private static void tone(Visualizer v, AudioTrack track) {
        final boolean[] playing = {true};
        Thread writer = new Thread(() -> {
            short[] block = new short[8820];  // 100 ms of stereo at 44.1 kHz
            double phase = 0;
            while (playing[0]) {
                for (int i = 0; i < block.length; i += 2) {
                    short sample = (short) (16384 * Math.sin(phase));
                    block[i] = sample;
                    block[i + 1] = sample;
                    phase += 2 * Math.PI * 1000 / 44100;
                }
                track.write(block, 0, block.length);
            }
        }, "visualizer-tone");
        writer.start();
        sleep(800);
        int size = v.getCaptureSize();
        byte[] wave = new byte[size];
        byte[] fft = new byte[size];
        log("tone getWaveForm: " + v.getWaveForm(wave) + ", getFft: " + v.getFft(fft));
        int high = 0;
        int low = 255;
        for (byte b : wave) {
            high = Math.max(high, b & 0xff);
            low = Math.min(low, b & 0xff);
        }
        device("tone waveform " + low + ".." + high);
        log("tone waveform moves round 128: " + (low < 128 && high > 128));
        int peakBin = 0;
        int peakMagnitude = 0;
        for (int k = 1; k < size / 2; k++) {
            int re = fft[2 * k];
            int im = fft[2 * k + 1];
            int magnitude = re * re + im * im;
            if (magnitude > peakMagnitude) {
                peakMagnitude = magnitude;
                peakBin = k;
            }
        }
        device("tone fft peak bin " + peakBin + " of " + size / 2 + ", magnitude " + Math.sqrt(peakMagnitude));
        int expected = size * 1000 / 48000;
        log("tone fft peaks at 1 kHz: " + (Math.abs(peakBin - expected) <= 1) + ", is there: " + (peakMagnitude > 0));
        Visualizer.MeasurementPeakRms peakRms = new Visualizer.MeasurementPeakRms();
        v.setEnabled(false);
        v.setMeasurementMode(Visualizer.MEASUREMENT_MODE_PEAK_RMS);
        v.setEnabled(true);
        sleep(500);
        log("tone measurement: " + v.getMeasurementPeakRms(peakRms));
        device("tone peak " + peakRms.mPeak + " mB, rms " + peakRms.mRms + " mB");
        log("tone peak above silence: " + (peakRms.mPeak > -9600) + ", rms above silence: " + (peakRms.mRms > -9600)
                + ", rms below peak: " + (peakRms.mRms < peakRms.mPeak));
        playing[0] = false;
    }

    private static void sleep(long millis) {
        try {
            Thread.sleep(millis);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }
}
