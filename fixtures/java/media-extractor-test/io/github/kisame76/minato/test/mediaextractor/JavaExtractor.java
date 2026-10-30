package io.github.kisame76.minato.test.mediaextractor;

import android.content.Context;
import android.content.res.AssetFileDescriptor;
import android.media.MediaCodec;
import android.media.MediaDataSource;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.net.Uri;
import android.os.PersistableBundle;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.zip.CRC32;

/**
 * ADR-0060 e2d part 2b's half of media-extractor-test: android.media.MediaExtractor on the same files. Each line
 * starts with "java <name>"; a line that starts with "device " goes to the device tag.
 * - From the APK's asset (an AssetFileDescriptor): each track's format (every key, by type and value, sorted), every
 *   sample of all tracks in the extractor's order (track, time, flags, size and a CRC32 of its bytes, read into a
 *   heap buffer), the first sample once more into a direct buffer at an offset, the answers once the samples are out,
 *   seeks in each mode, the cache's answers, crypto, DRM and audio presentations, the metrics, and the refusals (a
 *   track past the count, a seek mode past the last, buffers too small, a second source, a released extractor).
 * - From a FileDescriptor with an offset and a length, from the copy's path, its file:// form and a file Uri, and from
 *   the bytes through a MediaDataSource: the samples' count and digest, and how often the source was closed; from a
 *   missing path, an unreachable http URL and bytes that are no movie: the exception.
 * - The bytes with a broken avcC (step 2a.2): each track's MIME type or what getTrackFormat and selectTrack throw.
 */
final class JavaExtractor {
    private static final long[] TIMES = {-500000, 0, 200000, 433333, 450000, 966667, 2000000};
    private static final String[] MODES = {"previous", "next", "closest"};

    private final List<String> lines = new ArrayList<>();

    /** The lines for one file, joined by newlines. */
    static String extract(Context context, String name, AssetFileDescriptor asset, File copy, byte[] bytes) {
        JavaExtractor test = new JavaExtractor();
        test.run(context, name, asset, copy, bytes);
        return String.join("\n", test.lines);
    }

    private void add(String line) {
        lines.add(line);
    }

    private void run(Context context, String name, AssetFileDescriptor asset, File copy, byte[] bytes) {
        String prefix = "java " + name;
        long whole = 0;
        MediaExtractor extractor = new MediaExtractor();
        try {
            add(prefix + ": tracks before a source " + extractor.getTrackCount() + ", sample time "
                    + extractor.getSampleTime() + ", track " + extractor.getSampleTrackIndex());
            extractor.setDataSource(asset);
            int tracks = extractor.getTrackCount();
            add(prefix + ": " + tracks + " tracks");
            for (int track = 0; track < tracks; track++) {
                formatLines(prefix + " track " + track, extractor.getTrackFormat(track));
            }
            for (int track = 0; track < tracks; track++) extractor.selectTrack(track);
            int[] count = new int[1];
            whole = walk(prefix, extractor, true, count);
            add(prefix + ": " + count[0] + " samples");
            add(prefix + ": after the samples: advance " + extractor.advance() + ", time "
                    + extractor.getSampleTime() + ", track " + extractor.getSampleTrackIndex() + ", size "
                    + extractor.getSampleSize() + ", flags " + extractor.getSampleFlags() + ", read "
                    + extractor.readSampleData(ByteBuffer.allocate(16), 0));
            for (int mode = 0; mode < MODES.length; mode++) {
                for (long time : TIMES) seekLine(prefix, extractor, time, mode);
            }
            extractor.seekTo(0, MediaExtractor.SEEK_TO_PREVIOUS_SYNC);
            ByteBuffer direct = ByteBuffer.allocateDirect(8192);
            int size = extractor.readSampleData(direct, 3);
            add(prefix + ": first sample into a direct buffer at 3: " + size + ", position " + direct.position()
                    + ", limit " + direct.limit() + ", crc " + crc(direct));
            add(prefix + ": cached duration " + extractor.getCachedDuration() + ", cache at its end "
                    + extractor.hasCacheReachedEndOfStream() + ", crypto info "
                    + extractor.getSampleCryptoInfo(new MediaCodec.CryptoInfo()) + ", DRM "
                    + extractor.getDrmInitData() + ", pssh " + extractor.getPsshInfo() + ", presentations "
                    + extractor.getAudioPresentations(0).size() + ", CAS " + extractor.getCasInfo(0));
            metricsLines(prefix, extractor.getMetrics());
            add(prefix + ": track format " + tracks + ": " + thrown(() -> extractor.getTrackFormat(tracks)));
            add(prefix + ": select " + tracks + ": " + fails(() -> extractor.selectTrack(tracks)));
            add(prefix + ": unselect " + tracks + ": " + fails(() -> extractor.unselectTrack(tracks)));
            add(prefix + ": seek mode 3: " + fails(() -> extractor.seekTo(0, 3)));
            add(prefix + ": seek mode -1: " + fails(() -> extractor.seekTo(0, -1)));
            add(prefix + ": read into 1 byte: " + thrown(() -> extractor.readSampleData(ByteBuffer.allocate(1), 0)));
            add(prefix + ": read at 20 of 16 bytes: "
                    + thrown(() -> extractor.readSampleData(ByteBuffer.allocate(16), 20)));
            add(prefix + ": a second source: " + fails(() -> extractor.setDataSource(asset)));
        } catch (Throwable t) {
            add(prefix + ": failed: " + t);
        }
        extractor.release();
        add(prefix + ": after release: " + thrown(extractor::getTrackCount));

        try (FileInputStream in = new FileInputStream(copy)) {
            sourceLine(prefix + " fd", whole, e -> e.setDataSource(in.getFD(), 0, copy.length()));
        } catch (IOException e) {
            add(prefix + " fd: " + e);
        }
        sourceLine(prefix + " path", whole, e -> e.setDataSource(copy.getPath()));
        sourceLine(prefix + " file URL", whole, e -> e.setDataSource("file://" + copy.getPath()));
        sourceLine(prefix + " file Uri", whole, e -> e.setDataSource(context, Uri.fromFile(copy), null));
        sourceLine(prefix + " missing path", whole, e -> e.setDataSource(copy.getPath() + ".missing"));
        sourceLine(prefix + " http", whole, e -> e.setDataSource("http://127.0.0.1:1/" + name + ".mp4"));
        Bytes source = new Bytes(bytes);
        sourceLine(prefix + " data source", whole, e -> e.setDataSource(source));
        add(prefix + " data source: closed " + source.closed + " times");
        byte[] text = new byte[1000];
        java.util.Arrays.fill(text, (byte) 'x');
        Bytes other = new Bytes(text);
        sourceLine(prefix + " data source no movie", whole, e -> e.setDataSource(other));
        add(prefix + " data source no movie: closed " + other.closed + " times");
        brokenLines(prefix, bytes);
        csdLines(prefix, bytes);
    }

    /**
     * The bytes with the first avcC broken (step 2a.2): of version 0, and with its first SPS's length past the box.
     * Each track's MIME type or what getTrackFormat throws, what selectTrack throws, and the samples' count.
     */
    private void brokenLines(String prefix, byte[] bytes) {
        int at = -1;
        for (int index = 4; index + 12 <= bytes.length && at < 0; index++) {
            if (bytes[index] == 'a' && bytes[index + 1] == 'v' && bytes[index + 2] == 'c' && bytes[index + 3] == 'C') {
                at = index + 4;  // the body
            }
        }
        if (at < 0) {
            add(prefix + " broken: no avcC");
            return;
        }
        byte[] version = bytes.clone();
        version[at] = 0;
        byte[] past = bytes.clone();
        past[at + 6] = (byte) 0xff;
        brokenLine(prefix + " avcC of version 0", version);
        brokenLine(prefix + " avcC of an SPS past its end", past);
    }

    private void brokenLine(String what, byte[] bytes) {
        MediaExtractor extractor = new MediaExtractor();
        try {
            extractor.setDataSource(new Bytes(bytes));
            int tracks = extractor.getTrackCount();
            add(what + ": " + tracks + " tracks");
            for (int track = 0; track < tracks; track++) {
                int index = track;
                add(what + " track " + track + ": " + thrown(() -> extractor.getTrackFormat(index)
                        .getString(MediaFormat.KEY_MIME)));
                add(what + " select " + track + ": " + fails(() -> extractor.selectTrack(index)));
            }
            int count = 0;
            for (; extractor.getSampleTrackIndex() >= 0; extractor.advance()) count++;
            add(what + ": " + count + " samples");
        } catch (Throwable t) {
            add(what + ": " + t);
        } finally {
            extractor.release();
        }
    }

    /**
     * Files whose sample entries give a track an opaque csd-0 (step 2a.2 item 4), which the JNI library makes from the
     * bytes: each track's format or what getTrackFormat throws, what selectTrack throws, and the samples' count.
     */
    private void csdLines(String prefix, byte[] bytes) {
        Object[] files = MediaExtractorApplication.csdFiles(bytes);
        if (files.length == 0) add(prefix + " csd: no avc1 and mp4a");
        for (int index = 0; index + 1 < files.length; index += 2) {
            String what = prefix + " " + files[index];
            MediaExtractor extractor = new MediaExtractor();
            try {
                extractor.setDataSource(new Bytes((byte[]) files[index + 1]));
                int tracks = extractor.getTrackCount();
                add(what + ": " + tracks + " tracks");
                for (int track = 0; track < tracks; track++) {
                    int selected = track;
                    try {
                        formatLines(what + " track " + track, extractor.getTrackFormat(track));
                    } catch (IllegalArgumentException e) {
                        add(what + " track " + track + ": " + e);
                    }
                    add(what + " select " + track + ": " + fails(() -> extractor.selectTrack(selected)));
                }
                int count = 0;
                for (; extractor.getSampleTrackIndex() >= 0; extractor.advance()) count++;
                add(what + ": " + count + " samples");
            } catch (Throwable t) {
                add(what + ": " + t);
            } finally {
                extractor.release();
            }
        }
    }

    /** Every key of the format, sorted, one line each. */
    private void formatLines(String what, MediaFormat format) {
        List<String> keys = new ArrayList<>(format.getKeys());
        Collections.sort(keys);
        if (keys.isEmpty()) add(what + ": no keys");
        for (String key : keys) {
            String value;
            switch (format.getValueTypeForKey(key)) {
                case MediaFormat.TYPE_INTEGER: value = "int " + format.getInteger(key); break;
                case MediaFormat.TYPE_LONG: value = "long " + format.getLong(key); break;
                case MediaFormat.TYPE_FLOAT: value = "float " + format.getFloat(key); break;
                case MediaFormat.TYPE_STRING: value = "string " + format.getString(key); break;
                case MediaFormat.TYPE_BYTE_BUFFER: {
                    ByteBuffer buffer = format.getByteBuffer(key);
                    value = "buffer " + buffer.remaining() + " bytes, crc " + crc(buffer);
                    break;
                }
                default: value = "type " + format.getValueTypeForKey(key);
            }
            add(what + " " + key + ": " + value);
        }
    }

    /** Every sample left, in the extractor's order, and their count; their lines' digest. */
    private long walk(String prefix, MediaExtractor extractor, boolean verbose, int[] counted) {
        CRC32 digest = new CRC32();
        ByteBuffer buffer = ByteBuffer.allocate(1 << 16);
        int count = 0;
        do {
            int size = extractor.readSampleData(buffer, 0);
            if (size < 0) break;
            String line = "track " + extractor.getSampleTrackIndex() + ", time " + extractor.getSampleTime()
                    + ", flags " + extractor.getSampleFlags() + ", size " + size + "/" + extractor.getSampleSize()
                    + ", position " + buffer.position() + ", limit " + buffer.limit() + ", crc " + crc(buffer);
            if (verbose) add(prefix + " sample " + count + ": " + line);
            digest.update(line.getBytes());
            count++;
        } while (extractor.advance());
        counted[0] = count;
        return digest.getValue();
    }

    private void seekLine(String prefix, MediaExtractor extractor, long time, int mode) {
        extractor.seekTo(time, mode);
        StringBuilder line = new StringBuilder(prefix + ": seek " + MODES[mode] + " " + time + ":");
        for (int sample = 0; sample < 3; sample++) {
            int track = extractor.getSampleTrackIndex();
            if (track < 0) {
                line.append(" end");
                break;
            }
            line.append(" ").append(track).append("@").append(extractor.getSampleTime());
            if (!extractor.advance()) break;
        }
        add(line.toString());
    }

    private void metricsLines(String prefix, PersistableBundle metrics) {
        if (metrics == null) {
            add(prefix + " metrics: none");
            return;
        }
        List<String> keys = new ArrayList<>(metrics.keySet());
        Collections.sort(keys);
        for (String key : keys) {
            Object value = metrics.get(key);
            // The app's uid is the device's.
            add((key.equals("_uid") ? "device " : "") + prefix + " metrics " + key + ": "
                    + (value == null ? "null" : value.getClass().getSimpleName() + " " + value));
        }
    }

    private interface Setter {
        void set(MediaExtractor extractor) throws IOException;
    }

    /** An extractor on the source: its tracks and samples by digest, or the exception. */
    private void sourceLine(String what, long whole, Setter setter) {
        MediaExtractor extractor = new MediaExtractor();
        try {
            setter.set(extractor);
            int tracks = extractor.getTrackCount();
            for (int track = 0; track < tracks; track++) extractor.selectTrack(track);
            int[] count = new int[1];
            long digest = walk(what, extractor, false, count);
            add(what + ": " + tracks + " tracks, " + count[0] + " samples"
                    + (count[0] == 0 ? "" : digest == whole ? ", the same as from the asset"
                                                            : ", others than from the asset"));
        } catch (Throwable t) {
            add(what + ": " + t);
        } finally {
            extractor.release();
        }
    }

    private interface Call {
        Object call() throws Exception;
    }

    private interface VoidCall {
        void call() throws Exception;
    }

    private static String thrown(Call call) {
        try {
            return "no error, " + call.call();
        } catch (Throwable t) {
            return t.toString();
        }
    }

    private static String fails(VoidCall call) {
        try {
            call.call();
            return "no error";
        } catch (Throwable t) {
            return t.toString();
        }
    }

    /** CRC32 of the buffer's bytes from its position to its limit, which stay. */
    private static String crc(ByteBuffer buffer) {
        CRC32 crc = new CRC32();
        crc.update(buffer.duplicate());
        return String.format("%08x", crc.getValue());
    }

    /** A MediaDataSource of bytes that answers -1 past its end, as the API asks, and counts its closes. */
    private static final class Bytes extends MediaDataSource {
        private final byte[] bytes;
        int closed;

        Bytes(byte[] bytes) {
            this.bytes = bytes;
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
            return bytes.length;
        }

        @Override
        public void close() {
            closed++;
        }
    }
}
