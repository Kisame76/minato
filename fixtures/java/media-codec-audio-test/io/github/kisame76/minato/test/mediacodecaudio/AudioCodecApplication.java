package io.github.kisame76.minato.test.mediacodecaudio;

import android.app.Application;
import android.content.Context;
import android.content.res.AssetFileDescriptor;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.util.Range;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.ShortBuffer;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.TreeSet;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.zip.CRC32;

/**
 * ADR-0060 e2f's test of MediaCodec's AAC decoder, on the AAC track of Minato's own baseline.mp4 (a sine, mono,
 * 44100 Hz, through MediaExtractor). Decoders of AAC differ in the last bits of their samples, so the samples are
 * compared coarsely: their count, the RMS in steps of 1 % of full scale and the rising zero crossings; a CRC32 of them
 * goes to the device tag. From a thread that Application.attachBaseContext starts:
 * - MediaCodecList: the decoders of audio/mp4a-latm by name, alias and flags, and the first one's audio capabilities;
 * - the track's format through MediaExtractor;
 * - the track decoded synchronously by createDecoderByType: the input buffer's capacity, each output format change
 *   with its keys, the output buffers' count, the first and last times, the end of the stream, and the samples;
 * - again after a seek to 0.5 s and a flush;
 * - asynchronously (setCallback on a HandlerThread);
 * - by createByCodecName with the first decoder's name, and the refusals: an unknown name and an unknown type.
 * Each wait is bounded; it logs with the tag media-codec-audio-test and ends with "done".
 */
public class AudioCodecApplication extends Application {
    private static final String TAG = "media-codec-audio-test";
    private static final String AAC = MediaFormat.MIMETYPE_AUDIO_AAC;
    private static final long WAIT_US = 10_000;

    private final List<String> lines = new ArrayList<>();

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        new Thread(() -> {
            try {
                test(base);
            } catch (Throwable t) {
                add("failed: " + t);
                add("device " + Log.getStackTraceString(t));
            }
            add("done");
            flush();
        }, TAG).start();
    }

    private void add(String line) {
        lines.add(line);
    }

    /** The lines, those that start with "device " to the device tag, a pause after every 10 (liblog drops lines). */
    private void flush() {
        int count = 0;
        for (String line : lines) {
            for (String part : line.split("\n")) {
                if (line.startsWith("device ")) {
                    Log.i(TAG + "-device", part.startsWith("device ") ? part.substring(7) : part);
                } else {
                    Log.i(TAG, part);
                }
                if (++count % 10 == 0) pause(5);
            }
        }
        lines.clear();
    }

    private static void pause(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    private MediaExtractor extractor(Context context) throws Exception {
        MediaExtractor extractor = new MediaExtractor();
        try (AssetFileDescriptor file = context.getAssets().openFd("baseline.mp4")) {
            extractor.setDataSource(file.getFileDescriptor(), file.getStartOffset(), file.getLength());
        }
        for (int i = 0; i < extractor.getTrackCount(); i++) {
            if (extractor.getTrackFormat(i).getString(MediaFormat.KEY_MIME).startsWith("audio/")) {
                extractor.selectTrack(i);
                return extractor;
            }
        }
        throw new IllegalStateException("no audio track");
    }

    private static MediaFormat audioFormat(MediaExtractor extractor) {
        for (int i = 0; i < extractor.getTrackCount(); i++) {
            MediaFormat format = extractor.getTrackFormat(i);
            if (format.getString(MediaFormat.KEY_MIME).startsWith("audio/")) return format;
        }
        return null;
    }

    /** A format's keys in order, each with its value (buffers by their size). */
    private static String keys(MediaFormat format) {
        StringBuilder out = new StringBuilder();
        for (String key : new TreeSet<>(format.getKeys())) {
            Object value;
            switch (format.getValueTypeForKey(key)) {
                case MediaFormat.TYPE_INTEGER: value = format.getInteger(key); break;
                case MediaFormat.TYPE_LONG: value = format.getLong(key) + "L"; break;
                case MediaFormat.TYPE_FLOAT: value = format.getFloat(key) + "f"; break;
                case MediaFormat.TYPE_STRING: value = "\"" + format.getString(key) + "\""; break;
                case MediaFormat.TYPE_BYTE_BUFFER: value = "buffer " + format.getByteBuffer(key).remaining(); break;
                default: value = "?";
            }
            out.append(out.length() == 0 ? "" : ", ").append(key).append('=').append(value);
        }
        return out.toString();
    }

    private void test(Context context) throws Exception {
        String first = list();
        MediaExtractor extractor = extractor(context);
        add("track: " + keys(audioFormat(extractor)));
        extractor.release();
        decode(context, "sync", MediaCodec.createDecoderByType(AAC), -1);
        decode(context, "seek", MediaCodec.createDecoderByType(AAC), 500_000);
        decodeAsync(context, "async", MediaCodec.createDecoderByType(AAC));
        if (first != null) decode(context, "by name", MediaCodec.createByCodecName(first), -1);
        try {
            MediaCodec.createByCodecName("c2.minato.nothing.decoder").release();
            add("unknown name: made");
        } catch (Exception e) {
            add("unknown name: " + e.getClass().getName());
        }
        try {
            MediaCodec.createDecoderByType("audio/x-nothing").release();
            add("unknown type: made");
        } catch (Exception e) {
            add("unknown type: " + e.getClass().getName());
        }
    }

    /** The list's AAC decoders; the first one's name. */
    private String list() {
        String first = null;
        for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.ALL_CODECS).getCodecInfos()) {
            if (info.isEncoder() || !Arrays.asList(info.getSupportedTypes()).contains(AAC)) continue;
            add("decoder " + info.getName() + ": canonical " + info.getCanonicalName() + ", alias " + info.isAlias()
                    + ", software " + info.isSoftwareOnly() + ", hardware " + info.isHardwareAccelerated()
                    + ", vendor " + info.isVendor());
            if (first != null) continue;
            first = info.getName();
            MediaCodecInfo.CodecCapabilities caps = info.getCapabilitiesForType(AAC);
            MediaCodecInfo.AudioCapabilities audio = caps.getAudioCapabilities();
            StringBuilder rates = new StringBuilder();
            for (Range<Integer> range : audio.getSupportedSampleRateRanges()) {
                rates.append(rates.length() == 0 ? "" : " ").append(range.getLower()).append('-')
                        .append(range.getUpper());
            }
            add(first + " audio: bitrates " + audio.getBitrateRange() + ", channels " + audio.getMaxInputChannelCount()
                    + ", rates " + rates + ", max instances " + caps.getMaxSupportedInstances());
            add(first + " default format: " + keys(caps.getDefaultFormat()));
        }
        return first;
    }

    /** What the decoded samples are: their count, RMS in 1 % steps and rising zero crossings, and their CRC32. */
    private static final class Samples {
        long count;
        double sum;
        int crossings;
        short last;
        final CRC32 crc = new CRC32();

        void add(ByteBuffer buffer, int offset, int size) {
            ByteBuffer bytes = buffer.duplicate().order(ByteOrder.LITTLE_ENDIAN);
            bytes.position(offset).limit(offset + size);
            byte[] copy = new byte[size];
            bytes.duplicate().get(copy);
            crc.update(copy);
            ShortBuffer shorts = bytes.slice().order(ByteOrder.LITTLE_ENDIAN).asShortBuffer();
            while (shorts.hasRemaining()) {
                short value = shorts.get();
                sum += (double) value * value;
                if (last < 0 && value >= 0) crossings++;
                last = value;
                count++;
            }
        }

        String describe() {
            long rms = count == 0 ? 0 : Math.round(100 * Math.sqrt(sum / count) / 32768);
            return count + " samples, rms " + rms + " %, " + crossings + " rising zero crossings";
        }
    }

    private void decode(Context context, String name, MediaCodec codec, long seekUs) throws Exception {
        MediaExtractor extractor = extractor(context);
        MediaFormat format = audioFormat(extractor);
        codec.configure(format, null, null, 0);
        codec.start();
        if (seekUs >= 0) {
            // Two units in first, then the seek and a flush, as a player seeks.
            for (int i = 0; i < 2; i++) queue(codec, extractor);
            extractor.seekTo(seekUs, MediaExtractor.SEEK_TO_CLOSEST_SYNC);
            codec.flush();
            add(name + ": flushed after 2 units, the extractor at " + extractor.getSampleTime());
        }
        int capacity = -1;
        boolean inputDone = false;
        int outputs = 0;
        long firstTime = 0, lastTime = 0;
        int firstSize = -1;
        Samples samples = new Samples();
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        long deadline = System.currentTimeMillis() + 20_000;
        while (System.currentTimeMillis() < deadline) {
            if (!inputDone) {
                int index = codec.dequeueInputBuffer(WAIT_US);
                if (index >= 0) {
                    ByteBuffer buffer = codec.getInputBuffer(index);
                    if (capacity < 0) capacity = buffer.capacity();
                    int size = extractor.readSampleData(buffer, 0);
                    if (size < 0) {
                        codec.queueInputBuffer(index, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                        inputDone = true;
                    } else {
                        codec.queueInputBuffer(index, 0, size, extractor.getSampleTime(), 0);
                        extractor.advance();
                    }
                }
            }
            int index = codec.dequeueOutputBuffer(info, WAIT_US);
            if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                add(name + ": output format: " + keys(codec.getOutputFormat()));
            } else if (index >= 0) {
                if (info.size > 0) {
                    outputs++;
                    if (firstSize < 0) {
                        firstTime = info.presentationTimeUs;
                        firstSize = info.size;
                    }
                    lastTime = info.presentationTimeUs;
                    samples.add(codec.getOutputBuffer(index), info.offset, info.size);
                }
                codec.releaseOutputBuffer(index, false);
                if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                    add(name + ": end of stream");
                    break;
                }
            }
        }
        add(name + ": input capacity at least 1024: " + (capacity >= 1024));
        add(name + ": " + outputs + " output buffers, times " + firstTime + " to " + lastTime + ", the first "
                + firstSize / 2 + " samples");
        add(name + ": " + samples.describe());
        add("device " + name + ": samples' CRC32 " + Long.toHexString(samples.crc.getValue()) + ", input capacity "
                + capacity);
        codec.stop();
        codec.release();
        extractor.release();
    }

    private static void queue(MediaCodec codec, MediaExtractor extractor) {
        int index = codec.dequeueInputBuffer(1_000_000);
        if (index < 0) return;
        int size = extractor.readSampleData(codec.getInputBuffer(index), 0);
        codec.queueInputBuffer(index, 0, Math.max(size, 0), extractor.getSampleTime(), 0);
        extractor.advance();
    }

    private void decodeAsync(Context context, String name, MediaCodec codec) throws Exception {
        MediaExtractor extractor = extractor(context);
        MediaFormat format = audioFormat(extractor);
        HandlerThread thread = new HandlerThread("codec");
        thread.start();
        CountDownLatch ended = new CountDownLatch(1);
        Samples samples = new Samples();
        int[] counts = new int[2];  // output buffers with samples, format changes
        String[] formats = new String[1];
        codec.setCallback(new MediaCodec.Callback() {
            @Override
            public void onInputBufferAvailable(MediaCodec mc, int index) {
                ByteBuffer buffer = mc.getInputBuffer(index);
                int size = extractor.readSampleData(buffer, 0);
                if (size < 0) {
                    mc.queueInputBuffer(index, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                } else {
                    mc.queueInputBuffer(index, 0, size, extractor.getSampleTime(), 0);
                    extractor.advance();
                }
            }

            @Override
            public void onOutputBufferAvailable(MediaCodec mc, int index, MediaCodec.BufferInfo info) {
                if (info.size > 0) {
                    counts[0]++;
                    samples.add(mc.getOutputBuffer(index), info.offset, info.size);
                }
                mc.releaseOutputBuffer(index, false);
                if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) ended.countDown();
            }

            @Override
            public void onError(MediaCodec mc, MediaCodec.CodecException e) {
                formats[0] = "error " + e.getErrorCode();
                ended.countDown();
            }

            @Override
            public void onOutputFormatChanged(MediaCodec mc, MediaFormat changed) {
                counts[1]++;
                formats[0] = keys(changed);
            }
        }, new Handler(thread.getLooper()));
        codec.configure(format, null, null, 0);
        codec.start();
        add(name + ": ended: " + ended.await(20, TimeUnit.SECONDS));
        add(name + ": " + counts[1] + " format changes, the last: " + formats[0]);
        add(name + ": " + counts[0] + " output buffers, " + samples.describe());
        codec.stop();
        codec.release();
        thread.quitSafely();
        extractor.release();
    }
}
