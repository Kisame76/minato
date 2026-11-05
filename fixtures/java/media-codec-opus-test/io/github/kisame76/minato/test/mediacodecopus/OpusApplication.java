package io.github.kisame76.minato.test.mediacodecopus;

import android.app.Application;
import android.content.Context;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.util.Range;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.TreeSet;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.zip.CRC32;

/**
 * ADR-0060 e2's test of MediaCodec's Opus decoder. The app splits media-player-ogg-test's Ogg Opus files into packets
 * itself, each at the time of its first frame by the packets' tables of contents, and gives the decoder OpusHead as
 * csd-0, the pre-skip in nanoseconds as csd-1 and a seek pre-roll of 80 ms as csd-2, as ExoPlayer does. Opus
 * decoders differ in the last bits of their samples, so those are compared coarsely: their count, the RMS in steps of
 * 1 % of full scale, the rising zero crossings (each after the samples were below -1/32 of full scale) and the first
 * one's index, all of the first channel; their CRC32 goes to the device tag.
 * From a thread that Application.attachBaseContext starts:
 * - MediaCodecList: the decoders of audio/opus by name, alias and flags, and the first one's audio capabilities and
 *   default format;
 * - each stream decoded synchronously: the input buffer's capacity, each output format change with its keys, the
 *   output buffers' count (and the empty ones'), the first and last times and frames, the end of the stream, and the
 *   samples; a codec error by its exception's class;
 * - stereo and mono, across a flush (on mid-stream, and on at the start), the end flagged on the last packet, the
 *   configuration as units (flagged as codec config, and not), csd-0 alone, Android's unified csd-0 (its AOPUSHDR,
 *   AOPUSDLY and AOPUSPRL markers), a configure of another rate and channels than the stream's, OpusHead's output
 *   gain, a csd-0 that is not OpusHead, three channels in mapping family 0, a packet libopus refuses, asynchronously
 *   and by name and alias;
 * - surround in mapping family 1 (opus51.ogg, 5.1, and opus71.ogg, 7.1, a sine of its own in each channel), whose
 *   samples are described channel by channel, in the stream map's order: whole, across a flush, in family 255, with a
 *   channel the map leaves silent, with a stream count that does not fit the channels, configured as stereo, and with
 *   a channel mask configured.
 * Each wait is bounded; it logs with the tag media-codec-opus-test and ends with "done".
 */
public class OpusApplication extends Application {
    private static final String TAG = "media-codec-opus-test";
    private static final String OPUS = MediaFormat.MIMETYPE_AUDIO_OPUS;
    private static final long WAIT_US = 10_000;
    private static final long PRE_ROLL_NS = 80_000_000;

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

    /** A stream the app queues itself: its format, its configuration units, its packets and their times. */
    private static final class Stream {
        final MediaFormat format;
        final List<byte[]> configs = new ArrayList<>();  // queued before the packets, with configFlags
        int configFlags;
        final List<byte[]> units = new ArrayList<>();
        final List<Long> times = new ArrayList<>();
        boolean endWithLast;  // the end of the stream flagged on the last unit, not on an empty buffer after it

        Stream(MediaFormat format) {
            this.format = format;
        }
    }

    private static byte[] read(Context context, String name) throws Exception {
        try (InputStream in = context.getAssets().open(name)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] chunk = new byte[8192];
            for (int count; (count = in.read(chunk)) > 0;) out.write(chunk, 0, count);
            return out.toByteArray();
        }
    }

    private static int u8(byte[] data, int at) {
        return data[at] & 0xff;
    }

    /** An Ogg file's packets, of its one logical stream, joined across pages by their lacing values. */
    private static List<byte[]> oggPackets(byte[] data) {
        List<byte[]> packets = new ArrayList<>();
        ByteArrayOutputStream partial = new ByteArrayOutputStream();
        int at = 0;
        while (at + 27 <= data.length && data[at] == 'O' && data[at + 1] == 'g' && data[at + 2] == 'g'
                && data[at + 3] == 'S') {
            int segments = u8(data, at + 26);
            int body = at + 27 + segments;
            for (int i = 0; i < segments; i++) {
                int lace = u8(data, at + 27 + i);
                partial.write(data, body, lace);
                body += lace;
                if (lace < 255) {
                    packets.add(partial.toByteArray());
                    partial.reset();
                }
            }
            at = body;
        }
        return packets;
    }

    /** An Opus packet's frames at 48000 Hz by its table of contents (RFC 6716 section 3.1). */
    private static int opusFrames(byte[] packet) {
        int config = u8(packet, 0) >> 3, code = u8(packet, 0) & 3;
        int size = config < 12 ? new int[] {480, 960, 1920, 2880}[config & 3]
                : config < 16 ? ((config & 1) == 0 ? 480 : 960) : new int[] {120, 240, 480, 960}[config & 3];
        int count = code == 0 ? 1 : code < 3 ? 2 : packet.length > 1 ? u8(packet, 1) & 0x3f : 0;
        return size * count;
    }

    private static byte[] nanos(long value) {
        return ByteBuffer.allocate(8).order(ByteOrder.nativeOrder()).putLong(value).array();
    }

    /** OpusHead's pre-skip. */
    private static int preSkip(byte[] head) {
        return u8(head, 10) | u8(head, 11) << 8;
    }

    /**
     * An Ogg Opus file as an app with its own extractor queues it: its audio packets at the times of their first
     * frames, OpusHead and the pre-skip and pre-roll in nanoseconds as csd-0, csd-1 and csd-2 where `csd` is set,
     * else as the stream's configuration units.
     */
    private static Stream opus(byte[] file, boolean csd) {
        List<byte[]> packets = oggPackets(file);
        byte[] head = packets.get(0);
        MediaFormat format = MediaFormat.createAudioFormat(OPUS, 48000, u8(head, 9));
        Stream stream = new Stream(format);
        byte[][] configs = {head, nanos(preSkip(head) * 1_000_000_000L / 48000), nanos(PRE_ROLL_NS)};
        for (int i = 0; i < configs.length; i++) {
            if (csd) format.setByteBuffer("csd-" + i, ByteBuffer.wrap(configs[i]));
            else stream.configs.add(configs[i]);
        }
        long frames = 0;
        for (byte[] packet : packets.subList(2, packets.size())) {  // OpusHead and OpusTags left out
            stream.units.add(packet);
            stream.times.add(frames * 1_000_000 / 48000);
            frames += opusFrames(packet);
        }
        return stream;
    }

    /** Android's unified csd-0 (OpusHeader.cpp's WriteOpusHeaders): its three parts after their markers. */
    private static byte[] unified(byte[] head, long delayNs, long preRollNs) {
        ByteBuffer out = ByteBuffer.allocate(16 + head.length + 2 * 24).order(ByteOrder.LITTLE_ENDIAN);
        out.put("AOPUSHDR".getBytes()).putLong(head.length).put(head);
        out.put("AOPUSDLY".getBytes()).putLong(8).putLong(delayNs);
        out.put("AOPUSPRL".getBytes()).putLong(8).putLong(preRollNs);
        return out.array();
    }

    private void test(Context context) throws Exception {
        String opus = list(OPUS);
        byte[] stereo = read(context, "opus.ogg"), mono = read(context, "opus-mono.ogg");
        add("opus track: " + keys(opus(stereo, true).format) + ", " + opus(stereo, true).units.size() + " units");
        decode("opus", MediaCodec.createDecoderByType(OPUS), opus(stereo, true), -1);
        add("opus mono track: " + keys(opus(mono, true).format) + ", " + opus(mono, true).units.size() + " units");
        decode("opus mono", MediaCodec.createDecoderByType(OPUS), opus(mono, true), -1);
        decode("opus seek", MediaCodec.createDecoderByType(OPUS), opus(stereo, true), 20);
        decode("opus seek to the start", MediaCodec.createDecoderByType(OPUS), opus(stereo, true), 0);
        decode("opus mono seek", MediaCodec.createDecoderByType(OPUS), opus(mono, true), 5);
        Stream ending = opus(stereo, true);
        ending.endWithLast = true;
        decode("opus end with the last", MediaCodec.createDecoderByType(OPUS), ending, -1);
        Stream flagged = opus(stereo, false);
        flagged.configFlags = MediaCodec.BUFFER_FLAG_CODEC_CONFIG;
        decode("opus config units", MediaCodec.createDecoderByType(OPUS), flagged, -1);
        decode("opus config units unflagged", MediaCodec.createDecoderByType(OPUS), opus(stereo, false), -1);
        Stream alone = opus(stereo, true);
        alone.format.removeKey("csd-1");
        alone.format.removeKey("csd-2");
        decode("opus csd-0 alone", MediaCodec.createDecoderByType(OPUS), alone, -1);
        Stream joined = opus(stereo, true);
        byte[] head = oggPackets(stereo).get(0);
        joined.format.removeKey("csd-1");
        joined.format.removeKey("csd-2");
        joined.format.setByteBuffer("csd-0",
                ByteBuffer.wrap(unified(head, preSkip(head) * 1_000_000_000L / 48000, PRE_ROLL_NS)));
        decode("opus unified csd-0", MediaCodec.createDecoderByType(OPUS), joined, 20);
        Stream other = opus(stereo, true);
        other.format.setInteger(MediaFormat.KEY_SAMPLE_RATE, 44100);
        other.format.setInteger(MediaFormat.KEY_CHANNEL_COUNT, 1);
        decode("opus configured as mono at 44100", MediaCodec.createDecoderByType(OPUS), other, -1);
        Stream louder = opus(stereo, true);
        byte[] gained = head.clone();
        gained[16] = 0x00;
        gained[17] = 0x06;  // +6 dB in Q7.8
        louder.format.setByteBuffer("csd-0", ByteBuffer.wrap(gained));
        decode("opus output gain", MediaCodec.createDecoderByType(OPUS), louder, -1);
        Stream wrong = opus(stereo, true);
        byte[] notHead = head.clone();
        notHead[0] = 'X';
        wrong.format.setByteBuffer("csd-0", ByteBuffer.wrap(notHead));
        decode("opus csd-0 not OpusHead", MediaCodec.createDecoderByType(OPUS), wrong, -1);
        Stream three = opus(stereo, true);
        byte[] threeHead = head.clone();
        threeHead[9] = 3;
        three.format.setByteBuffer("csd-0", ByteBuffer.wrap(threeHead));
        decode("opus three channels in family 0", MediaCodec.createDecoderByType(OPUS), three, -1);
        Stream refused = opus(stereo, true);
        int bad = refused.units.size() / 2;
        refused.units.set(bad, new byte[] {(byte) (refused.units.get(bad)[0] | 3), 0});  // code 3, no frames
        decode("opus refused packet", MediaCodec.createDecoderByType(OPUS), refused, -1);
        decodeAsync("opus async", MediaCodec.createDecoderByType(OPUS), opus(stereo, true));
        surround(context);
        if (opus != null) decode("opus by name", MediaCodec.createByCodecName(opus), opus(stereo, true), -1);
        decode("opus by alias", MediaCodec.createByCodecName("OMX.google.opus.decoder"), opus(mono, true), -1);
    }

    /** Mapping family 1: OpusHead's stream count at 19, coupled count at 20 and stream map from 21. */
    private void surround(Context context) throws Exception {
        byte[] file51 = read(context, "opus51.ogg"), file71 = read(context, "opus71.ogg");
        byte[] head = oggPackets(file51).get(0);
        add("opus 5.1 track: " + keys(opus(file51, true).format) + ", " + opus(file51, true).units.size() + " units, "
                + "family " + u8(head, 18) + ", streams " + u8(head, 19) + ", coupled " + u8(head, 20));
        decode("opus 5.1", MediaCodec.createDecoderByType(OPUS), opus(file51, true), -1);
        decode("opus 7.1", MediaCodec.createDecoderByType(OPUS), opus(file71, true), -1);
        decode("opus 5.1 seek", MediaCodec.createDecoderByType(OPUS), opus(file51, true), 20);
        Stream family = opus(file51, true);
        byte[] other = head.clone();
        other[18] = (byte) 255;
        family.format.setByteBuffer("csd-0", ByteBuffer.wrap(other));
        decode("opus 5.1 in family 255", MediaCodec.createDecoderByType(OPUS), family, -1);
        Stream silent = opus(file51, true);
        byte[] unmapped = head.clone();
        unmapped[21 + 5] = (byte) 255;  // the last channel: none of the streams'
        silent.format.setByteBuffer("csd-0", ByteBuffer.wrap(unmapped));
        decode("opus 5.1 with a silent channel", MediaCodec.createDecoderByType(OPUS), silent, -1);
        Stream wrong = opus(file51, true);
        byte[] streams = head.clone();
        streams[19] = 3;  // three streams and two coupled: five channels, not six
        wrong.format.setByteBuffer("csd-0", ByteBuffer.wrap(streams));
        decode("opus 5.1 with streams that do not fit", MediaCodec.createDecoderByType(OPUS), wrong, -1);
        Stream asStereo = opus(file51, true);
        asStereo.format.setInteger(MediaFormat.KEY_CHANNEL_COUNT, 2);
        decode("opus 5.1 configured as stereo", MediaCodec.createDecoderByType(OPUS), asStereo, -1);
        Stream masked = opus(file51, true);
        masked.format.setInteger(MediaFormat.KEY_CHANNEL_MASK, 0xfc);  // AudioFormat.CHANNEL_OUT_5POINT1
        decode("opus 5.1 with a channel mask", MediaCodec.createDecoderByType(OPUS), masked, -1);
    }

    /** The list's decoders of a type; the first one's name. */
    private String list(String type) {
        String first = null;
        for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.ALL_CODECS).getCodecInfos()) {
            if (info.isEncoder() || !Arrays.asList(info.getSupportedTypes()).contains(type)) continue;
            add("decoder " + info.getName() + ": canonical " + info.getCanonicalName() + ", alias " + info.isAlias()
                    + ", software " + info.isSoftwareOnly() + ", hardware " + info.isHardwareAccelerated()
                    + ", vendor " + info.isVendor());
            if (first != null) continue;
            first = info.getName();
            MediaCodecInfo.CodecCapabilities caps = info.getCapabilitiesForType(type);
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

    /** What the decoded samples are, of the first channel: their count, RMS in 1 % steps, rising zero crossings and
     *  the first one's frame; of more than two channels, each channel's RMS and crossings too; and the CRC32 of all of
     *  them. */
    private static final class Samples {
        int channels;
        long frames;
        double sum;
        int crossings;
        long firstCrossing = -1;
        boolean below;  // below -1/32 of full scale since the last crossing
        final double[] sums = new double[8];
        final int[] crossingsOf = new int[8];
        final boolean[] belowOf = new boolean[8];
        final CRC32 crc = new CRC32();

        void shape(MediaFormat format) {
            if (format.containsKey(MediaFormat.KEY_CHANNEL_COUNT)) {
                channels = format.getInteger(MediaFormat.KEY_CHANNEL_COUNT);
            }
        }

        int frameSize() {
            return Math.max(channels, 1) * 2;
        }

        void add(ByteBuffer buffer, int offset, int size) {
            ByteBuffer bytes = buffer.duplicate().order(ByteOrder.LITTLE_ENDIAN);
            bytes.position(offset).limit(offset + size);
            byte[] copy = new byte[size];
            bytes.duplicate().get(copy);
            crc.update(copy);
            ByteBuffer samples = bytes.slice().order(ByteOrder.LITTLE_ENDIAN);
            int step = frameSize();
            for (int at = 0; at + step <= size; at += step) {
                double value = samples.getShort(at) / 32768.0;
                for (int c = 0; c < Math.min(channels, 8); c++) {
                    double each = samples.getShort(at + 2 * c) / 32768.0;
                    sums[c] += each * each;
                    if (each < -1.0 / 32) {
                        belowOf[c] = true;
                    } else if (belowOf[c] && each >= 0) {
                        belowOf[c] = false;
                        crossingsOf[c]++;
                    }
                }
                frames++;
                sum += value * value;
                if (value < -1.0 / 32) {
                    below = true;
                } else if (below && value >= 0) {
                    below = false;
                    crossings++;
                    if (firstCrossing < 0) firstCrossing = frames - 1;
                }
            }
        }

        String describe() {
            long rms = frames == 0 ? 0 : Math.round(100 * Math.sqrt(sum / frames));
            String out = frames + " frames, rms " + rms + " %, " + crossings + " rising zero crossings, the first at "
                    + firstCrossing;
            if (channels <= 2) return out;
            StringBuilder each = new StringBuilder();
            for (int c = 0; c < Math.min(channels, 8); c++) {
                long level = frames == 0 ? 0 : Math.round(100 * Math.sqrt(sums[c] / frames));
                each.append(c == 0 ? "" : ", ").append(level).append(" % ").append(crossingsOf[c]);
            }
            return out + "; by channel (rms, crossings): " + each;
        }
    }

    /**
     * Decodes a stream synchronously; from unit `resume` on after two units and a flush where it is not -1. A codec
     * error is logged by its exception's class.
     */
    private void decode(String name, MediaCodec codec, Stream stream, int resume) throws Exception {
        try {
            codec.configure(stream.format, null, null, 0);
        } catch (IllegalArgumentException e) {
            add(name + ": configure " + e.getClass().getName());
            codec.release();
            return;
        }
        codec.start();
        Samples samples = new Samples();
        samples.shape(stream.format);
        int next = 0;
        int capacity = -1;
        boolean inputDone = false, failed = false;
        int outputs = 0, empty = 0;
        long firstTime = 0, lastTime = 0, firstFrames = -1, lastFrames = 0;
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        long deadline = System.currentTimeMillis() + 20_000;
        try {
            for (byte[] config : stream.configs) {
                int index = codec.dequeueInputBuffer(1_000_000);
                codec.getInputBuffer(index).put(config);
                codec.queueInputBuffer(index, 0, config.length, 0, stream.configFlags);
            }
            if (resume >= 0) {
                for (; next < 2; next++) {
                    int index = codec.dequeueInputBuffer(1_000_000);
                    codec.getInputBuffer(index).put(stream.units.get(next));
                    codec.queueInputBuffer(index, 0, stream.units.get(next).length, stream.times.get(next), 0);
                }
                codec.flush();
                next = resume;
                add(name + ": flushed after 2 units, on at " + stream.times.get(next));
            }
            while (System.currentTimeMillis() < deadline) {
                if (!inputDone) {
                    int index = codec.dequeueInputBuffer(WAIT_US);
                    if (index >= 0) {
                        ByteBuffer buffer = codec.getInputBuffer(index);
                        if (capacity < 0) capacity = buffer.capacity();
                        if (next >= stream.units.size()) {
                            codec.queueInputBuffer(index, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                            inputDone = true;
                        } else {
                            long time = stream.times.get(next);
                            buffer.put(stream.units.get(next++));
                            boolean last = stream.endWithLast && next == stream.units.size();
                            codec.queueInputBuffer(index, 0, buffer.position(), time,
                                    last ? MediaCodec.BUFFER_FLAG_END_OF_STREAM : 0);
                            inputDone = last;
                        }
                    }
                }
                int index = codec.dequeueOutputBuffer(info, WAIT_US);
                if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    MediaFormat format = codec.getOutputFormat();
                    add(name + ": output format: " + keys(format));
                    samples.shape(format);
                } else if (index >= 0) {
                    if (info.size > 0) {
                        outputs++;
                        long frames = info.size / samples.frameSize();
                        if (firstFrames < 0) {
                            firstTime = info.presentationTimeUs;
                            firstFrames = frames;
                        }
                        lastTime = info.presentationTimeUs;
                        lastFrames = frames;
                        samples.add(codec.getOutputBuffer(index), info.offset, info.size);
                    } else if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) == 0) {
                        empty++;
                    }
                    codec.releaseOutputBuffer(index, false);
                    if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                        add(name + ": end of stream, " + info.size + " bytes with it");
                        break;
                    }
                }
            }
        } catch (IllegalStateException e) {
            add(name + ": " + e.getClass().getName());
            failed = true;
        }
        // After an error, how far the codec got before the app saw it depends on the threads: the device tag's.
        String counts = failed ? "device " + name + ": after " + next + " units" : name;
        add(counts + ": input capacity " + capacity);
        add(counts + ": " + outputs + " output buffers and " + empty + " empty ones, times " + firstTime + " to "
                + lastTime + ", the first " + firstFrames + " frames, the last " + lastFrames);
        add(counts + ": " + samples.describe());
        add("device " + name + ": samples' CRC32 " + Long.toHexString(samples.crc.getValue()));
        try {
            codec.stop();
        } catch (IllegalStateException e) {
            add(name + ": stop " + e.getClass().getName());
        }
        codec.release();
    }

    private void decodeAsync(String name, MediaCodec codec, Stream stream) throws Exception {
        HandlerThread thread = new HandlerThread("codec");
        thread.start();
        CountDownLatch ended = new CountDownLatch(1);
        Samples samples = new Samples();
        samples.shape(stream.format);
        int[] counts = new int[3];  // output buffers with samples, format changes, the next unit
        String[] formats = new String[1];
        codec.setCallback(new MediaCodec.Callback() {
            @Override
            public void onInputBufferAvailable(MediaCodec mc, int index) {
                int next = counts[2];
                if (next > stream.units.size()) return;
                counts[2]++;
                if (next == stream.units.size()) {
                    mc.queueInputBuffer(index, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                } else {
                    mc.getInputBuffer(index).put(stream.units.get(next));
                    mc.queueInputBuffer(index, 0, stream.units.get(next).length, stream.times.get(next), 0);
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
                samples.shape(changed);
            }
        }, new Handler(thread.getLooper()));
        codec.configure(stream.format, null, null, 0);
        codec.start();
        add(name + ": ended: " + ended.await(20, TimeUnit.SECONDS));
        add(name + ": " + counts[1] + " format changes, the last: " + formats[0]);
        add(name + ": " + counts[0] + " output buffers, " + samples.describe());
        codec.stop();
        codec.release();
        thread.quitSafely();
    }
}
