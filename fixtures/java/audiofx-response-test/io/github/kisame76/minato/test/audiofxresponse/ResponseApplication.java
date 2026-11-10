package io.github.kisame76.minato.test.audiofxresponse;

import android.app.Application;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.media.MediaPlayer;
import android.media.audiofx.BassBoost;
import android.media.audiofx.Equalizer;
import android.media.audiofx.LoudnessEnhancer;
import android.media.audiofx.Virtualizer;
import android.media.audiofx.Visualizer;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;

/**
 * The calibration of android.media.audiofx: a steady sine of a chosen frequency plays on a track, an effect of the
 * track's session is set, and the Visualizer, which sits after the effects, measures the peak in mB (the highest of
 * six reads over 300 ms). The effect's gain at that frequency is the peak with the effect less the peak without it, in
 * mB (100 mB = 1 dB). Absolute levels are not compared: the emulator's stream volume is in them. The gains go to the
 * tag audiofx-response-test-device, one line each; the compared lines only say that the effects act in the direction
 * they should. The runtime's own effects are calibrated against these gains.
 */
public class ResponseApplication extends Application {
    static String TAG = "audiofx-response-test";
    private static final int[] BAND_CENTERS = {60, 230, 910, 3600, 14000};

    private volatile double mHertz = 1000;
    private volatile boolean mLeftOnly;
    private volatile boolean mPlaying = true;
    private Visualizer mVisualizer;

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
        new Thread(this::run, "audiofx-response").start();
    }

    private void run() {
        final AudioTrack track = new AudioTrack(AudioManager.STREAM_MUSIC, 44100, AudioFormat.CHANNEL_OUT_STEREO,
                AudioFormat.ENCODING_PCM_16BIT, 16384, AudioTrack.MODE_STREAM);
        track.play();
        final Thread writer = new Thread(() -> {
            short[] block = new short[4410];  // 50 ms of stereo at 44.1 kHz
            double phase = 0;
            while (mPlaying) {
                for (int i = 0; i < block.length; i += 2) {
                    short sample = (short) (3277 * Math.sin(phase));  // 0.1 of full scale
                    block[i] = sample;
                    block[i + 1] = mLeftOnly ? 0 : sample;
                    phase += 2 * Math.PI * mHertz / 44100;
                }
                track.write(block, 0, block.length);
            }
        }, "audiofx-response-tone");
        writer.start();
        int session = track.getAudioSessionId();
        mVisualizer = new Visualizer(session);
        mVisualizer.setMeasurementMode(Visualizer.MEASUREMENT_MODE_PEAK_RMS);
        mVisualizer.setEnabled(true);
        equalizer(session);
        bassBoost(session);
        virtualizer(session);
        loudness(session);
        mPlaying = false;
        mediaPlayer();
        mVisualizer.release();
        track.release();
    }

    /** The Visualizer's peak in mB at the current frequency: the highest of six reads, 50 ms apart. */
    private int peak(double hertz) {
        mHertz = hertz;
        sleep(500);
        Visualizer.MeasurementPeakRms measurement = new Visualizer.MeasurementPeakRms();
        int highest = -9600;
        for (int i = 0; i < 6; i++) {
            mVisualizer.getMeasurementPeakRms(measurement);
            highest = Math.max(highest, measurement.mPeak);
            sleep(50);
        }
        return highest;
    }

    private void equalizer(int session) {
        Equalizer eq = new Equalizer(0, session);
        eq.setEnabled(true);
        short[] range = eq.getBandLevelRange();
        int improved = 0;
        int total = 0;
        for (int band = 0; band < BAND_CENTERS.length; band++) {
            double[] frequencies = {BAND_CENTERS[band] / 2.0, BAND_CENTERS[band], BAND_CENTERS[band] * 2.0};
            for (short level : new short[] {range[1], range[0]}) {
                for (short b = 0; b < BAND_CENTERS.length; b++) {
                    eq.setBandLevel(b, (short) 0);
                }
                for (double hertz : frequencies) {
                    if (hertz > 20000) {
                        continue;
                    }
                    eq.setBandLevel((short) band, (short) 0);
                    int base = peak(hertz);
                    eq.setBandLevel((short) band, level);
                    int with = peak(hertz);
                    device("eq band " + band + " (" + BAND_CENTERS[band] + " Hz) level " + level + " mB at "
                            + Math.round(hertz) + " Hz: gain " + (with - base) + " mB");
                    if (hertz == BAND_CENTERS[band]) {
                        total++;
                        if (level > 0 ? with - base > 300 : with - base < -300) {
                            improved++;
                        }
                    }
                }
            }
            eq.setBandLevel((short) band, (short) 0);
        }
        log("equalizer: boosts and cuts at the band centres act in their direction: " + (improved == total));
        eq.release();
    }

    private void bassBoost(int session) {
        BassBoost boost = new BassBoost(0, session);
        boost.setEnabled(false);
        double[] frequencies = {40, 80, 150, 300, 600, 1000, 3000};
        int[] base = new int[frequencies.length];
        for (int i = 0; i < frequencies.length; i++) {
            base[i] = peak(frequencies[i]);
        }
        for (short strength : new short[] {500, 1000}) {
            boost.setStrength(strength);
            boost.setEnabled(true);
            for (int i = 0; i < frequencies.length; i++) {
                device("bass boost strength " + strength + " at " + Math.round(frequencies[i]) + " Hz: gain "
                        + (peak(frequencies[i]) - base[i]) + " mB");
            }
            boost.setEnabled(false);
        }
        boost.setStrength((short) 1000);
        boost.setEnabled(true);
        log("bass boost: raises 80 Hz more than 3 kHz: " + (peak(80) - base[1] > peak(3000) - base[6] + 100));
        boost.release();
    }

    private void virtualizer(int session) {
        Virtualizer v = new Virtualizer(0, session);
        v.setEnabled(false);
        for (boolean left : new boolean[] {false, true}) {
            mLeftOnly = left;
            int base = peak(1000);
            v.setStrength((short) 1000);
            v.setEnabled(true);
            int with = peak(1000);
            device("virtualizer strength 1000, " + (left ? "left only" : "both channels") + " at 1000 Hz: gain "
                    + (with - base) + " mB");
            v.setEnabled(false);
        }
        mLeftOnly = false;
        v.release();
    }

    private void loudness(int session) {
        LoudnessEnhancer enhancer = new LoudnessEnhancer(session);
        enhancer.setEnabled(false);
        int base = peak(1000);
        for (int gain : new int[] {300, 600, 1500}) {
            enhancer.setTargetGain(gain);
            enhancer.setEnabled(true);
            int with = peak(1000);
            device("loudness enhancer target " + gain + " mB at 1000 Hz: gain " + (with - base) + " mB");
            enhancer.setEnabled(false);
        }
        enhancer.setTargetGain(600);
        enhancer.setEnabled(true);
        log("loudness enhancer: +600 mB raises the level: " + (peak(1000) - base > 300));
        enhancer.release();
    }

    /** A 1 kHz sine of three seconds as a WAV file: 16-bit mono at 44.1 kHz, 0.1 of full scale. */
    private File sineFile() throws IOException {
        final int frames = 44100 * 3;
        final byte[] wav = new byte[44 + 2 * frames];
        final int[] header = {0x52, 0x49, 0x46, 0x46, 0, 0, 0, 0, 0x57, 0x41, 0x56, 0x45, 0x66, 0x6d, 0x74, 0x20, 16, 0,
                0, 0, 1, 0, 1, 0, 0x44, 0xac, 0, 0, 0x88, 0x58, 0x01, 0, 2, 0, 16, 0, 0x64, 0x61, 0x74, 0x61};
        for (int i = 0; i < header.length; i++) {
            wav[i] = (byte) header[i];
        }
        final int riff = 36 + 2 * frames;
        for (int i = 0; i < 4; i++) {
            wav[4 + i] = (byte) (riff >> (8 * i));
            wav[40 + i] = (byte) ((2 * frames) >> (8 * i));
        }
        for (int i = 0; i < frames; i++) {
            final short sample = (short) (3277 * Math.sin(2 * Math.PI * 1000 * i / 44100));
            wav[44 + 2 * i] = (byte) sample;
            wav[45 + 2 * i] = (byte) (sample >> 8);
        }
        final File file = new File(getCacheDir(), "sine.wav");
        try (FileOutputStream out = new FileOutputStream(file)) {
            out.write(wav);
        }
        return file;
    }

    /** The same measurement on a MediaPlayer's session: a loudness enhancer raises the level of its sound. */
    private void mediaPlayer() {
        boolean raised = false;
        try {
            final MediaPlayer player = new MediaPlayer();
            player.setDataSource(sineFile().getPath());
            player.setLooping(true);
            player.prepare();
            final int session = player.getAudioSessionId();
            final Visualizer visualizer = new Visualizer(session);
            visualizer.setMeasurementMode(Visualizer.MEASUREMENT_MODE_PEAK_RMS);
            visualizer.setEnabled(true);
            final LoudnessEnhancer enhancer = new LoudnessEnhancer(session);
            enhancer.setEnabled(false);
            player.start();
            sleep(1500);
            final int base = measure(visualizer);
            enhancer.setTargetGain(600);
            enhancer.setEnabled(true);
            sleep(300);
            final int with = measure(visualizer);
            device("media player loudness enhancer target 600 mB at 1000 Hz: gain " + (with - base) + " mB");
            raised = with - base > 300;
            enhancer.setEnabled(false);
            player.setVolume(0.5f, 0.5f);  // Android applies the track's volume before the session's effects
            sleep(300);
            final int halved = measure(visualizer);
            device("media player volume 0.5 at 1000 Hz: gain " + (halved - base) + " mB");
            enhancer.release();
            visualizer.release();
            player.release();
        } catch (IOException | RuntimeException e) {
            device("media player: " + e);
        }
        log("media player: loudness enhancer +600 mB raises the level: " + raised);
        log("done");
    }

    private static int measure(Visualizer visualizer) {
        Visualizer.MeasurementPeakRms measurement = new Visualizer.MeasurementPeakRms();
        int highest = -9600;
        for (int i = 0; i < 6; i++) {
            visualizer.getMeasurementPeakRms(measurement);
            highest = Math.max(highest, measurement.mPeak);
            sleep(50);
        }
        return highest;
    }

    private static void sleep(long millis) {
        try {
            Thread.sleep(millis);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }
}
