package com.android.minato.picker;

import java.nio.charset.StandardCharsets;

/**
 * The text of the request the host gets for a picker intent (APKRT_REQUEST_PICK, ADR-0057): "<kind> <flags> <max>
 * <MIME types> <title>", the fields separated by tabs and the MIME types by newlines. Flags: 1 for several picks
 * (EXTRA_ALLOW_MULTIPLE); max is EXTRA_PICK_IMAGES_MAX for the photo picker, else 0; the title is a save's. The
 * app chooses the types, the title and the maximum, and the host parses the text, so every field is brought inside
 * the limits that the runtime enforces (guest_interface.h): what is outside them is cut or left out, never sent.
 * Plain Java, for tools/framework-host-check.sh.
 */
public final class PickerRequest {
    /** The limits of APKRT_REQUEST_PICK's text. */
    public static final int TYPES_LIMIT = 8;
    public static final int TYPE_LENGTH = 100;
    public static final int TITLE_BYTES = 200;
    public static final int MAX_LIMIT = 1000;

    private PickerRequest() {
    }

    /**
     * @param kind       1 to 5, PickerIntents' kinds
     * @param type       the intent's type, or null
     * @param extraTypes EXTRA_MIME_TYPES, or null
     * @param multiple   EXTRA_ALLOW_MULTIPLE
     * @param max        EXTRA_PICK_IMAGES_MAX for the photo picker, else 0
     * @param title      EXTRA_TITLE, or null
     */
    public static String text(int kind, String type, String[] extraTypes, boolean multiple, int max, String title) {
        // Each field goes only where the kind uses it, as the runtime demands: several picks for the opens and the
        // photo picker, a maximum for the photo picker, types for all but a folder, a title for a save.
        final boolean severalAllowed = kind == 1 || kind == 2 || kind == 5;
        final StringBuilder types = new StringBuilder();
        int count = 0;
        if (kind != 4) {
            if (type != null && mimeType(type)) {
                types.append(type);
                count++;
            }
            if (extraTypes != null) {
                for (String extra : extraTypes) {
                    if (count == TYPES_LIMIT) {
                        break;
                    }
                    if (extra != null && mimeType(extra)) {
                        if (count > 0) {
                            types.append('\n');
                        }
                        types.append(extra);
                        count++;
                    }
                }
            }
        }
        final int bounded = kind == 5 ? Math.max(0, Math.min(MAX_LIMIT, max)) : 0;
        return kind + "\t" + (multiple && severalAllowed ? 1 : 0) + "\t" + bounded + "\t" + types + "\t"
                + (kind == 3 ? plainTitle(title) : "");
    }

    /**
     * Whether `type` is a MIME type or a pattern such as image/*: two tokens around one '/', each RFC 6838's
     * characters or "*" alone (so also the pattern of every type), as the runtime takes them.
     */
    static boolean mimeType(String type) {
        if (type.length() < 3 || type.length() > TYPE_LENGTH) {
            return false;
        }
        final int slash = type.indexOf('/');
        return slash > 0 && mimeToken(type.substring(0, slash)) && mimeToken(type.substring(slash + 1));
    }

    private static boolean mimeToken(String token) {
        if (token.isEmpty()) {
            return false;
        }
        if (token.equals("*")) {
            return true;
        }
        for (int at = 0; at < token.length(); at++) {
            final char c = token.charAt(at);
            final boolean allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                    || "!#$&^_.+-".indexOf(c) >= 0;
            if (!allowed) {
                return false;
            }
        }
        return true;
    }

    /**
     * Whether a title may hold the code point; the runtime's plain_code_point: no control characters (C0, DEL, C1), no
     * line or paragraph separators, no zero-width or bidirectional formatting characters, no byte order mark, no
     * interlinear annotation marks, no surrogates.
     */
    static boolean plainCodePoint(int point) {
        if (point < 0x20 || (point >= 0x7f && point <= 0x9f) || (point >= 0xd800 && point <= 0xdfff)) {
            return false;
        }
        if ((point >= 0x200b && point <= 0x200f) || (point >= 0x2028 && point <= 0x202e)) {
            return false;
        }
        if (point >= 0x2060 && point <= 0x206f) {
            return false;
        }
        return point != 0xfeff && !(point >= 0xfff9 && point <= 0xfffb);
    }

    /** The title with the plain code points only, cut at a code point to TITLE_BYTES bytes of UTF-8. */
    static String plainTitle(String title) {
        if (title == null) {
            return "";
        }
        final StringBuilder plain = new StringBuilder();
        int bytes = 0;
        for (int at = 0; at < title.length();) {
            final int point = title.codePointAt(at);
            at += Character.charCount(point);
            if (!plainCodePoint(point)) {
                continue;
            }
            final int size = new String(Character.toChars(point)).getBytes(StandardCharsets.UTF_8).length;
            if (bytes + size > TITLE_BYTES) {
                break;
            }
            bytes += size;
            plain.appendCodePoint(point);
        }
        return plain.toString();
    }
}
