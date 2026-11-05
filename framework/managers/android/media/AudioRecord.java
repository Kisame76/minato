// The written part of AudioRecord (ADR-0064), which tools/managergen joins with the API's methods. The checks, states
// and messages are those of AOSP's AudioRecord.java at android-15.0.0_r1 (checked by mic-test against Android 15),
// without the audio server: the natives (android_media_AudioRecord.cpp, ours) read the microphone through an input
// stream of libaaudio, which needs RECORD_AUDIO's runtime grant (the host's iOS prompt): without it the recorder stays
// uninitialized, as Android's does. Not written, so logged once: routing, position and periodic notifications,
// microphone direction, active microphones, playback capture, preferred devices.
import android.annotation.NonNull;
import android.util.Log;

import java.nio.ByteBuffer;
import java.util.concurrent.atomic.AtomicInteger;

class AudioRecord {
    private static final String TAG = "android.media.AudioRecord";
    private static final AtomicInteger sNextSession = new AtomicInteger(1);

    private final Object mRecordingStateLock = new Object();
    private int mState = STATE_UNINITIALIZED;
    private int mRecordingState = RECORDSTATE_STOPPED;
    private int mSampleRate;
    private int mChannelCount = 1;
    private int mChannelMask = AudioFormat.CHANNEL_IN_MONO;
    private int mAudioFormat = AudioFormat.ENCODING_PCM_16BIT;
    private int mRecordSource = MediaRecorder.AudioSource.DEFAULT;
    private int mSessionId = AudioManager.AUDIO_SESSION_ID_GENERATE;
    private int mNativeBufferSizeInBytes;
    /** The native recorder, 0 once released. */
    private long mNative;

    public AudioRecord(int audioSource, int sampleRateInHz, int channelConfig, int audioFormat,
            int bufferSizeInBytes) throws IllegalArgumentException {
        this(new AudioAttributes.Builder().setInternalCapturePreset(audioSource).build(),
                new AudioFormat.Builder().setChannelMask(getChannelMaskFromLegacyConfig(channelConfig, false))
                        .setEncoding(audioFormat).setSampleRate(sampleRateInHz).build(),
                bufferSizeInBytes, AudioManager.AUDIO_SESSION_ID_GENERATE);
    }

    private AudioRecord(AudioAttributes attributes, AudioFormat format, int bufferSizeInBytes, int sessionId)
            throws IllegalArgumentException {
        if (format == null) {
            throw new IllegalArgumentException("Illegal null AudioFormat");
        }
        if (attributes == null) {
            throw new IllegalArgumentException("Illegal null AudioAttributes");
        }
        int rate = format.getSampleRate();
        if (rate == AudioFormat.SAMPLE_RATE_UNSPECIFIED) {
            rate = 0;
        }
        int encoding = AudioFormat.ENCODING_DEFAULT;
        if ((format.getPropertySetMask() & AudioFormat.AUDIO_FORMAT_HAS_PROPERTY_ENCODING) != 0) {
            encoding = format.getEncoding();
        }
        mChannelMask = AudioFormat.CHANNEL_IN_MONO;
        if ((format.getPropertySetMask() & AudioFormat.AUDIO_FORMAT_HAS_PROPERTY_CHANNEL_MASK) != 0) {
            mChannelMask = getChannelMaskFromLegacyConfig(format.getChannelMask(), false);
        }
        mChannelCount = Integer.bitCount(mChannelMask);
        audioParamCheck(attributes.getCapturePreset(), rate, encoding);
        audioBuffSizeCheck(bufferSizeInBytes);
        if (sessionId < 0) {
            throw new IllegalArgumentException("Invalid audio session ID: " + sessionId);
        }
        int frames = mNativeBufferSizeInBytes / (mChannelCount * AudioFormat.getBytesPerSample(mAudioFormat));
        if (mSampleRate == 0) {
            mSampleRate = DEFAULT_SAMPLE_RATE;
        }
        frames = Math.max(frames, minFrameCount(mSampleRate));
        mNative = nativeCreate(mSampleRate, mChannelCount, mAudioFormat, frames);
        if (mNative == 0) {
            Log.e(TAG, "Error code -20 when initializing native AudioRecord object.");
            return;
        }
        mSessionId = sessionId != AudioManager.AUDIO_SESSION_ID_GENERATE ? sessionId
                : (sNextSession.getAndIncrement() << 3) | 2;
        mState = STATE_INITIALIZED;
    }

    /** MediaRecorder.getAudioSourceMin and getAudioSourceMax, which the compile-only MediaRecorder cannot answer. */
    private static final int AUDIO_SOURCE_MIN = MediaRecorder.AudioSource.DEFAULT;
    private static final int AUDIO_SOURCE_MAX = MediaRecorder.AudioSource.VOICE_PERFORMANCE;

    /** The rate of a recorder that asked for none: the microphone's. */
    private static final int DEFAULT_SAMPLE_RATE = 48000;

    /** The fewest frames of a recorder at a rate: the microphone's 32 ms, in whole 16 frames, as the emulator's HAL. */
    private static int minFrameCount(int sampleRate) {
        return Math.max(16, (int) ((long) sampleRate * 32 / 1000 / 16 * 16));
    }

    private static int getChannelMaskFromLegacyConfig(int inChannelConfig, boolean allowLegacyConfig) {
        int mask;
        switch (inChannelConfig) {
            case AudioFormat.CHANNEL_IN_DEFAULT:
            case AudioFormat.CHANNEL_IN_MONO:
            case AudioFormat.CHANNEL_CONFIGURATION_MONO:
                mask = AudioFormat.CHANNEL_IN_MONO;
                break;
            case AudioFormat.CHANNEL_IN_STEREO:
            case AudioFormat.CHANNEL_CONFIGURATION_STEREO:
                mask = AudioFormat.CHANNEL_IN_STEREO;
                break;
            case (AudioFormat.CHANNEL_IN_FRONT | AudioFormat.CHANNEL_IN_BACK):
                mask = inChannelConfig;
                break;
            default:
                throw new IllegalArgumentException("Unsupported channel configuration.");
        }
        return mask;
    }

    private void audioParamCheck(int audioSource, int sampleRateInHz, int audioFormat)
            throws IllegalArgumentException {
        if ((audioSource < AUDIO_SOURCE_MIN || audioSource > AUDIO_SOURCE_MAX)
                && audioSource != MediaRecorder.AudioSource.RADIO_TUNER
                && audioSource != MediaRecorder.AudioSource.ECHO_REFERENCE
                && audioSource != MediaRecorder.AudioSource.HOTWORD) {
            throw new IllegalArgumentException("Invalid audio source " + audioSource);
        }
        mRecordSource = audioSource;
        if ((sampleRateInHz < AudioFormat.SAMPLE_RATE_HZ_MIN || sampleRateInHz > AudioFormat.SAMPLE_RATE_HZ_MAX)
                && sampleRateInHz != AudioFormat.SAMPLE_RATE_UNSPECIFIED) {
            throw new IllegalArgumentException(sampleRateInHz + "Hz is not a supported sample rate.");
        }
        mSampleRate = sampleRateInHz;
        switch (audioFormat) {
            case AudioFormat.ENCODING_DEFAULT:
                mAudioFormat = AudioFormat.ENCODING_PCM_16BIT;
                break;
            case AudioFormat.ENCODING_PCM_FLOAT:
            case AudioFormat.ENCODING_PCM_16BIT:
            case AudioFormat.ENCODING_PCM_8BIT:
                mAudioFormat = audioFormat;
                break;
            default:
                throw new IllegalArgumentException("Unsupported sample encoding " + audioFormat
                        + ". Should be ENCODING_PCM_8BIT, ENCODING_PCM_16BIT, or ENCODING_PCM_FLOAT.");
        }
    }

    private void audioBuffSizeCheck(int audioBufferSize) throws IllegalArgumentException {
        int frameSizeInBytes = mChannelCount * AudioFormat.getBytesPerSample(mAudioFormat);
        if ((audioBufferSize % frameSizeInBytes != 0) || (audioBufferSize < 1)) {
            throw new IllegalArgumentException("Invalid audio buffer size " + audioBufferSize + " (frame size "
                    + frameSizeInBytes + ")");
        }
        mNativeBufferSizeInBytes = audioBufferSize;
    }

    public void release() {
        try {
            stop();
        } catch (IllegalStateException ise) {
            // Released already, or never initialized.
        }
        long recorder;
        synchronized (mRecordingStateLock) {
            recorder = mNative;
            mNative = 0;
            mState = STATE_UNINITIALIZED;
            mRecordingState = RECORDSTATE_STOPPED;
        }
        if (recorder != 0) {
            nativeRelease(recorder);
        }
    }

    @Override
    protected void finalize() {
        long recorder = mNative;
        mNative = 0;
        if (recorder != 0) {
            nativeRelease(recorder);
        }
    }

    public int getSampleRate() {
        return mSampleRate;
    }

    public int getAudioSource() {
        return mRecordSource;
    }

    public int getAudioFormat() {
        return mAudioFormat;
    }

    public int getChannelConfiguration() {
        return mChannelMask;
    }

    public @NonNull AudioFormat getFormat() {
        AudioFormat.Builder builder = new AudioFormat.Builder().setSampleRate(mSampleRate).setEncoding(mAudioFormat);
        return builder.setChannelMask(mChannelMask).build();
    }

    public int getChannelCount() {
        return mChannelCount;
    }

    public int getState() {
        return mState;
    }

    public int getRecordingState() {
        synchronized (mRecordingStateLock) {
            return mRecordingState;
        }
    }

    public int getBufferSizeInFrames() {
        return mNative == 0 ? 0 : nativeBufferFrames(mNative);
    }

    public int getAudioSessionId() {
        return mSessionId;
    }

    public static int getMinBufferSize(int sampleRateInHz, int channelConfig, int audioFormat) {
        int channelCount;
        switch (channelConfig) {
            case AudioFormat.CHANNEL_IN_DEFAULT:
            case AudioFormat.CHANNEL_IN_MONO:
            case AudioFormat.CHANNEL_CONFIGURATION_MONO:
                channelCount = 1;
                break;
            case AudioFormat.CHANNEL_IN_STEREO:
            case AudioFormat.CHANNEL_CONFIGURATION_STEREO:
            case (AudioFormat.CHANNEL_IN_FRONT | AudioFormat.CHANNEL_IN_BACK):
                channelCount = 2;
                break;
            case AudioFormat.CHANNEL_INVALID:
            default:
                loge("getMinBufferSize(): Invalid channel configuration.");
                return ERROR_BAD_VALUE;
        }
        if (sampleRateInHz <= 0) {
            return ERROR_BAD_VALUE;
        }
        switch (audioFormat) {
            case AudioFormat.ENCODING_PCM_16BIT:
                break;
            case AudioFormat.ENCODING_PCM_8BIT:
            case AudioFormat.ENCODING_PCM_FLOAT:
                // The emulator's HAL answers its fallback buffer for these.
                return 640;
            default:
                loge("getMinBufferSize(): Invalid audio format.");
                return ERROR_BAD_VALUE;
        }
        if (sampleRateInHz < AudioFormat.SAMPLE_RATE_HZ_MIN || sampleRateInHz > AudioFormat.SAMPLE_RATE_HZ_MAX) {
            return 640;
        }
        return minFrameCount(sampleRateInHz) * channelCount * 2;
    }

    private static void loge(String message) {
        Log.e(TAG, message);
    }

    public void startRecording() throws IllegalStateException {
        if (mState != STATE_INITIALIZED) {
            throw new IllegalStateException("startRecording() called on an uninitialized AudioRecord.");
        }
        synchronized (mRecordingStateLock) {
            if (mNative != 0 && nativeStart(mNative) == SUCCESS) {
                mRecordingState = RECORDSTATE_RECORDING;
            }
        }
    }

    public void startRecording(MediaSyncEvent syncEvent) throws IllegalStateException {
        startRecording();
    }

    public void stop() throws IllegalStateException {
        if (mState != STATE_INITIALIZED) {
            throw new IllegalStateException("stop() called on an uninitialized AudioRecord.");
        }
        synchronized (mRecordingStateLock) {
            if (mNative != 0) {
                nativeStop(mNative);
            }
            mRecordingState = RECORDSTATE_STOPPED;
        }
    }

    private static boolean badMode(int readMode) {
        return readMode != READ_BLOCKING && readMode != READ_NON_BLOCKING;
    }

    public int read(@NonNull byte[] audioData, int offsetInBytes, int sizeInBytes) {
        return read(audioData, offsetInBytes, sizeInBytes, READ_BLOCKING);
    }

    public int read(@NonNull byte[] audioData, int offsetInBytes, int sizeInBytes, int readMode) {
        if (mState != STATE_INITIALIZED || mAudioFormat == AudioFormat.ENCODING_PCM_FLOAT) {
            return ERROR_INVALID_OPERATION;
        }
        if (badMode(readMode)) {
            loge("AudioRecord.read() called with invalid blocking mode");
            return ERROR_BAD_VALUE;
        }
        if (audioData == null || offsetInBytes < 0 || sizeInBytes < 0 || offsetInBytes + sizeInBytes < 0
                || offsetInBytes + sizeInBytes > audioData.length) {
            return ERROR_BAD_VALUE;
        }
        return nativeReadBytes(mNative, audioData, offsetInBytes, sizeInBytes, readMode == READ_BLOCKING);
    }

    public int read(@NonNull short[] audioData, int offsetInShorts, int sizeInShorts) {
        return read(audioData, offsetInShorts, sizeInShorts, READ_BLOCKING);
    }

    public int read(@NonNull short[] audioData, int offsetInShorts, int sizeInShorts, int readMode) {
        if (mState != STATE_INITIALIZED || mAudioFormat == AudioFormat.ENCODING_PCM_FLOAT) {
            return ERROR_INVALID_OPERATION;
        }
        if (badMode(readMode)) {
            loge("AudioRecord.read() called with invalid blocking mode");
            return ERROR_BAD_VALUE;
        }
        if (audioData == null || offsetInShorts < 0 || sizeInShorts < 0 || offsetInShorts + sizeInShorts < 0
                || offsetInShorts + sizeInShorts > audioData.length) {
            return ERROR_BAD_VALUE;
        }
        return nativeReadShorts(mNative, audioData, offsetInShorts, sizeInShorts, readMode == READ_BLOCKING);
    }

    public int read(@NonNull float[] audioData, int offsetInFloats, int sizeInFloats, int readMode) {
        if (mState == STATE_UNINITIALIZED) {
            Log.e(TAG, "AudioRecord.read() called in invalid state STATE_UNINITIALIZED");
            return ERROR_INVALID_OPERATION;
        }
        if (mAudioFormat != AudioFormat.ENCODING_PCM_FLOAT) {
            Log.w(TAG, "AudioRecord.read(float[] ...) requires format ENCODING_PCM_FLOAT");
            return ERROR_INVALID_OPERATION;
        }
        if (badMode(readMode)) {
            loge("AudioRecord.read() called with invalid blocking mode");
            return ERROR_BAD_VALUE;
        }
        if (audioData == null || offsetInFloats < 0 || sizeInFloats < 0 || offsetInFloats + sizeInFloats < 0
                || offsetInFloats + sizeInFloats > audioData.length) {
            return ERROR_BAD_VALUE;
        }
        return nativeReadFloats(mNative, audioData, offsetInFloats, sizeInFloats, readMode == READ_BLOCKING);
    }

    public int read(@NonNull ByteBuffer audioBuffer, int sizeInBytes) {
        return read(audioBuffer, sizeInBytes, READ_BLOCKING);
    }

    public int read(@NonNull ByteBuffer audioBuffer, int sizeInBytes, int readMode) {
        if (mState != STATE_INITIALIZED) {
            return ERROR_INVALID_OPERATION;
        }
        if (badMode(readMode)) {
            loge("AudioRecord.read() called with invalid blocking mode");
            return ERROR_BAD_VALUE;
        }
        if (audioBuffer == null || sizeInBytes < 0) {
            return ERROR_BAD_VALUE;
        }
        return nativeReadDirect(mNative, audioBuffer, sizeInBytes, readMode == READ_BLOCKING);
    }

    public static class Builder {
        private int mAudioSource = MediaRecorder.AudioSource.DEFAULT;
        private AudioFormat mFormat;
        private int mBufferSizeInBytes;
        private int mSessionId = AudioManager.AUDIO_SESSION_ID_GENERATE;

        public Builder() {
        }

        public Builder setAudioSource(int source) throws IllegalArgumentException {
            if ((source < AUDIO_SOURCE_MIN || source > AUDIO_SOURCE_MAX)
                    && source != MediaRecorder.AudioSource.RADIO_TUNER
                    && source != MediaRecorder.AudioSource.ECHO_REFERENCE
                    && source != MediaRecorder.AudioSource.HOTWORD) {
                throw new IllegalArgumentException("Invalid audio source " + source);
            }
            mAudioSource = source;
            return this;
        }

        public Builder setAudioFormat(@NonNull AudioFormat format) throws IllegalArgumentException {
            if (format == null) {
                throw new IllegalArgumentException("Illegal null AudioFormat argument");
            }
            mFormat = format;
            return this;
        }

        public Builder setBufferSizeInBytes(int bufferSizeInBytes) throws IllegalArgumentException {
            if (bufferSizeInBytes <= 0) {
                throw new IllegalArgumentException("Invalid buffer size " + bufferSizeInBytes);
            }
            mBufferSizeInBytes = bufferSizeInBytes;
            return this;
        }

        public Builder setContext(@NonNull android.content.Context context) {
            java.util.Objects.requireNonNull(context);
            return this;
        }

        public AudioRecord build() throws UnsupportedOperationException {
            if (mFormat == null) {
                mFormat = new AudioFormat.Builder().setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setChannelMask(AudioFormat.CHANNEL_IN_MONO).build();
            } else {
                if (mFormat.getEncoding() == AudioFormat.ENCODING_INVALID) {
                    mFormat = new AudioFormat.Builder(mFormat).setEncoding(AudioFormat.ENCODING_PCM_16BIT).build();
                }
                if (mFormat.getChannelMask() == AudioFormat.CHANNEL_INVALID
                        && mFormat.getChannelIndexMask() == AudioFormat.CHANNEL_INVALID) {
                    mFormat = new AudioFormat.Builder(mFormat).setChannelMask(AudioFormat.CHANNEL_IN_MONO).build();
                }
            }
            if (mBufferSizeInBytes == 0) {
                mBufferSizeInBytes = mFormat.getChannelCount() * mFormat.getBytesPerSample(mFormat.getEncoding());
            }
            try {
                final AudioRecord record = new AudioRecord(
                        new AudioAttributes.Builder().setInternalCapturePreset(mAudioSource).build(), mFormat,
                        mBufferSizeInBytes, mSessionId);
                if (record.getState() == STATE_UNINITIALIZED) {
                    throw new UnsupportedOperationException("Cannot create AudioRecord");
                }
                return record;
            } catch (IllegalArgumentException e) {
                throw new UnsupportedOperationException(e.getMessage());
            }
        }
    }

    private static native long nativeCreate(int sampleRate, int channelCount, int encoding, int frames);
    private static native void nativeRelease(long recorder);
    private static native int nativeStart(long recorder);
    private static native void nativeStop(long recorder);
    private static native int nativeBufferFrames(long recorder);
    private static native int nativeReadBytes(long recorder, byte[] data, int offset, int size, boolean blocking);
    private static native int nativeReadShorts(long recorder, short[] data, int offset, int size, boolean blocking);
    private static native int nativeReadFloats(long recorder, float[] data, int offset, int size, boolean blocking);
    private static native int nativeReadDirect(long recorder, ByteBuffer data, int size, boolean blocking);
}
