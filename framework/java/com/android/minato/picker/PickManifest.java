package com.android.minato.picker;

import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * The host's manifest of a pick (ADR-0057 step b; APKRT_EVENT_PICK_RESULT in guest_interface.h): the file
 * .minato/picks/&lt;request ID&gt; below the app's data folder, UTF-8, one name per line, each line ended by a newline:
 * the names of the files the host cloned into the app's Download folder. The host wrote it before it posted the
 * answer, and the guest deletes it after reading. The app can write that folder too, so the framework takes nothing
 * from it but plain file names: 1 to 255 bytes of strict UTF-8 without a slash, a NUL, control, separator or
 * formatting characters, other than "." and "..", each once, and as many as the host announced (1 to 1000). Anything
 * else makes the whole answer a cancel. Plain Java: tools/framework-host-check.sh compiles it without android.*.
 */
public final class PickManifest {
    /** The most names a pick has, as the runtime's PICK request limits the maximum of a photo pick. */
    public static final int MAX_NAMES = 1000;
    /** The longest name, as a file name on Android's file systems. */
    public static final int NAME_BYTES = 255;
    /** The most bytes of a manifest: 1000 longest names with their newlines. */
    public static final int MAX_BYTES = MAX_NAMES * (NAME_BYTES + 1);

    private PickManifest() {
    }

    /** The names in `bytes`, or null when it is not a manifest of exactly `count` plain names. */
    public static List<String> parse(byte[] bytes, int count) {
        if (count < 1 || count > MAX_NAMES || bytes.length == 0 || bytes.length > MAX_BYTES
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
        final List<String> names = new ArrayList<>(count);
        final Set<String> seen = new HashSet<>();
        for (final String line : lines) {
            if (!plainName(line) || !seen.add(line)) {
                return null;
            }
            names.add(line);
        }
        return names;
    }

    /** Whether `name` is a plain file name: see the class's comment. */
    public static boolean plainName(String name) {
        if (name.isEmpty() || name.equals(".") || name.equals("..")
                || name.getBytes(StandardCharsets.UTF_8).length > NAME_BYTES) {
            return false;
        }
        for (int i = 0; i < name.length(); ) {
            final int point = name.codePointAt(i);
            if (point == '/' || !PickerRequest.plainCodePoint(point)) {
                return false;
            }
            i += Character.charCount(point);
        }
        return true;
    }
}
