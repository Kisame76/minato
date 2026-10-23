package com.android.minato.picker;

import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;

/**
 * The host's answer to a save or a folder request (ADR-0057 step d; APKRT_EVENT_PICK_RESULT in guest_interface.h):
 * the file .minato/picks/&lt;request ID&gt; below the app's data folder holds one line, the place the user chose as
 * the guest sees it, relative to /storage/emulated/0, "/"-separated: for ACTION_CREATE_DOCUMENT the file the host
 * created there under the title (made unique), for ACTION_OPEN_DOCUMENT_TREE the folder the host made visible. The
 * app can write the folder too, so the framework takes only a path of plain names, at most 16 of them and 1024 bytes of
 * UTF-8, each not "." or "..", ended by one newline; anything else makes the answer a cancel (parse returns null).
 * Plain Java: tools/framework-host-check.sh compiles it without android.*.
 */
public final class PickLocation {
    public static final int MAX_COMPONENTS = 16;
    public static final int MAX_BYTES = 1024;

    private PickLocation() {
    }

    /** The relative path in `bytes`, or null when it is not one line of a path of plain names. */
    public static String parse(byte[] bytes) {
        if (bytes.length < 2 || bytes.length > MAX_BYTES + 1 || bytes[bytes.length - 1] != '\n') {
            return null;
        }
        final String text;
        try {
            text = StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes)).toString();
        } catch (CharacterCodingException e) {
            return null;
        }
        final String path = text.substring(0, text.length() - 1);
        final String[] components = path.split("/", -1);
        if (components.length > MAX_COMPONENTS) {
            return null;
        }
        for (final String component : components) {
            if (!PickManifest.plainName(component)) {
                return null;
            }
        }
        return path;
    }
}
