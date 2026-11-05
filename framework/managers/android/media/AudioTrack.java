/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// The written part of AudioTrack (the compatibility plan's gap 13, the Unity test APK probe: FMOD's output thread), which
// tools/managergen joins with the API's methods. The checks, states and messages are those of AOSP's AudioTrack.java
// at android-15.0.0_r1, without PlayerBase and the audio server: the natives (android_media_AudioTrack.cpp, ours) keep
// the track's buffer and play it on a libaaudio stream. Not written, so logged once: routing, codec and stream event
// callbacks, position notifications, loop points, playback rate and params, offload, timestamps, aux effects.
import android.annotation.NonNull;
import android.util.Log;

import java.nio.ByteBuffer;
import java.nio.NioUtils;
import java.util.concurrent.atomic.AtomicInteger;

class AudioTrack {
    private static final String TAG = "android.media.AudioTrack";
    /** AudioManager's PROPERTY_OUTPUT_SAMPLE_RATE, the rate of a track without one. */
    private static final int OUTPUT_SAMPLE_RATE = 48000;
    private static final float GAIN_MIN = 0.0f;
    private static final float GAIN_MAX = 1.0f;
    /** AudioSystem's AUDIO_UNIQUE_ID_USE_SESSION in a new ID's low bits, as the audio server makes session IDs. */
    private static final AtomicInteger sNextSession = new AtomicInteger(1);

    private final Object mPlayStateLock = new Object();
    private int mState = STATE_UNINITIALIZED;
    private int mPlayState = PLAYSTATE_STOPPED;
    private int mSampleRate;
    private int mChannelCount = 1;
    private int mChannelMask = AudioFormat.CHANNEL_OUT_MONO;
    private int mChannelIndexMask;
    private int mChannelConfiguration = AudioFormat.CHANNEL_OUT_MONO;
    private int mAudioFormat = AudioFormat.ENCODING_PCM_16BIT;
    private int mDataLoadMode = MODE_STREAM;
    private int mStreamType = AudioManager.STREAM_MUSIC;
    private int mSessionId = AudioManager.AUDIO_SESSION_ID_GENERATE;
    private int mPerformanceMode = PERFORMANCE_MODE_NONE;
    private AudioAttributes mAttributes;
    private int mNativeBufferSizeInBytes;
    private int mNativeBufferSizeInFrames;
    /** The native track, 0 once released. */
    private long mNative;

    public AudioTrack(int streamType, int sampleRateInHz, int channelConfig, int audioFormat, int bufferSizeInBytes,
            int mode) throws IllegalArgumentException {
        this(streamType, sampleRateInHz, channelConfig, audioFormat, bufferSizeInBytes, mode,
                AudioManager.AUDIO_SESSION_ID_GENERATE);
    }

    public AudioTrack(int streamType, int sampleRateInHz, int channelConfig, int audioFormat, int bufferSizeInBytes,
            int mode, int sessionId) throws IllegalArgumentException {
        this(new AudioAttributes.Builder().setLegacyStreamType(streamType).build(),
                new AudioFormat.Builder().setChannelMask(channelConfig).setEncoding(audioFormat)
                        .setSampleRate(sampleRateInHz).build(),
                bufferSizeInBytes, mode, sessionId);
    }

    public AudioTrack(AudioAttributes attributes, AudioFormat format, int bufferSizeInBytes, int mode, int sessionId)
            throws IllegalArgumentException {
        this(attributes, format, bufferSizeInBytes, mode, sessionId, PERFORMANCE_MODE_NONE);
    }

    private AudioTrack(AudioAttributes attributes, AudioFormat format, int bufferSizeInBytes, int mode,
            int sessionId, int performanceMode) throws IllegalArgumentException {
        // PlayerBase's check.
        if (attributes == null) {
            throw new IllegalArgumentException("Illegal null AudioAttributes");
        }
        mAttributes = attributes;
        if (format == null) {
            throw new IllegalArgumentException("Illegal null AudioFormat");
        }
        int rate = format.getSampleRate();
        if (rate == AudioFormat.SAMPLE_RATE_UNSPECIFIED) {
            rate = 0;
        }
        int channelIndexMask = 0;
        if ((format.getPropertySetMask() & AudioFormat.AUDIO_FORMAT_HAS_PROPERTY_CHANNEL_INDEX_MASK) != 0) {
            channelIndexMask = format.getChannelIndexMask();
        }
        int channelMask = 0;
        if ((format.getPropertySetMask() & AudioFormat.AUDIO_FORMAT_HAS_PROPERTY_CHANNEL_MASK) != 0) {
            channelMask = format.getChannelMask();
        } else if (channelIndexMask == 0) {
            channelMask = AudioFormat.CHANNEL_OUT_FRONT_LEFT | AudioFormat.CHANNEL_OUT_FRONT_RIGHT;
        }
        int encoding = AudioFormat.ENCODING_DEFAULT;
        if ((format.getPropertySetMask() & AudioFormat.AUDIO_FORMAT_HAS_PROPERTY_ENCODING) != 0) {
            encoding = format.getEncoding();
        }
        audioParamCheck(rate, channelMask, channelIndexMask, encoding, mode);
        // The native track's stream type, which Android's setup writes back: the attributes' legacy stream.
        mStreamType = AudioAttributes.toLegacyStreamType(attributes);
        audioBuffSizeCheck(bufferSizeInBytes);
        if (sessionId < 0) {
            throw new IllegalArgumentException("Invalid audio session ID: " + sessionId);
        }
        mPerformanceMode = performanceMode;
        if (mSampleRate == 0) {
            mSampleRate = OUTPUT_SAMPLE_RATE;
        }
        int frames = mNativeBufferSizeInFrames;
        if (mDataLoadMode == MODE_STREAM) {
            frames = Math.max(frames, minFrameCount(mSampleRate));
        }
        // The session comes first: the native stream plays through that session's audio effects.
        final int session = sessionId != AudioManager.AUDIO_SESSION_ID_GENERATE ? sessionId
                : (sNextSession.getAndIncrement() << 3) | 1;
        mNative = nativeCreate(mSampleRate, mChannelCount, mAudioFormat, frames, mDataLoadMode == MODE_STATIC,
                performanceMode == PERFORMANCE_MODE_LOW_LATENCY, session);
        if (mNative == 0) {
            Log.e(TAG, "Error code -20 when initializing AudioTrack.");
            return;
        }
        mSessionId = session;
        mState = mDataLoadMode == MODE_STATIC ? STATE_NO_STATIC_DATA : STATE_INITIALIZED;
    }

    /**
     * The fewest frames of a streaming track at a rate: AOSP's calculateMinFrameCount over the output's rate, mixer
     * period and latency (OUTPUT_SAMPLE_RATE, MIXER_FRAMES and MIXER_LATENCY_MS), as AudioTrack::getMinFrameCount asks
     * the audio server for them.
     */
    private static int minFrameCount(int sampleRate) {
        int buffers = Math.max(2, MIXER_LATENCY_MS / ((1000 * MIXER_FRAMES) / OUTPUT_SAMPLE_RATE));
        long needed = sampleRate == OUTPUT_SAMPLE_RATE ? MIXER_FRAMES
                : (long) MIXER_FRAMES * sampleRate / OUTPUT_SAMPLE_RATE + 1 + 1;
        return (int) (buffers * needed);
    }

    /** The output mixer's period in frames and its latency, which a minimum buffer covers. */
    private static final int MIXER_FRAMES = 960;
    private static final int MIXER_LATENCY_MS = 40;

    private void audioParamCheck(int sampleRateInHz, int channelConfig, int channelIndexMask, int audioFormat,
            int mode) {
        if ((sampleRateInHz < AudioFormat.SAMPLE_RATE_HZ_MIN || sampleRateInHz > AudioFormat.SAMPLE_RATE_HZ_MAX)
                && sampleRateInHz != AudioFormat.SAMPLE_RATE_UNSPECIFIED) {
            throw new IllegalArgumentException(sampleRateInHz + "Hz is not a supported sample rate.");
        }
        mSampleRate = sampleRateInHz;
        mChannelConfiguration = channelConfig;
        switch (channelConfig) {
            case AudioFormat.CHANNEL_OUT_DEFAULT:
            case AudioFormat.CHANNEL_OUT_MONO:
            case AudioFormat.CHANNEL_CONFIGURATION_MONO:
                mChannelCount = 1;
                mChannelMask = AudioFormat.CHANNEL_OUT_MONO;
                break;
            case AudioFormat.CHANNEL_OUT_STEREO:
            case AudioFormat.CHANNEL_CONFIGURATION_STEREO:
                mChannelCount = 2;
                mChannelMask = AudioFormat.CHANNEL_OUT_STEREO;
                break;
            default:
                if (channelConfig == AudioFormat.CHANNEL_INVALID && channelIndexMask != 0) {
                    mChannelCount = 0;
                    break;
                }
                if (!isMultichannelConfigSupported(channelConfig, audioFormat)) {
                    throw new IllegalArgumentException("Unsupported channel mask configuration " + channelConfig
                            + " for encoding " + audioFormat);
                }
                mChannelMask = channelConfig;
                mChannelCount = AudioFormat.channelCountFromOutChannelMask(channelConfig);
        }
        mChannelIndexMask = channelIndexMask;
        if (mChannelIndexMask != 0) {
            final int fullIndexMask = (1 << AudioSystem.FCC_24) - 1;
            final int channelIndexCount = Integer.bitCount(channelIndexMask);
            final boolean accepted = (channelIndexMask & ~fullIndexMask) == 0
                    && (!AudioFormat.isEncodingLinearFrames(audioFormat)
                            || channelIndexCount <= AudioSystem.OUT_CHANNEL_COUNT_MAX);
            if (!accepted) {
                throw new IllegalArgumentException("Unsupported channel index mask configuration " + channelIndexMask
                        + " for encoding " + audioFormat);
            }
            if (mChannelCount == 0) {
                mChannelCount = channelIndexCount;
            } else if (mChannelCount != channelIndexCount) {
                throw new IllegalArgumentException("Channel count must match");
            }
        }
        if (audioFormat == AudioFormat.ENCODING_DEFAULT) {
            audioFormat = AudioFormat.ENCODING_PCM_16BIT;
        }
        if (!AudioFormat.isPublicEncoding(audioFormat)) {
            throw new IllegalArgumentException("Unsupported audio encoding.");
        }
        mAudioFormat = audioFormat;
        if ((mode != MODE_STREAM && mode != MODE_STATIC)
                || (mode != MODE_STREAM && !AudioFormat.isEncodingLinearPcm(mAudioFormat))) {
            throw new IllegalArgumentException("Invalid mode.");
        }
        mDataLoadMode = mode;
    }

    /** The output's channels: AOSP's SUPPORTED_OUT_CHANNELS, the positions a mask may hold. */
    private static final int SUPPORTED_OUT_CHANNELS = AudioFormat.CHANNEL_OUT_FRONT_LEFT
            | AudioFormat.CHANNEL_OUT_FRONT_RIGHT | AudioFormat.CHANNEL_OUT_FRONT_CENTER
            | AudioFormat.CHANNEL_OUT_LOW_FREQUENCY | AudioFormat.CHANNEL_OUT_BACK_LEFT
            | AudioFormat.CHANNEL_OUT_BACK_RIGHT | AudioFormat.CHANNEL_OUT_FRONT_LEFT_OF_CENTER
            | AudioFormat.CHANNEL_OUT_FRONT_RIGHT_OF_CENTER | AudioFormat.CHANNEL_OUT_BACK_CENTER
            | AudioFormat.CHANNEL_OUT_SIDE_LEFT | AudioFormat.CHANNEL_OUT_SIDE_RIGHT
            | AudioFormat.CHANNEL_OUT_TOP_CENTER | AudioFormat.CHANNEL_OUT_TOP_FRONT_LEFT
            | AudioFormat.CHANNEL_OUT_TOP_FRONT_CENTER | AudioFormat.CHANNEL_OUT_TOP_FRONT_RIGHT
            | AudioFormat.CHANNEL_OUT_TOP_BACK_LEFT | AudioFormat.CHANNEL_OUT_TOP_BACK_CENTER
            | AudioFormat.CHANNEL_OUT_TOP_BACK_RIGHT | AudioFormat.CHANNEL_OUT_TOP_SIDE_LEFT
            | AudioFormat.CHANNEL_OUT_TOP_SIDE_RIGHT | AudioFormat.CHANNEL_OUT_BOTTOM_FRONT_LEFT
            | AudioFormat.CHANNEL_OUT_BOTTOM_FRONT_CENTER | AudioFormat.CHANNEL_OUT_BOTTOM_FRONT_RIGHT
            | AudioFormat.CHANNEL_OUT_LOW_FREQUENCY_2 | AudioFormat.CHANNEL_OUT_FRONT_WIDE_LEFT
            | AudioFormat.CHANNEL_OUT_FRONT_WIDE_RIGHT;

    /** The channel pairs whose sides go together, front first. */
    private static final int[] CHANNEL_PAIRS = {
        AudioFormat.CHANNEL_OUT_FRONT_LEFT | AudioFormat.CHANNEL_OUT_FRONT_RIGHT,
        AudioFormat.CHANNEL_OUT_BACK_LEFT | AudioFormat.CHANNEL_OUT_BACK_RIGHT,
        AudioFormat.CHANNEL_OUT_FRONT_LEFT_OF_CENTER | AudioFormat.CHANNEL_OUT_FRONT_RIGHT_OF_CENTER,
        AudioFormat.CHANNEL_OUT_SIDE_LEFT | AudioFormat.CHANNEL_OUT_SIDE_RIGHT,
        AudioFormat.CHANNEL_OUT_TOP_FRONT_LEFT | AudioFormat.CHANNEL_OUT_TOP_FRONT_RIGHT,
        AudioFormat.CHANNEL_OUT_TOP_BACK_LEFT | AudioFormat.CHANNEL_OUT_TOP_BACK_RIGHT,
        AudioFormat.CHANNEL_OUT_TOP_SIDE_LEFT | AudioFormat.CHANNEL_OUT_TOP_SIDE_RIGHT,
        AudioFormat.CHANNEL_OUT_BOTTOM_FRONT_LEFT | AudioFormat.CHANNEL_OUT_BOTTOM_FRONT_RIGHT,
        AudioFormat.CHANNEL_OUT_FRONT_WIDE_LEFT | AudioFormat.CHANNEL_OUT_FRONT_WIDE_RIGHT,
    };

    private static boolean isMultichannelConfigSupported(int channelConfig, int encoding) {
        if ((channelConfig & SUPPORTED_OUT_CHANNELS) != channelConfig) {
            Log.e(TAG, "Channel configuration features unsupported channels");
            return false;
        }
        final int channelCount = AudioFormat.channelCountFromOutChannelMask(channelConfig);
        final int channelCountLimit;
        try {
            channelCountLimit = AudioFormat.isEncodingLinearFrames(encoding) ? AudioSystem.OUT_CHANNEL_COUNT_MAX
                    : AudioSystem.FCC_24;
        } catch (IllegalArgumentException iae) {
            Log.e(TAG, "Unsupported encoding " + iae);
            return false;
        }
        if (channelCount > channelCountLimit) {
            Log.e(TAG, "Channel configuration contains too many channels for encoding " + encoding + "("
                    + channelCount + " > " + channelCountLimit + ")");
            return false;
        }
        for (int pair : CHANNEL_PAIRS) {
            if ((channelConfig & pair) != pair && (pair == CHANNEL_PAIRS[0] || (channelConfig & pair) != 0)) {
                Log.e(TAG, pair == CHANNEL_PAIRS[0] ? "Front channels must be present in multichannel configurations"
                        : "Channel pair cannot be used independently");
                return false;
            }
        }
        return true;
    }

    private void audioBuffSizeCheck(int audioBufferSize) {
        int frameSizeInBytes = AudioFormat.isEncodingLinearFrames(mAudioFormat)
                ? mChannelCount * AudioFormat.getBytesPerSample(mAudioFormat) : 1;
        if ((audioBufferSize % frameSizeInBytes != 0) || (audioBufferSize < 1)) {
            throw new IllegalArgumentException("Invalid audio buffer size.");
        }
        mNativeBufferSizeInBytes = audioBufferSize;
        mNativeBufferSizeInFrames = audioBufferSize / frameSizeInBytes;
    }

    public void release() {
        try {
            stop();
        } catch (IllegalStateException ise) {
            // Released already, or never initialized.
        }
        long track;
        synchronized (mPlayStateLock) {
            track = mNative;
            mNative = 0;
            mState = STATE_UNINITIALIZED;
            mPlayState = PLAYSTATE_STOPPED;
            mPlayStateLock.notify();
        }
        if (track != 0) {
            nativeRelease(track);
        }
    }

    @Override
    protected void finalize() {
        long track = mNative;
        mNative = 0;
        if (track != 0) {
            nativeRelease(track);
        }
    }

    public static float getMinVolume() {
        return GAIN_MIN;
    }

    public static float getMaxVolume() {
        return GAIN_MAX;
    }

    public int getSampleRate() {
        return mSampleRate;
    }

    public int getPlaybackRate() {
        return mSampleRate;
    }

    public int getAudioFormat() {
        return mAudioFormat;
    }

    public @NonNull AudioAttributes getAudioAttributes() {
        if (mState == STATE_UNINITIALIZED || mAttributes == null) {
            throw new IllegalStateException("track not initialized");
        }
        return mAttributes;
    }

    public int getStreamType() {
        return mStreamType;
    }

    public int getChannelConfiguration() {
        return mChannelConfiguration;
    }

    public @NonNull AudioFormat getFormat() {
        AudioFormat.Builder builder = new AudioFormat.Builder().setSampleRate(mSampleRate).setEncoding(mAudioFormat);
        if (mChannelConfiguration != AudioFormat.CHANNEL_INVALID) {
            builder.setChannelMask(mChannelConfiguration);
        }
        if (mChannelIndexMask != AudioFormat.CHANNEL_INVALID) {
            builder.setChannelIndexMask(mChannelIndexMask);
        }
        return builder.build();
    }

    public int getChannelCount() {
        return mChannelCount;
    }

    public int getState() {
        return mState;
    }

    public int getPlayState() {
        synchronized (mPlayStateLock) {
            return mPlayState;
        }
    }

    public int getBufferSizeInFrames() {
        return mNative == 0 ? mNativeBufferSizeInFrames : nativeBufferFrames(mNative);
    }

    public int getBufferCapacityInFrames() {
        return getBufferSizeInFrames();
    }

    public int getPerformanceMode() {
        return mPerformanceMode;
    }

    public int getPlaybackHeadPosition() {
        long track = mNative;
        if (track == 0) {
            throw new IllegalStateException("Unable to retrieve AudioTrack pointer for getPosition()");
        }
        return nativePosition(track);
    }

    public int getUnderrunCount() {
        long track = mNative;
        if (track == 0) {
            throw new IllegalStateException("Unable to retrieve AudioTrack pointer for getUnderrunCount()");
        }
        return nativeUnderruns(track);
    }

    public static int getNativeOutputSampleRate(int streamType) {
        return OUTPUT_SAMPLE_RATE;
    }

    public static int getMinBufferSize(int sampleRateInHz, int channelConfig, int audioFormat) {
        int channelCount;
        switch (channelConfig) {
            case AudioFormat.CHANNEL_OUT_MONO:
            case AudioFormat.CHANNEL_CONFIGURATION_MONO:
                channelCount = 1;
                break;
            case AudioFormat.CHANNEL_OUT_STEREO:
            case AudioFormat.CHANNEL_CONFIGURATION_STEREO:
                channelCount = 2;
                break;
            default:
                if (!isMultichannelConfigSupported(channelConfig, audioFormat)) {
                    Log.e(TAG, "getMinBufferSize(): Invalid channel configuration.");
                    return ERROR_BAD_VALUE;
                }
                channelCount = AudioFormat.channelCountFromOutChannelMask(channelConfig);
        }
        if (!AudioFormat.isPublicEncoding(audioFormat)) {
            Log.e(TAG, "getMinBufferSize(): Invalid audio format.");
            return ERROR_BAD_VALUE;
        }
        if (sampleRateInHz < AudioFormat.SAMPLE_RATE_HZ_MIN || sampleRateInHz > AudioFormat.SAMPLE_RATE_HZ_MAX) {
            Log.e(TAG, "getMinBufferSize(): " + sampleRateInHz + " Hz is not a supported sample rate.");
            return ERROR_BAD_VALUE;
        }
        int frames = minFrameCount(sampleRateInHz);
        return AudioFormat.isEncodingLinearFrames(audioFormat)
                ? frames * channelCount * AudioFormat.getBytesPerSample(audioFormat) : frames;
    }

    public int getAudioSessionId() {
        return mSessionId;
    }

    public int setStereoVolume(float leftGain, float rightGain) {
        long track = mNative;
        if (mState == STATE_UNINITIALIZED || track == 0) {
            return ERROR_INVALID_OPERATION;
        }
        nativeSetVolume(track, clampGain(leftGain), clampGain(rightGain));
        return SUCCESS;
    }

    private static float clampGain(float gain) {
        return gain < GAIN_MIN ? GAIN_MIN : gain > GAIN_MAX ? GAIN_MAX : gain;
    }

    public int setVolume(float gain) {
        return setStereoVolume(gain, gain);
    }

    public void play() throws IllegalStateException {
        if (mState != STATE_INITIALIZED) {
            throw new IllegalStateException("play() called on uninitialized AudioTrack.");
        }
        synchronized (mPlayStateLock) {
            nativeStart(mNative);
            mPlayState = PLAYSTATE_PLAYING;
        }
    }

    public void stop() throws IllegalStateException {
        if (mState != STATE_INITIALIZED) {
            throw new IllegalStateException("stop() called on uninitialized AudioTrack.");
        }
        synchronized (mPlayStateLock) {
            nativeStop(mNative);
            mPlayState = PLAYSTATE_STOPPED;
            mPlayStateLock.notify();
        }
    }

    public void pause() throws IllegalStateException {
        if (mState != STATE_INITIALIZED) {
            throw new IllegalStateException("pause() called on uninitialized AudioTrack.");
        }
        synchronized (mPlayStateLock) {
            nativePause(mNative);
            mPlayState = PLAYSTATE_PAUSED;
        }
    }

    public void flush() {
        if (mState == STATE_INITIALIZED) {
            nativeFlush(mNative);
        }
    }

    private int afterWrite(int written) {
        if (mDataLoadMode == MODE_STATIC && mState == STATE_NO_STATIC_DATA && written > 0) {
            mState = STATE_INITIALIZED;
        }
        return written;
    }

    private static boolean badMode(int writeMode) {
        if (writeMode != WRITE_BLOCKING && writeMode != WRITE_NON_BLOCKING) {
            Log.e(TAG, "AudioTrack.write() called with invalid blocking mode");
            return true;
        }
        return false;
    }

    public int write(@NonNull byte[] audioData, int offsetInBytes, int sizeInBytes) {
        return write(audioData, offsetInBytes, sizeInBytes, WRITE_BLOCKING);
    }

    public int write(@NonNull byte[] audioData, int offsetInBytes, int sizeInBytes, int writeMode) {
        long track = mNative;
        if (mState == STATE_UNINITIALIZED || track == 0 || mAudioFormat == AudioFormat.ENCODING_PCM_FLOAT) {
            return ERROR_INVALID_OPERATION;
        }
        if (badMode(writeMode)) {
            return ERROR_BAD_VALUE;
        }
        if (audioData == null || offsetInBytes < 0 || sizeInBytes < 0 || offsetInBytes + sizeInBytes < 0
                || offsetInBytes + sizeInBytes > audioData.length) {
            return ERROR_BAD_VALUE;
        }
        return afterWrite(nativeWriteBytes(track, audioData, offsetInBytes, sizeInBytes,
                writeMode == WRITE_BLOCKING));
    }

    public int write(@NonNull short[] audioData, int offsetInShorts, int sizeInShorts) {
        return write(audioData, offsetInShorts, sizeInShorts, WRITE_BLOCKING);
    }

    public int write(@NonNull short[] audioData, int offsetInShorts, int sizeInShorts, int writeMode) {
        long track = mNative;
        if (mState == STATE_UNINITIALIZED || track == 0 || mAudioFormat == AudioFormat.ENCODING_PCM_FLOAT
                || mAudioFormat > AudioFormat.ENCODING_LEGACY_SHORT_ARRAY_THRESHOLD) {
            return ERROR_INVALID_OPERATION;
        }
        if (badMode(writeMode)) {
            return ERROR_BAD_VALUE;
        }
        if (audioData == null || offsetInShorts < 0 || sizeInShorts < 0 || offsetInShorts + sizeInShorts < 0
                || offsetInShorts + sizeInShorts > audioData.length) {
            return ERROR_BAD_VALUE;
        }
        return afterWrite(nativeWriteShorts(track, audioData, offsetInShorts, sizeInShorts,
                writeMode == WRITE_BLOCKING));
    }

    public int write(@NonNull float[] audioData, int offsetInFloats, int sizeInFloats, int writeMode) {
        long track = mNative;
        if (mState == STATE_UNINITIALIZED || track == 0) {
            Log.e(TAG, "AudioTrack.write() called in invalid state STATE_UNINITIALIZED");
            return ERROR_INVALID_OPERATION;
        }
        if (mAudioFormat != AudioFormat.ENCODING_PCM_FLOAT) {
            Log.e(TAG, "AudioTrack.write(float[] ...) requires format ENCODING_PCM_FLOAT");
            return ERROR_INVALID_OPERATION;
        }
        if (badMode(writeMode)) {
            return ERROR_BAD_VALUE;
        }
        if (audioData == null || offsetInFloats < 0 || sizeInFloats < 0 || offsetInFloats + sizeInFloats < 0
                || offsetInFloats + sizeInFloats > audioData.length) {
            Log.e(TAG, "AudioTrack.write() called with invalid array, offset, or size");
            return ERROR_BAD_VALUE;
        }
        return afterWrite(nativeWriteFloats(track, audioData, offsetInFloats, sizeInFloats,
                writeMode == WRITE_BLOCKING));
    }

    public int write(@NonNull ByteBuffer audioData, int sizeInBytes, int writeMode) {
        long track = mNative;
        if (mState == STATE_UNINITIALIZED || track == 0) {
            Log.e(TAG, "AudioTrack.write() called in invalid state STATE_UNINITIALIZED");
            return ERROR_INVALID_OPERATION;
        }
        if (badMode(writeMode)) {
            return ERROR_BAD_VALUE;
        }
        if (audioData == null || sizeInBytes < 0 || sizeInBytes > audioData.remaining()) {
            Log.e(TAG, "AudioTrack.write() called with invalid size (" + sizeInBytes + ") value");
            return ERROR_BAD_VALUE;
        }
        boolean blocking = writeMode == WRITE_BLOCKING;
        int written = audioData.isDirect()
                ? nativeWriteDirect(track, audioData, audioData.position(), sizeInBytes, blocking)
                : nativeWriteBytes(track, NioUtils.unsafeArray(audioData),
                        NioUtils.unsafeArrayOffset(audioData) + audioData.position(), sizeInBytes, blocking);
        afterWrite(written);
        if (written > 0) {
            audioData.position(audioData.position() + written);
        }
        return written;
    }

    public int write(@NonNull ByteBuffer audioData, int sizeInBytes, int writeMode, long timestamp) {
        if (mState == STATE_UNINITIALIZED) {
            Log.e(TAG, "AudioTrack.write() called in invalid state STATE_UNINITIALIZED");
            return ERROR_INVALID_OPERATION;
        }
        if (badMode(writeMode)) {
            return ERROR_BAD_VALUE;
        }
        if (mDataLoadMode != MODE_STREAM) {
            Log.e(TAG, "AudioTrack.write() with timestamp called for non-streaming mode track");
            return ERROR_INVALID_OPERATION;
        }
        // No track here has FLAG_HW_AV_SYNC.
        Log.d(TAG, "AudioTrack.write() called on a regular AudioTrack. Ignoring pts...");
        return write(audioData, sizeInBytes, writeMode);
    }

    public int reloadStaticData() {
        if (mDataLoadMode == MODE_STREAM || mState != STATE_INITIALIZED) {
            return ERROR_INVALID_OPERATION;
        }
        return nativeReloadStatic(mNative);
    }

    public static class Builder {
        private AudioAttributes mAttributes;
        private AudioFormat mFormat;
        private int mBufferSizeInBytes;
        private int mSessionId = AudioManager.AUDIO_SESSION_ID_GENERATE;
        private int mMode = MODE_STREAM;
        private int mPerformanceMode = PERFORMANCE_MODE_NONE;

        public Builder() {
        }

        public @NonNull Builder setContext(@NonNull android.content.Context context) {
            java.util.Objects.requireNonNull(context);
            return this;
        }

        public @NonNull Builder setAudioAttributes(@NonNull AudioAttributes attributes)
                throws IllegalArgumentException {
            if (attributes == null) {
                throw new IllegalArgumentException("Illegal null AudioAttributes argument");
            }
            mAttributes = attributes;
            return this;
        }

        public @NonNull Builder setAudioFormat(@NonNull AudioFormat format) throws IllegalArgumentException {
            if (format == null) {
                throw new IllegalArgumentException("Illegal null AudioFormat argument");
            }
            mFormat = format;
            return this;
        }

        public @NonNull Builder setBufferSizeInBytes(int bufferSizeInBytes) throws IllegalArgumentException {
            if (bufferSizeInBytes <= 0) {
                throw new IllegalArgumentException("Invalid buffer size " + bufferSizeInBytes);
            }
            mBufferSizeInBytes = bufferSizeInBytes;
            return this;
        }

        public @NonNull Builder setTransferMode(int mode) throws IllegalArgumentException {
            switch (mode) {
                case MODE_STREAM:
                case MODE_STATIC:
                    mMode = mode;
                    break;
                default:
                    throw new IllegalArgumentException("Invalid transfer mode " + mode);
            }
            return this;
        }

        public @NonNull Builder setSessionId(int sessionId) throws IllegalArgumentException {
            if (sessionId != AudioManager.AUDIO_SESSION_ID_GENERATE && sessionId < 1) {
                throw new IllegalArgumentException("Invalid audio session ID " + sessionId);
            }
            mSessionId = sessionId;
            return this;
        }

        public @NonNull Builder setPerformanceMode(int performanceMode) {
            switch (performanceMode) {
                case PERFORMANCE_MODE_NONE:
                case PERFORMANCE_MODE_LOW_LATENCY:
                case PERFORMANCE_MODE_POWER_SAVING:
                    mPerformanceMode = performanceMode;
                    break;
                default:
                    throw new IllegalArgumentException("Invalid performance mode " + performanceMode);
            }
            return this;
        }

        public @NonNull AudioTrack build() throws UnsupportedOperationException {
            if (mAttributes == null) {
                mAttributes = new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).build();
            }
            if (mFormat == null) {
                mFormat = new AudioFormat.Builder().setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                        .setEncoding(AudioFormat.ENCODING_DEFAULT).build();
            }
            if (mMode == MODE_STREAM && mBufferSizeInBytes == 0) {
                int bytesPerSample = 1;
                if (AudioFormat.isEncodingLinearFrames(mFormat.getEncoding())) {
                    try {
                        bytesPerSample = AudioFormat.getBytesPerSample(mFormat.getEncoding());
                    } catch (IllegalArgumentException e) {
                        // Stays 1, as AOSP's.
                    }
                }
                mBufferSizeInBytes = mFormat.getChannelCount() * bytesPerSample;
            }
            try {
                final AudioTrack track = new AudioTrack(mAttributes, mFormat, mBufferSizeInBytes, mMode, mSessionId,
                        mPerformanceMode);
                if (track.getState() == STATE_UNINITIALIZED) {
                    throw new UnsupportedOperationException("Cannot create AudioTrack");
                }
                return track;
            } catch (IllegalArgumentException e) {
                throw new UnsupportedOperationException(e.getMessage());
            }
        }
    }

    private static native long nativeCreate(int sampleRate, int channelCount, int encoding, int frames,
            boolean isStatic, boolean lowLatency, int session);
    private static native void nativeRelease(long track);
    private static native void nativeStart(long track);
    private static native void nativeStop(long track);
    private static native void nativePause(long track);
    private static native void nativeFlush(long track);
    private static native int nativeReloadStatic(long track);
    private static native int nativeWriteBytes(long track, byte[] data, int offset, int size, boolean blocking);
    private static native int nativeWriteShorts(long track, short[] data, int offset, int size, boolean blocking);
    private static native int nativeWriteFloats(long track, float[] data, int offset, int size, boolean blocking);
    private static native int nativeWriteDirect(long track, ByteBuffer data, int offset, int size, boolean blocking);
    private static native int nativePosition(long track);
    private static native int nativeUnderruns(long track);
    private static native int nativeBufferFrames(long track);
    private static native void nativeSetVolume(long track, float left, float right);

    /** @hide Encapsulated audio is not offered, but AudioDevicePort and the encoding checks name its types. */
    @java.lang.annotation.Retention(java.lang.annotation.RetentionPolicy.SOURCE)
    public @interface EncapsulationMode {}

    /** @hide */
    @java.lang.annotation.Retention(java.lang.annotation.RetentionPolicy.SOURCE)
    public @interface EncapsulationMetadataType {}

    /** @hide The mode of a handle from the Tuner API (a system API in AOSP). */
    public static final int ENCAPSULATION_MODE_HANDLE = 2;
}
