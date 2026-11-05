package io.github.kisame76.minato.test.audiofx;

import android.app.Application;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.media.audiofx.AudioEffect;
import android.media.audiofx.BassBoost;
import android.media.audiofx.EnvironmentalReverb;
import android.media.audiofx.Equalizer;
import android.media.audiofx.LoudnessEnhancer;
import android.media.audiofx.PresetReverb;
import android.media.audiofx.Virtualizer;
import android.util.Log;

/**
 * android.media.audiofx (ADR-0074 plan): the effects the device lists, and each of the five effects that players create
 * at their start, on a track's session: construction, control, the parameters with their ranges and defaults, a change
 * that is read back, the enabled state, a second instance on the same session, and the release. The sound is not
 * compared. What the device decides (the list of effects with their UUIDs and names, the band frequencies) goes to the
 * tag audiofx-test-device; the lines compared say how the calls answer.
 */
public class AudioFxApplication extends Application {
    static final String TAG = "audiofx-test";

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
        new Thread(() -> {
            AudioTrack track = new AudioTrack(AudioManager.STREAM_MUSIC, 44100, AudioFormat.CHANNEL_OUT_STEREO,
                    AudioFormat.ENCODING_PCM_16BIT, 8192, AudioTrack.MODE_STREAM);
            int session = track.getAudioSessionId();
            query();
            equalizer(session);
            bassBoost(session);
            virtualizer(session);
            loudness(session);
            reverbs(session);
            sessionZero();
            track.release();
            log("done");
        }, "audiofx-test").start();
    }

    private void query() {
        AudioEffect.Descriptor[] all = AudioEffect.queryEffects();
        log("queryEffects: " + (all != null) + ", " + (all != null && all.length > 0 ? "some" : "none"));
        if (all == null) {
            return;
        }
        for (AudioEffect.Descriptor d : all) {
            device("effect " + d.name + " | " + d.implementor + " | type " + d.type + " | uuid " + d.uuid + " | mode "
                    + d.connectMode);
        }
        String[] types = {"EQUALIZER", "BASS_BOOST", "VIRTUALIZER", "PRESET_REVERB", "ENV_REVERB", "AEC", "AGC", "NS"};
        java.util.UUID[] ids = {AudioEffect.EFFECT_TYPE_EQUALIZER, AudioEffect.EFFECT_TYPE_BASS_BOOST,
            AudioEffect.EFFECT_TYPE_VIRTUALIZER, AudioEffect.EFFECT_TYPE_PRESET_REVERB,
            AudioEffect.EFFECT_TYPE_ENV_REVERB, AudioEffect.EFFECT_TYPE_AEC, AudioEffect.EFFECT_TYPE_AGC,
            AudioEffect.EFFECT_TYPE_NS};
        for (int i = 0; i < ids.length; i++) {
            boolean listed = false;
            for (AudioEffect.Descriptor d : all) {
                listed |= d.type.equals(ids[i]);
            }
            log("type " + types[i] + " listed: " + listed);
        }
        log("AEC available: " + android.media.audiofx.AcousticEchoCanceler.isAvailable() + ", AGC "
                + android.media.audiofx.AutomaticGainControl.isAvailable() + ", NS "
                + android.media.audiofx.NoiseSuppressor.isAvailable());
    }

    private void equalizer(int session) {
        final Equalizer eq;
        try {
            eq = new Equalizer(0, session);
        } catch (Exception e) {
            log("Equalizer: " + e.getClass().getName());
            return;
        }
        log("Equalizer built, hasControl " + eq.hasControl() + ", enabled " + eq.getEnabled());
        short bands = eq.getNumberOfBands();
        short[] range = eq.getBandLevelRange();
        log("bands " + bands + ", level range " + range[0] + " " + range[1] + ", presets " + eq.getNumberOfPresets());
        for (short b = 0; b < bands; b++) {
            device("band " + b + ": " + eq.getCenterFreq(b) + " mHz, " + eq.getBandFreqRange(b)[0] + ".."
                    + eq.getBandFreqRange(b)[1] + ", level " + eq.getBandLevel(b));
        }
        for (short p = 0; p < eq.getNumberOfPresets(); p++) {
            device("preset " + p + ": " + eq.getPresetName(p));
        }
        log("current preset " + eq.getCurrentPreset());
        log("getBand(0 Hz) " + eq.getBand(0) + ", getBand(1000000 mHz) " + eq.getBand(1000000));
        eq.setBandLevel((short) 0, range[1]);
        log("band 0 after setting the maximum: " + (eq.getBandLevel((short) 0) == range[1]));
        log("current preset after a band change: " + eq.getCurrentPreset());
        eq.setBandLevel((short) 1, (short) -700);
        log("band 1 after -700: " + eq.getBandLevel((short) 1));
        log("band level beyond the range: " + outcome(() -> {
            eq.setBandLevel((short) 2, (short) 1600);
            return "no exception, level " + eq.getBandLevel((short) 2);
        }));
        log("setEnabled(true): " + eq.setEnabled(true) + ", enabled " + eq.getEnabled());
        Equalizer.Settings settings = eq.getProperties();
        log("properties: " + settings.numBands + " bands");
        final Equalizer eq2 = eq;
        log("band out of range: " + outcome(() -> {
            eq2.setBandLevel((short) 99, (short) 0);
            return "no exception";
        }));
        log("a preset out of range: " + outcome(() -> {
            eq2.usePreset((short) 99);
            return "no exception";
        }));
        eq.usePreset((short) 0);
        log("preset 0 used, current " + eq.getCurrentPreset() + ", band 0 " + eq.getBandLevel((short) 0) + ", band 1 "
                + eq.getBandLevel((short) 1) + ", band 4 " + eq.getBandLevel((short) 4));
        eq.usePreset((short) 2);
        log("preset 2 (Dance) bands " + eq.getBandLevel((short) 0) + " " + eq.getBandLevel((short) 1) + " "
                + eq.getBandLevel((short) 2) + " " + eq.getBandLevel((short) 3) + " " + eq.getBandLevel((short) 4));
        eq.usePreset((short) 9);
        log("preset 9 (Rock) bands " + eq.getBandLevel((short) 0) + " " + eq.getBandLevel((short) 1) + " "
                + eq.getBandLevel((short) 2) + " " + eq.getBandLevel((short) 3) + " " + eq.getBandLevel((short) 4));
        Equalizer second = new Equalizer(0, session);
        log("a second Equalizer on the session: hasControl " + second.hasControl() + ", the first " + eq.hasControl());
        second.release();
        eq.release();
        log("after release: " + outcome(() -> eq2.getEnabled()));
    }

    private void bassBoost(int session) {
        BassBoost bb = new BassBoost(0, session);
        log("BassBoost: strength supported " + bb.getStrengthSupported() + ", strength " + bb.getRoundedStrength());
        bb.setStrength((short) 500);
        log("strength after 500: " + bb.getRoundedStrength());
        bb.setStrength((short) 333);
        log("strength after 333: " + bb.getRoundedStrength());
        final BassBoost bb2 = bb;
        log("strength 2000: " + outcome(() -> {
            bb2.setStrength((short) 2000);
            return "no exception, strength " + bb2.getRoundedStrength();
        }));
        log("setEnabled(true): " + bb.setEnabled(true));
        bb.release();
    }

    private void virtualizer(int session) {
        Virtualizer v = new Virtualizer(0, session);
        log("Virtualizer: strength supported " + v.getStrengthSupported() + ", strength " + v.getRoundedStrength());
        v.setStrength((short) 500);
        log("strength after 500: " + v.getRoundedStrength());
        log("speaker angles: " + outcome(() -> v.getSpeakerAngles(android.media.AudioFormat.CHANNEL_OUT_STEREO,
                Virtualizer.VIRTUALIZATION_MODE_BINAURAL, new int[4])));
        log("virtualization mode: " + v.getVirtualizationMode());
        log("setEnabled(true): " + v.setEnabled(true));
        v.release();
    }

    private void loudness(int session) {
        LoudnessEnhancer le = new LoudnessEnhancer(session);
        log("LoudnessEnhancer: target gain " + le.getTargetGain());
        le.setTargetGain(500);
        log("target gain after 500: " + le.getTargetGain());
        log("setEnabled(true): " + le.setEnabled(true));
        le.release();
    }

    private void reverbs(int session) {
        try {
            PresetReverb pr = new PresetReverb(0, session);
            log("PresetReverb: preset " + pr.getPreset());
            pr.setPreset(PresetReverb.PRESET_LARGEHALL);
            log("preset after LARGEHALL: " + pr.getPreset());
            pr.release();
        } catch (Exception e) {
            log("PresetReverb: " + e.getClass().getName());
        }
        try {
            EnvironmentalReverb er = new EnvironmentalReverb(0, session);
            log("EnvironmentalReverb: room " + er.getRoomLevel() + ", hf " + er.getRoomHFLevel() + ", decay "
                    + er.getDecayTime() + ", ratio " + er.getDecayHFRatio() + ", reflections "
                    + er.getReflectionsLevel() + " / " + er.getReflectionsDelay() + ", reverb " + er.getReverbLevel()
                    + " / " + er.getReverbDelay() + ", diffusion " + er.getDiffusion() + ", density "
                    + er.getDensity());
            er.setDecayTime(3000);
            log("decay time after 3000: " + er.getDecayTime());
            er.release();
        } catch (Exception e) {
            log("EnvironmentalReverb: " + e.getClass().getName());
        }
    }

    /** Effects on the output mix (session 0), which players create for the whole device. */
    private void sessionZero() {
        log("Equalizer on session 0: " + outcome(() -> {
            Equalizer e = new Equalizer(0, 0);
            String s = "built, control " + e.hasControl();
            e.release();
            return s;
        }));
        log("Equalizer on a session nobody uses: " + outcome(() -> {
            Equalizer e = new Equalizer(0, 12345);
            String s = "built, control " + e.hasControl();
            e.release();
            return s;
        }));
    }
}
