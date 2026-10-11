package android.os;

import com.android.minato.services.Unwritten;

/**
 * Placeholder (App, the gap list of 2026-10-11): our class under AOSP's hidden name, which only has to load, since
 * VibrationEffect and its segments name it and apps reflect over VibrationEffect; every body logs once and throws
 * UnsupportedOperationException. Before, a compile-only stub (ADR-0045 gap 1, batch 2): a vibrator's capabilities,
 * against which a vibration effect checks itself before the vibrator service plays it; Minato's vibrator comes with
 * the vibrator's step.
 */
public class VibratorInfo {
    private VibratorInfo() {
        throw Unwritten.placeholder("android.os.VibratorInfo()");
    }

    public boolean hasAmplitudeControl() {
        throw Unwritten.placeholder("android.os.VibratorInfo.hasAmplitudeControl()");
    }

    public boolean hasFrequencyControl() {
        throw Unwritten.placeholder("android.os.VibratorInfo.hasFrequencyControl()");
    }

    public int isEffectSupported(int effectId) {
        throw Unwritten.placeholder("android.os.VibratorInfo.isEffectSupported(int)");
    }

    public boolean isPrimitiveSupported(int primitiveId) {
        throw Unwritten.placeholder("android.os.VibratorInfo.isPrimitiveSupported(int)");
    }
}
