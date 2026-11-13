package io.github.kisame76.minato.test.mediaextractorvorbis;

import android.app.Application;
import android.content.Context;
import android.content.res.AssetFileDescriptor;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaDataSource;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.os.PersistableBundle;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.TreeSet;
import java.util.zip.CRC32;

/**
 * MediaExtractor and MediaCodec on Ogg Vorbis. From a thread that
 * Application.attachBaseContext starts, for media-player-ogg-test's two stereo Ogg Vorbis files:
 * - through the asset's descriptor: the track count, the track's format (every key with its value, its buffers in
 *   hex), the metrics, every sample's time, size and flags (the first and last ones logged, all of them by CRC32),
 *   and the next samples after seeks to five times in each of the three sync modes;
 * - through a MediaDataSource of its size, of an unknown size (no duration, no seeks) and of its first 6000 bytes;
 * - its samples decoded by MediaCodec with the track's format, whole and across a seek and a flush: the output
 *   formats, buffers, times and samples (their count, RMS in steps of 1 % of full scale, rising zero crossings and the
 *   first one's frame, of the first channel, and of more than two channels each one's RMS and crossings; their
 *   CRC32 to the device tag, as decoders differ in the last bits);
 * - and an Ogg page that is neither Vorbis nor Opus.
 * Each wait is bounded; it logs with the tag media-extractor-vorbis-test and ends with "done".
 */
public class ExtractorVorbisApplication extends Application {
    private static final String TAG = "media-extractor-vorbis-test";
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

    private static String hex(ByteBuffer buffer) {
        ByteBuffer bytes = buffer.duplicate();
        StringBuilder out = new StringBuilder();
        while (bytes.hasRemaining()) out.append(String.format("%02x", bytes.get() & 0xff));
        return out.toString();
    }

    /** A format's keys in order, each with its value (buffers by their size and bytes). */
    private static String keys(MediaFormat format) {
        StringBuilder out = new StringBuilder();
        for (String key : new TreeSet<>(format.getKeys())) {
            Object value;
            switch (format.getValueTypeForKey(key)) {
                case MediaFormat.TYPE_INTEGER: value = format.getInteger(key); break;
                case MediaFormat.TYPE_LONG: value = format.getLong(key) + "L"; break;
                case MediaFormat.TYPE_FLOAT: value = format.getFloat(key) + "f"; break;
                case MediaFormat.TYPE_STRING: value = "\"" + format.getString(key) + "\""; break;
                case MediaFormat.TYPE_BYTE_BUFFER:
                    ByteBuffer buffer = format.getByteBuffer(key);
                    // A long buffer by its size and CRC32: liblog cuts a line at 4068 bytes, csd-1 has 3247.
                    if (buffer.remaining() > 64) {
                        byte[] copy = new byte[buffer.remaining()];
                        buffer.duplicate().get(copy);
                        CRC32 crc = new CRC32();
                        crc.update(copy);
                        value = "buffer " + copy.length + " crc " + Long.toHexString(crc.getValue());
                    } else {
                        value = "buffer " + buffer.remaining() + " " + hex(buffer);
                    }
                    break;
                default: value = "?";
            }
            out.append(out.length() == 0 ? "" : ", ").append(key).append('=').append(value);
        }
        return out.toString();
    }

    private static byte[] read(Context context, String name) throws Exception {
        try (InputStream in = context.getAssets().open(name)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] chunk = new byte[8192];
            for (int count; (count = in.read(chunk)) > 0;) out.write(chunk, 0, count);
            return out.toByteArray();
        }
    }

    /** An app's source of bytes in memory, with its size or with -1 (unknown). */
    private static final class MemorySource extends MediaDataSource {
        final byte[] bytes;
        final boolean sized;

        MemorySource(byte[] bytes, boolean sized) {
            this.bytes = bytes;
            this.sized = sized;
        }

        @Override
        public int readAt(long position, byte[] buffer, int offset, int size) {
            if (position >= bytes.length) return -1;
            int count = (int) Math.min(size, bytes.length - position);
            System.arraycopy(bytes, (int) position, buffer, offset, count);
            return count;
        }

        @Override
        public long getSize() {
            return sized ? bytes.length : -1;
        }

        @Override
        public void close() {
        }
    }

    private interface Setter {
        void set(MediaExtractor extractor) throws IOException;
    }

    private void test(Context context) throws Exception {
        for (String name : new String[] {"vorbis.ogg", "vorbis-long.ogg"}) {
            byte[] bytes = read(context, name);
            Setter asset = extractor -> {
                try (AssetFileDescriptor fd = context.getAssets().openFd(name)) {
                    extractor.setDataSource(fd.getFileDescriptor(), fd.getStartOffset(), fd.getLength());
                }
            };
            MediaExtractor extractor = open(name, asset);
            if (extractor != null) {
                metrics(name, extractor.getMetrics());
                samples(name, extractor);
                seeks(name, extractor);
                extractor.release();
            }
            for (int kind = 0; kind < 3; kind++) {
                String label = name + (kind == 0 ? " in memory" : kind == 1 ? " in memory of unknown size"
                        : " cut after 6000 bytes");
                byte[] source = kind == 2 ? Arrays.copyOf(bytes, 6000) : bytes;
                boolean sized = kind != 1;
                MediaExtractor memory = open(label, e -> e.setDataSource(new MemorySource(source, sized)));
                if (memory == null) continue;
                samples(label, memory);
                if (kind == 1) {
                    memory.seekTo(600_000, MediaExtractor.SEEK_TO_CLOSEST_SYNC);
                    add(label + ": after a seek to 600000 the sample at " + memory.getSampleTime() + ", size "
                            + memory.getSampleSize());
                }
                memory.release();
            }
            decode(name, open(name + " to decode", asset), -1);
            decode(name + " across a seek", open(name + " to decode across a seek", asset), 600_000);
            decode(name + " raw packets", open(name + " to decode raw packets", asset), -1, true);
        }
        byte[] page = read(context, "vorbis.ogg");
        page[29] = 'X';  // the first packet no longer \1vorbis
        open("a page of neither Vorbis nor Opus", e -> e.setDataSource(new MemorySource(page, true)));
        codecs();
    }

    /** The decoder for audio/vorbis by name and by type, and what MediaCodecList lists for it. */
    private void codecs() {
        try {
            MediaCodec byType = MediaCodec.createDecoderByType("audio/vorbis");
            add("createDecoderByType(audio/vorbis): " + byType.getName());
            byType.release();
        } catch (Exception e) {
            add("createDecoderByType(audio/vorbis): " + e.getClass().getName());
        }
        try {
            MediaCodec byName = MediaCodec.createByCodecName("c2.android.vorbis.decoder");
            add("createByCodecName(c2.android.vorbis.decoder): " + byName.getName());
            add("  its codec info: " + byName.getCodecInfo().getName() + ", encoder "
                    + byName.getCodecInfo().isEncoder() + ", types "
                    + Arrays.toString(byName.getCodecInfo().getSupportedTypes()));
            byName.release();
        } catch (Exception e) {
            add("createByCodecName(c2.android.vorbis.decoder): " + e.getClass().getName());
        }
        List<String> listed = new ArrayList<>();
        for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.ALL_CODECS).getCodecInfos()) {
            for (String type : info.getSupportedTypes()) {
                if (type.equalsIgnoreCase("audio/vorbis")) {
                    MediaCodecInfo.CodecCapabilities caps = info.getCapabilitiesForType(type);
                    listed.add(info.getName() + " encoder " + info.isEncoder() + " max channels "
                            + caps.getAudioCapabilities().getMaxInputChannelCount() + " rates "
                            + Arrays.toString(caps.getAudioCapabilities().getSupportedSampleRateRanges()));
                }
            }
        }
        Collections.sort(listed);
        add("MediaCodecList audio/vorbis: " + listed.size());
        for (String line : listed) add("  " + line);
        add("findDecoderForFormat(vorbis 44100 2): " + new MediaCodecList(MediaCodecList.REGULAR_CODECS)
                .findDecoderForFormat(MediaFormat.createAudioFormat("audio/vorbis", 44100, 2)));
    }

    /** An extractor on the source with its track selected, its tracks and format logged; null where it fails. */
    private MediaExtractor open(String label, Setter setter) {
        MediaExtractor extractor = new MediaExtractor();
        try {
            setter.set(extractor);
        } catch (Exception e) {
            add(label + ": " + e.getClass().getName() + " " + e.getMessage());
            extractor.release();
            return null;
        }
        add(label + ": " + extractor.getTrackCount() + " tracks");
        if (extractor.getTrackCount() == 0) {
            extractor.release();
            return null;
        }
        add(label + " track 0: " + keys(extractor.getTrackFormat(0)));
        extractor.selectTrack(0);
        return extractor;
    }

    private void metrics(String label, PersistableBundle metrics) {
        if (metrics == null) {
            add(label + " metrics: none");
            return;
        }
        List<String> names = new ArrayList<>(metrics.keySet());
        Collections.sort(names);
        for (String key : names) {
            Object value = metrics.get(key);
            add((key.equals("_uid") ? "device " : "") + label + " metrics " + key + ": "
                    + (value == null ? "null" : value.getClass().getSimpleName() + " " + value));
        }
    }

    /** Every sample from where the extractor is: the first six and the last two, and all of them by CRC32. */
    private void samples(String label, MediaExtractor extractor) {
        ByteBuffer buffer = ByteBuffer.allocate(1 << 16);
        CRC32 data = new CRC32(), times = new CRC32();
        List<String> found = new ArrayList<>();
        long bytes = 0;
        int count = 0;
        for (; count < 10_000; count++) {
            buffer.clear();
            int size = extractor.readSampleData(buffer, 0);
            if (size < 0) break;
            byte[] copy = new byte[size];
            buffer.get(copy, 0, size);
            data.update(copy);
            long time = extractor.getSampleTime();
            times.update(ByteBuffer.allocate(8).putLong(time).array());
            bytes += size;
            found.add("sample " + count + " at " + time + ", " + size + " bytes, flags "
                    + extractor.getSampleFlags() + ", track " + extractor.getSampleTrackIndex());
            extractor.advance();
        }
        for (int i = 0; i < found.size(); i++) {
            if (i < 6 || i >= found.size() - 2) add(label + ": " + found.get(i));
        }
        add(label + ": " + count + " samples, " + bytes + " bytes, their CRC32 " + Long.toHexString(data.getValue())
                + ", their times' " + Long.toHexString(times.getValue()) + "; then the time "
                + extractor.getSampleTime() + ", advance " + extractor.advance());
    }

    /** The next samples after seeks to five times in each sync mode. */
    private void seeks(String label, MediaExtractor extractor) {
        long[] targets = {0, 250_000, 600_000, 1_100_000, 5_000_000};
        int[] modes = {MediaExtractor.SEEK_TO_PREVIOUS_SYNC, MediaExtractor.SEEK_TO_NEXT_SYNC,
            MediaExtractor.SEEK_TO_CLOSEST_SYNC};
        for (long target : targets) {
            for (int mode : modes) {
                extractor.seekTo(target, mode);
                StringBuilder line = new StringBuilder(label + ": seek to " + target + " in mode " + mode + ":");
                for (int i = 0; i < 3; i++) {
                    line.append(' ').append(extractor.getSampleTime()).append('/').append(extractor.getSampleSize());
                    extractor.advance();
                }
                add(line.toString());
            }
        }
    }

    /** What the decoded samples are, of the first channel, and of more than two channels of each one. */
    private static final class Samples {
        int channels = 1;
        long frames;
        double sum;
        int crossings;
        long firstCrossing = -1;
        boolean below;
        final double[] sums = new double[8];
        final int[] crossingsOf = new int[8];
        final boolean[] belowOf = new boolean[8];
        final CRC32 crc = new CRC32();

        void add(ByteBuffer buffer, int offset, int size) {
            ByteBuffer bytes = buffer.duplicate().order(ByteOrder.LITTLE_ENDIAN);
            bytes.position(offset).limit(offset + size);
            byte[] copy = new byte[size];
            bytes.duplicate().get(copy);
            crc.update(copy);
            ByteBuffer samples = bytes.slice().order(ByteOrder.LITTLE_ENDIAN);
            for (int at = 0; at + channels * 2 <= size; at += channels * 2) {
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
     * The extractor's samples decoded synchronously with the track's format; where `seek` is not -1, after ten
     * samples the extractor seeks there (CLOSEST_SYNC) and the codec is flushed.
     */
    private void decode(String label, MediaExtractor extractor, long seek) throws Exception {
        decode(label, extractor, seek, false);
    }

    /** As above; `raw` hands the decoder each packet without the 4 bytes OggExtractor appends to it. */
    private void decode(String label, MediaExtractor extractor, long seek, boolean raw) throws Exception {
        if (extractor == null) return;
        MediaFormat format = extractor.getTrackFormat(0);
        MediaCodec codec = MediaCodec.createDecoderByType(format.getString(MediaFormat.KEY_MIME));
        codec.configure(format, null, null, 0);
        codec.start();
        Samples samples = new Samples();
        samples.channels = format.getInteger(MediaFormat.KEY_CHANNEL_COUNT);
        int queued = 0, outputs = 0;
        long firstTime = -1, lastTime = -1;
        boolean inputDone = false;
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        long deadline = System.currentTimeMillis() + 20_000;
        try {
            while (System.currentTimeMillis() < deadline) {
                if (!inputDone) {
                    int index = codec.dequeueInputBuffer(WAIT_US);
                    if (index >= 0) {
                        ByteBuffer buffer = codec.getInputBuffer(index);
                        int size = extractor.readSampleData(buffer, 0);
                        if (raw && size > 4) size -= 4;
                        if (size < 0) {
                            codec.queueInputBuffer(index, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                            inputDone = true;
                        } else {
                            codec.queueInputBuffer(index, 0, size, extractor.getSampleTime(), 0);
                            extractor.advance();
                            if (++queued == 10 && seek >= 0) {
                                extractor.seekTo(seek, MediaExtractor.SEEK_TO_CLOSEST_SYNC);
                                codec.flush();
                                add(label + ": flushed after 10 samples, on at " + extractor.getSampleTime());
                            }
                        }
                    }
                }
                int index = codec.dequeueOutputBuffer(info, WAIT_US);
                if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    MediaFormat changed = codec.getOutputFormat();
                    add(label + ": output format: " + keys(changed));
                    samples.channels = changed.getInteger(MediaFormat.KEY_CHANNEL_COUNT);
                } else if (index >= 0) {
                    if (info.size > 0) {
                        outputs++;
                        if (firstTime < 0) firstTime = info.presentationTimeUs;
                        lastTime = info.presentationTimeUs;
                        samples.add(codec.getOutputBuffer(index), info.offset, info.size);
                    }
                    codec.releaseOutputBuffer(index, false);
                    if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) break;
                }
            }
        } catch (IllegalStateException e) {
            add(label + ": " + e.getClass().getName());
        }
        add(label + ": " + queued + " samples queued, " + outputs + " output buffers, times " + firstTime + " to "
                + lastTime + ", " + samples.describe());
        add("device " + label + ": samples' CRC32 " + Long.toHexString(samples.crc.getValue()));
        codec.stop();
        codec.release();
        extractor.release();
    }
}
