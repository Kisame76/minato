package com.android.minato.notifications;

import java.nio.charset.StandardCharsets;

/**
 * APKRT_REQUEST_NOTIFICATION's text (ADR-0074 decision 2), which the runtime checks whole before the host shows it:
 * "post\t<id>\t<tag>\t<channel>\t<importance>\t<sound>\t<when>\t<group>\t<title>\t<subText>\t<intent>\t<text>",
 * "cancel\t<id>\t<tag>" and "cancelAll". The app's strings are made fit here, so that a post is never refused: a tab is
 * a space, a new line is one only in the text and a space elsewhere, the C0 and C1 controls, the bidirectional
 * controls and the invisible characters go, and each field is cut to its bytes of UTF-8 at a character boundary (tag,
 * channel and group 64, title and sub text 256, text 4096). The importance is NotificationManager's, 0 to 5. Plain
 * Java, so tools/framework-host-check.sh runs it on the host's JVM.
 */
public final class NotificationText {
    static final int NAME_BYTES = 64;
    static final int LINE_BYTES = 256;
    static final int TEXT_BYTES = 4096;
    static final int INTENT_BYTES = 2048;

    private NotificationText() {
    }

    public static String post(int id, String tag, String channel, int importance, boolean sound, long when,
            String group, CharSequence title, CharSequence subText, String intent, CharSequence text) {
        return "post\t" + id + "\t" + field(tag(tag), NAME_BYTES, false) + "\t" + field(channel, NAME_BYTES, false)
                + "\t"
                + Math.max(0, Math.min(5, importance)) + "\t" + (sound ? 1 : 0) + "\t" + Math.max(0, when) + "\t"
                + field(group, NAME_BYTES, false) + "\t" + field(title, LINE_BYTES, false) + "\t"
                + field(subText, LINE_BYTES, false) + "\t" + intent(intent) + "\t" + field(text, TEXT_BYTES, true);
    }

    public static String cancel(int id, String tag) {
        return "cancel\t" + id + "\t" + field(tag(tag), NAME_BYTES, false);
    }

    /**
     * A tag as Android's notification key writes it (StatusBarNotification's "|" + tag + "|"): none is "null", so
     * the host keeps it apart from the empty tag, as NotificationManagerService does, and the tag "null" is the same.
     */
    static String tag(String tag) {
        return String.valueOf(tag);
    }

    public static String cancelAll() {
        return "cancelAll";
    }

    /** A content intent's URI as it is, when it is printable ASCII within INTENT_BYTES; else none, and a cold tap opens
     * the app at its launcher. */
    static String intent(String uri) {
        if (uri == null || uri.length() > INTENT_BYTES) {
            return "";
        }
        for (int i = 0; i < uri.length(); i++) {
            if (uri.charAt(i) < 0x21 || uri.charAt(i) > 0x7e) {
                return "";
            }
        }
        return uri;
    }

    /** The black flag, which tag characters and their cancel tag make a subdivision's flag (an emoji tag sequence). */
    private static final int BLACK_FLAG = 0x1f3f4;
    private static final int CANCEL_TAG = 0xe007f;

    /**
     * `value` fit for a field of `bytes` bytes; `lines` keeps its new lines. A subdivision's flag is one piece: it is
     * kept or cut whole, and tag characters outside one go.
     */
    static String field(CharSequence value, int bytes, boolean lines) {
        if (value == null) {
            return "";
        }
        StringBuilder out = new StringBuilder();
        int used = 0;
        for (int i = 0; i < value.length(); ) {
            int c = Character.codePointAt(value, i);
            int next = i + Character.charCount(c);
            String piece;
            if (c == '\t' || (c == '\n' && !lines)) {
                piece = " ";
            } else if (c == BLACK_FLAG) {
                next = tagSequenceEnd(value, next);
                piece = value.subSequence(i, next).toString();
            } else if (c != '\n' && dropped(c)) {
                i = next;
                continue;
            } else {
                piece = new String(Character.toChars(c));
            }
            int size = piece.getBytes(StandardCharsets.UTF_8).length;
            if (used + size > bytes) {
                break;
            }
            out.append(piece);
            used += size;
            i = next;
        }
        return out.toString();
    }

    /** Where the tags from `from` end, their cancel tag included; `from` when no tag and its cancel tag follow. */
    private static int tagSequenceEnd(CharSequence value, int from) {
        int at = from;
        while (at < value.length()) {
            int c = Character.codePointAt(value, at);
            if (c == CANCEL_TAG) {
                return at > from ? at + Character.charCount(c) : from;
            }
            if (c < 0xe0020 || c > 0xe007e) {
                return from;
            }
            at += Character.charCount(c);
        }
        return from;
    }

    /**
     * A control, a bidirectional control (U+061C too), an invisible character (U+034F too) or a tag character, as the
     * runtime's check of a title refuses the first three.
     */
    private static boolean dropped(int c) {
        return c < 0x20 || (c >= 0x7f && c <= 0x9f) || (c >= 0x200b && c <= 0x200f) || (c >= 0x2028 && c <= 0x202e)
                || (c >= 0x2060 && c <= 0x206f) || c == 0xfeff || (c >= 0xfff9 && c <= 0xfffb)
                || (c >= 0xd800 && c <= 0xdfff) || c == 0x061c || c == 0x034f || (c >= 0xe0000 && c <= 0xe007f);
    }
}
