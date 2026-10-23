package com.android.minato.picker;

import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/**
 * The host's manifest of a photo pick (ADR-0057 step c; APKRT_EVENT_PICK_RESULT in guest_interface.h): the file
 * .minato/picks/&lt;request ID&gt; below the app's data folder, UTF-8, one photo or video a line, each line ended by a
 * newline: the name of the clone in the folder .minato/photos, then the width, the height, the duration in
 * milliseconds, the time taken in milliseconds since 1970 and the orientation in degrees, tab-separated. The host
 * knows these from the file; the guest has no decoders at hand. The app can write the folder too, so the framework
 * takes only plain names of a kind it knows (the extension names the MIME type), numbers of at most 18 digits inside
 * the limits below, each name once and as many lines as the host announced (1 to 1000); anything else makes the whole
 * answer a cancel (parse returns null). Plain Java: tools/framework-host-check.sh compiles it without android.*.
 */
public final class PhotoManifest {
    /** One photo or video of a pick. */
    public static final class Item {
        public final String name;
        public final int width;
        public final int height;
        public final long durationMs;
        public final long takenMs;
        public final int orientation;

        public Item(String name, int width, int height, long durationMs, long takenMs, int orientation) {
            this.name = name;
            this.width = width;
            this.height = height;
            this.durationMs = durationMs;
            this.takenMs = takenMs;
            this.orientation = orientation;
        }

        /** The MIME type of the name's extension. */
        public String mimeType() {
            return mimeTypeOf(name);
        }
    }

    /** The most pixels a side has, a duration of a year, and a time up to the year 5138. */
    static final int MAX_SIDE = 1 << 20;
    static final long MAX_DURATION_MS = 365L * 24 * 3600 * 1000;
    static final long MAX_TAKEN_MS = 100_000_000_000_000L;
    /** The longest line: a name, five numbers and the tabs. */
    public static final int MAX_BYTES = PickManifest.MAX_NAMES * (PickManifest.NAME_BYTES + 5 * 20 + 6);

    private PhotoManifest() {
    }

    /** The MIME type for the extension of `name`, null for one the photo picker does not serve. */
    public static String mimeTypeOf(String name) {
        final int dot = name.lastIndexOf('.');
        if (dot < 0) {
            return null;
        }
        switch (name.substring(dot + 1).toLowerCase(Locale.ROOT)) {
            case "jpg":
            case "jpeg":
                return "image/jpeg";
            case "png":
                return "image/png";
            case "gif":
                return "image/gif";
            case "webp":
                return "image/webp";
            case "bmp":
                return "image/bmp";
            case "mp4":
                return "video/mp4";
            case "webm":
                return "video/webm";
            case "3gp":
                return "video/3gpp";
            default:
                return null;
        }
    }

    /** The items in `bytes`, or null when it is not a manifest of exactly `count` well formed lines. */
    public static List<Item> parse(byte[] bytes, int count) {
        if (count < 1 || count > PickManifest.MAX_NAMES || bytes.length == 0 || bytes.length > MAX_BYTES
                || bytes[bytes.length - 1] != '\n') {
            return null;
        }
        final String text;
        try {
            text = StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes)).toString();
        } catch (CharacterCodingException e) {
            return null;
        }
        final String[] lines = text.substring(0, text.length() - 1).split("\n", -1);
        if (lines.length != count) {
            return null;
        }
        final List<Item> items = new ArrayList<>(count);
        final Set<String> seen = new HashSet<>();
        for (final String line : lines) {
            final Item item = item(line);
            if (item == null || !seen.add(item.name)) {
                return null;
            }
            items.add(item);
        }
        return items;
    }

    private static Item item(String line) {
        final String[] field = line.split("\t", -1);
        if (field.length != 6 || !PickManifest.plainName(field[0]) || mimeTypeOf(field[0]) == null) {
            return null;
        }
        for (int i = 1; i < 6; i++) {
            if (!field[i].matches("[0-9]{1,18}") || (field[i].length() > 1 && field[i].charAt(0) == '0')) {
                return null;
            }
        }
        final long width = Long.parseLong(field[1]);
        final long height = Long.parseLong(field[2]);
        final long duration = Long.parseLong(field[3]);
        final long taken = Long.parseLong(field[4]);
        final long orientation = Long.parseLong(field[5]);
        if (width > MAX_SIDE || height > MAX_SIDE || duration > MAX_DURATION_MS || taken > MAX_TAKEN_MS
                || (orientation != 0 && orientation != 90 && orientation != 180 && orientation != 270)) {
            return null;
        }
        return new Item(field[0], (int) width, (int) height, duration, taken, (int) orientation);
    }
}
