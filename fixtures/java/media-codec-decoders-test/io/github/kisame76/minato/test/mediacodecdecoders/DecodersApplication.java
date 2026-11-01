package io.github.kisame76.minato.test.mediacodecdecoders;

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
 * ADR-0060 e2f.2's test of MediaCodec's MP3, FLAC and raw decoders. The app splits soundpool-codec-test's tones into
 * units itself (MP3 frames by their headers, the LAME tag's encoder delay and padding into the format as MP3Extractor
 * puts them; FLAC frames by their sync code and header CRC, the stream's metadata as csd-0) and makes the raw samples
 * itself (a sine, 16-bit and float). MP3 decoders differ in the last bits of their samples, so those are compared
 * coarsely: their count, the RMS in steps of 1 % of full scale, the rising zero crossings (each after the samples
 * were below -1/32 of full scale, so that the noise around silence, where decoders differ, does not count) and the
 * first one's index, all of the first channel; their CRC32 goes to the device tag. FLAC and raw are lossless, so
 * their CRC32 is logged.
 * From a thread that Application.attachBaseContext starts:
 * - MediaCodecList: the decoders of audio/mpeg, audio/flac and audio/raw by name, alias and flags, and the first one's
 *   audio capabilities and default format;
 * - each stream decoded synchronously: the input buffer's capacity, each output format change with its keys, the
 *   output buffers' count (and the empty ones'), the first and last times and frames, the end of the stream, and the
 *   samples; a configure refused, or a codec error, by its exception's class;
 * - MP3 with three frames in a buffer, across a flush, the end flagged on its last frame, of MPEG-2 (mono,
 *   22050 Hz), asynchronously, with a frame mid-stream whose side information pvmp3 refuses, and changing from MPEG-1
 *   stereo to MPEG-2 mono mid-stream (its measures leave out the first 1152 frames after the change);
 * - FLAC to float samples, across a flush, with its metadata as a unit instead of csd-0 (flagged as codec config,
 *   not flagged, and flagged with the end of the stream too), and with a corrupt frame, also followed by a flush;
 * - raw to 16-bit and float samples, trimmed by an encoder delay and padding, and a format without a sample rate.
 * Each wait is bounded; it logs with the tag media-codec-decoders-test and ends with "done".
 */
public class DecodersApplication extends Application {
    private static final String TAG = "media-codec-decoders-test";
    private static final String MP3 = MediaFormat.MIMETYPE_AUDIO_MPEG;
    private static final String FLAC = MediaFormat.MIMETYPE_AUDIO_FLAC;
    private static final String RAW = MediaFormat.MIMETYPE_AUDIO_RAW;
    private static final long WAIT_US = 10_000;
    private static final int PCM_16 = 2;     // AudioFormat.ENCODING_PCM_16BIT
    private static final int PCM_FLOAT = 4;  // AudioFormat.ENCODING_PCM_FLOAT

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

    /** A stream the app queues itself: its format, its units and their times. */
    private static final class Stream {
        final MediaFormat format;
        final List<byte[]> units = new ArrayList<>();
        final List<Long> times = new ArrayList<>();
        byte[] header;  // FLAC's metadata, for a codec config unit
        boolean endWithLast;  // the end of the stream flagged on the last unit, not on an empty buffer after it
        long leaveOut;  // the frames after a change of rate that the measures of the samples leave out

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

    /** An MP3 frame's size, rate, channels and samples from its header, as C2SoftMp3Dec's parseMp3Header; 0. */
    private static int mp3Frame(byte[] data, int at, int[] shape) {
        if (at + 4 > data.length) return 0;
        int header = u8(data, at) << 24 | u8(data, at + 1) << 16 | u8(data, at + 2) << 8 | u8(data, at + 3);
        if ((header & 0xffe00000) != 0xffe00000) return 0;
        int version = header >> 19 & 3, layer = header >> 17 & 3, bitrate = header >> 12 & 15, rate = header >> 10 & 3;
        if (version == 1 || layer != 1 || bitrate == 0 || bitrate == 15 || rate == 3) return 0;  // Layer III only
        int[] rates = {44100, 48000, 32000};
        int[] v1 = {32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
        int[] v2 = {8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
        int hz = rates[rate] >> (version == 3 ? 0 : version == 2 ? 1 : 2);
        int kbps = version == 3 ? v1[bitrate - 1] : v2[bitrate - 1];
        shape[0] = hz;
        shape[1] = (header >> 6 & 3) == 3 ? 1 : 2;
        shape[2] = version == 3 ? 1152 : 576;
        return (version == 3 ? 144000 : 72000) * kbps / hz + (header >> 9 & 1);
    }

    /** An MP3 file's frames after its ID3 tag; its Info or Xing frame left out, its LAME tag's delay and padding. */
    private static Stream mp3(byte[] data) {
        int at = 0;
        if (data.length > 10 && data[0] == 'I' && data[1] == 'D' && data[2] == '3') {
            at = 10 + ((data[6] & 0x7f) << 21 | (data[7] & 0x7f) << 14 | (data[8] & 0x7f) << 7 | (data[9] & 0x7f));
        }
        int[] shape = new int[3];
        int first = mp3Frame(data, at, shape);
        MediaFormat format = MediaFormat.createAudioFormat(MP3, shape[0], shape[1]);
        boolean mpeg1 = shape[2] == 1152;
        int xing = at + 4 + (mpeg1 ? (shape[1] == 1 ? 17 : 32) : (shape[1] == 1 ? 9 : 17));
        String tag = new String(data, xing, 4);
        if (tag.equals("Info") || tag.equals("Xing")) {
            int lame = xing + 120;
            if (new String(data, lame, 4).equals("LAME")) {
                int delay = u8(data, lame + 21) << 4 | u8(data, lame + 22) >> 4;
                int padding = (u8(data, lame + 22) & 0xf) << 8 | u8(data, lame + 23);
                format.setInteger(MediaFormat.KEY_ENCODER_DELAY, delay);
                format.setInteger(MediaFormat.KEY_ENCODER_PADDING, padding);
            }
            at += first;
        }
        Stream stream = new Stream(format);
        long frames = 0;
        for (int size; (size = mp3Frame(data, at, shape)) > 0 && at + size <= data.length; at += size) {
            stream.units.add(Arrays.copyOfRange(data, at, at + size));
            stream.times.add(frames * 1_000_000 / shape[0]);
            frames += shape[2];
        }
        return stream;
    }

    /**
     * Breaks an MPEG-1 frame's side information as pvmp3_get_side_info refuses it (SIDE_INFO_ERROR, which
     * C2SoftMp3Dec gives as silence): the first granule's first channel switches windows with block type 0.
     */
    private static void breakSideInfo(byte[] frame) {
        boolean mono = u8(frame, 3) >> 6 == 3;
        int bit = ((frame[1] & 1) == 0 ? 48 : 32) + 9 + (mono ? 5 + 4 : 3 + 8) + 12 + 9 + 8 + 4;
        frame[bit >> 3] |= (byte) (0x80 >> (bit & 7));  // window_switching_flag
        for (int i = bit + 1; i <= bit + 2; i++) frame[i >> 3] &= (byte) ~(0x80 >> (i & 7));  // block_type
    }

    /** CRC-8 of a FLAC frame header (polynomial 0x07). */
    private static int crc8(byte[] data, int from, int to) {
        int crc = 0;
        for (int i = from; i < to; i++) {
            crc ^= data[i] & 0xff;
            for (int bit = 0; bit < 8; bit++) crc = (crc & 0x80) != 0 ? (crc << 1 ^ 7) & 0xff : crc << 1 & 0xff;
        }
        return crc;
    }

    /** Whether a FLAC frame header starts at `at`: its sync code and its CRC-8. */
    private static boolean flacFrame(byte[] data, int at) {
        if (at + 6 > data.length || u8(data, at) != 0xff || (u8(data, at + 1) & 0xfe) != 0xf8) return false;
        int block = u8(data, at + 2) >> 4, rate = u8(data, at + 2) & 15, lead = u8(data, at + 4);
        int length = 5 + (lead < 0x80 ? 0 : lead < 0xe0 ? 1 : lead < 0xf0 ? 2 : lead < 0xf8 ? 3 : lead < 0xfc ? 4
                : lead < 0xfe ? 5 : 6);
        length += block == 6 ? 1 : block == 7 ? 2 : 0;
        length += rate == 12 ? 1 : rate == 13 || rate == 14 ? 2 : 0;
        return at + length < data.length && crc8(data, at, at + length) == u8(data, at + length);
    }

    /** A FLAC file's frames, each unit at the time of its first sample; its metadata as csd-0. */
    private static Stream flac(byte[] data, boolean csd) {
        int at = 4;
        for (boolean last = false; !last;) {
            last = (data[at] & 0x80) != 0;
            at += 4 + (u8(data, at + 1) << 16 | u8(data, at + 2) << 8 | u8(data, at + 3));
        }
        int rate = u8(data, 18) << 12 | u8(data, 19) << 4 | u8(data, 20) >> 4;
        int channels = (u8(data, 20) >> 1 & 7) + 1;
        MediaFormat format = MediaFormat.createAudioFormat(FLAC, rate, channels);
        Stream stream = new Stream(format);
        stream.header = Arrays.copyOf(data, at);
        if (csd) format.setByteBuffer("csd-0", ByteBuffer.wrap(stream.header));
        long frames = 0;
        int block = u8(data, 10) << 8 | u8(data, 11);  // STREAMINFO's maximum, every block's but the last here
        while (at < data.length) {
            int end = at + 1;
            while (end < data.length && !flacFrame(data, end)) end++;
            stream.units.add(Arrays.copyOfRange(data, at, end));
            stream.times.add(frames * 1_000_000 / rate);
            frames += block;
            at = end;
        }
        return stream;
    }

    /** 0.25 s of a 441 Hz sine, stereo at 44100 Hz, 16-bit or float, in units of 1024 frames. */
    private static Stream raw(int encoding) {
        MediaFormat format = MediaFormat.createAudioFormat(RAW, 44100, 2);
        if (encoding != PCM_16) format.setInteger(MediaFormat.KEY_PCM_ENCODING, encoding);
        Stream stream = new Stream(format);
        int size = encoding == PCM_16 ? 2 : 4;
        for (int start = 0; start < 11025; start += 1024) {
            int frames = Math.min(1024, 11025 - start);
            ByteBuffer unit = ByteBuffer.allocate(frames * 2 * size).order(ByteOrder.LITTLE_ENDIAN);
            for (int i = start; i < start + frames; i++) {
                double value = 0.5 * Math.sin(2 * Math.PI * 441 * i / 44100);
                for (int channel = 0; channel < 2; channel++) {
                    if (size == 2) unit.putShort((short) Math.round(value * 32767));
                    else unit.putFloat((float) value);
                }
            }
            stream.units.add(unit.array());
            stream.times.add(start * 1_000_000L / 44100);
        }
        return stream;
    }

    private void test(Context context) throws Exception {
        String mp3 = list(MP3), flac = list(FLAC), raw = list(RAW);
        byte[] tone = read(context, "tone.mp3"), mono = read(context, "tone-mono.mp3");
        byte[] lossless = read(context, "tone.flac");
        add("mp3 track: " + keys(mp3(tone).format) + ", " + mp3(tone).units.size() + " units");
        decode("mp3", MediaCodec.createDecoderByType(MP3), mp3(tone), 1, -1, -1);
        decode("mp3 by three", MediaCodec.createDecoderByType(MP3), mp3(tone), 3, -1, -1);
        decode("mp3 seek", MediaCodec.createDecoderByType(MP3), mp3(tone), 1, 10, -1);
        Stream ending = mp3(tone);
        ending.endWithLast = true;
        decode("mp3 end with the last", MediaCodec.createDecoderByType(MP3), ending, 1, -1, -1);
        add("mp3 mono track: " + keys(mp3(mono).format) + ", " + mp3(mono).units.size() + " units");
        decode("mp3 mono", MediaCodec.createDecoderByType(MP3), mp3(mono), 1, -1, -1);
        decodeAsync("mp3 async", MediaCodec.createDecoderByType(MP3), mp3(tone));
        Stream broken = mp3(tone);
        breakSideInfo(broken.units.get(broken.units.size() / 2));
        decode("mp3 bad side info", MediaCodec.createDecoderByType(MP3), broken, 1, -1, -1);
        Stream change = mp3(tone), second = mp3(mono);
        long after = change.times.get(change.times.size() - 1)
                + 1152 * 1_000_000L / change.format.getInteger(MediaFormat.KEY_SAMPLE_RATE);
        for (int i = 0; i < second.units.size(); i++) {
            change.units.add(second.units.get(i));
            change.times.add(after + second.times.get(i));
        }
        change.leaveOut = 1152;  // pvmp3 decodes on through the change, which a new decoder cannot
        decode("mp3 rate change", MediaCodec.createDecoderByType(MP3), change, 1, -1, -1);
        if (mp3 != null) decode("mp3 by name", MediaCodec.createByCodecName(mp3), mp3(tone), 1, -1, -1);
        add("flac track: " + keys(flac(lossless, true).format) + ", " + flac(lossless, true).units.size() + " units");
        decode("flac", MediaCodec.createDecoderByType(FLAC), flac(lossless, true), 1, -1, -1);
        Stream floats = flac(lossless, true);
        floats.format.setInteger(MediaFormat.KEY_PCM_ENCODING, PCM_FLOAT);
        decode("flac float", MediaCodec.createDecoderByType(FLAC), floats, 1, -1, -1);
        decode("flac seek", MediaCodec.createDecoderByType(FLAC), flac(lossless, true), 1, 2, -1);
        decode("flac config unit", MediaCodec.createDecoderByType(FLAC), flac(lossless, false), 1, -1,
                MediaCodec.BUFFER_FLAG_CODEC_CONFIG);
        decode("flac header as a unit", MediaCodec.createDecoderByType(FLAC), flac(lossless, false), 1, -1, 0);
        Stream configOnly = flac(lossless, false);
        configOnly.units.clear();
        decode("flac config with the end", MediaCodec.createDecoderByType(FLAC), configOnly, 1, -1,
                MediaCodec.BUFFER_FLAG_CODEC_CONFIG | MediaCodec.BUFFER_FLAG_END_OF_STREAM);
        Stream corrupt = flac(lossless, true);
        byte[] frame = corrupt.units.get(2);
        for (int i = 16; i < frame.length - 2; i++) frame[i] ^= 0x55;  // the header kept, the samples not
        decode("flac corrupt frame", MediaCodec.createDecoderByType(FLAC), corrupt, 1, -1, -1);
        Stream recovered = flac(lossless, true);
        frame = recovered.units.get(1);
        for (int i = 16; i < frame.length - 2; i++) frame[i] ^= 0x55;
        decode("flac corrupt then flush", MediaCodec.createDecoderByType(FLAC), recovered, 1, 2, -1);
        if (flac != null) {
            decode("flac by name", MediaCodec.createByCodecName(flac), flac(lossless, true), 1, -1, -1);
        }
        decode("raw", MediaCodec.createDecoderByType(RAW), raw(PCM_16), 1, -1, -1);
        decode("raw float", MediaCodec.createDecoderByType(RAW), raw(PCM_FLOAT), 1, -1, -1);
        Stream trimmed = raw(PCM_16);
        trimmed.format.setInteger(MediaFormat.KEY_ENCODER_DELAY, 100);
        trimmed.format.setInteger(MediaFormat.KEY_ENCODER_PADDING, 50);
        decode("raw trimmed", MediaCodec.createDecoderByType(RAW), trimmed, 1, -1, -1);
        MediaFormat rateless = new MediaFormat();
        rateless.setString(MediaFormat.KEY_MIME, RAW);
        rateless.setInteger(MediaFormat.KEY_CHANNEL_COUNT, 2);
        Stream noRate = new Stream(rateless);
        noRate.units.addAll(raw(PCM_16).units);
        noRate.times.addAll(raw(PCM_16).times);
        decode("raw without a rate", MediaCodec.createDecoderByType(RAW), noRate, 1, -1, -1);
        if (raw != null) decode("raw by name", MediaCodec.createByCodecName(raw), raw(PCM_16), 1, -1, -1);
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
     *  the first one's frame; and the CRC32 of all of them. */
    private static final class Samples {
        int channels;
        int rate;
        int encoding = PCM_16;
        long frames, measured;
        long leaveOut;  // the frames after a change of rate the measures leave out: there decoders differ
        long left;
        double sum;
        int crossings;
        long firstCrossing = -1;
        boolean below;  // below -1/32 of full scale since the last crossing
        final CRC32 crc = new CRC32();

        void shape(MediaFormat format) {
            if (format.containsKey(MediaFormat.KEY_CHANNEL_COUNT)) {
                channels = format.getInteger(MediaFormat.KEY_CHANNEL_COUNT);
            }
            if (format.containsKey(MediaFormat.KEY_SAMPLE_RATE)) {
                int now = format.getInteger(MediaFormat.KEY_SAMPLE_RATE);
                if (rate != 0 && now != rate) left = leaveOut;
                rate = now;
            }
            encoding = format.containsKey(MediaFormat.KEY_PCM_ENCODING)
                    ? format.getInteger(MediaFormat.KEY_PCM_ENCODING) : PCM_16;
        }

        int frameSize() {
            return channels * (encoding == PCM_16 ? 2 : 4);
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
                double value = encoding == PCM_16 ? samples.getShort(at) / 32768.0 : samples.getFloat(at);
                frames++;
                if (left > 0) {
                    left--;
                    below = false;
                    continue;
                }
                measured++;
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
            long rms = measured == 0 ? 0 : Math.round(100 * Math.sqrt(sum / measured));
            return frames + " frames, rms " + rms + " %, " + crossings + " rising zero crossings, the first at "
                    + firstCrossing + (leaveOut > 0 ? ", " + leaveOut + " after the change of rate left out" : "");
        }
    }

    /**
     * Decodes a stream synchronously, `group` units in each input buffer; from unit `resume` on after two units and a
     * flush where it is not -1; FLAC's metadata as a unit first, with `headerFlags`, where they are not -1. A codec
     * error is logged by its exception's class.
     */
    private void decode(String name, MediaCodec codec, Stream stream, int group, int resume, int headerFlags)
            throws Exception {
        try {
            codec.configure(stream.format, null, null, 0);
        } catch (IllegalArgumentException e) {
            add(name + ": configure " + e.getClass().getName());
            codec.release();
            return;
        }
        codec.start();
        Samples samples = new Samples();
        samples.leaveOut = stream.leaveOut;
        samples.shape(stream.format);
        int next = 0;
        if (headerFlags >= 0) {
            int index = codec.dequeueInputBuffer(1_000_000);
            codec.getInputBuffer(index).put(stream.header);
            codec.queueInputBuffer(index, 0, stream.header.length, 0, headerFlags);
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
        int capacity = -1;
        boolean inputDone = headerFlags >= 0 && (headerFlags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
        int outputs = 0, empty = 0;
        long firstTime = 0, lastTime = 0, firstFrames = -1, lastFrames = 0;
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        long deadline = System.currentTimeMillis() + 20_000;
        try {
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
                            for (int end = Math.min(next + group, stream.units.size()); next < end; next++) {
                                buffer.put(stream.units.get(next));
                            }
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
        }
        add(name + ": input capacity " + capacity);
        add(name + ": " + outputs + " output buffers and " + empty + " empty ones, times " + firstTime + " to "
                + lastTime + ", the first " + firstFrames + " frames, the last " + lastFrames);
        add(name + ": " + samples.describe());
        String crc = name + ": samples' CRC32 " + Long.toHexString(samples.crc.getValue());
        add(stream.format.getString(MediaFormat.KEY_MIME).equals(MP3) ? "device " + crc : crc);
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
        samples.leaveOut = stream.leaveOut;
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
