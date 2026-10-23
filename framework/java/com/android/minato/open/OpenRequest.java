package com.android.minato.open;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * The text of the request the host gets when an app leaves for iOS (APKRT_REQUEST_OPEN, ADR-0062): "<kind>\t<field>..."
 * with every field percent-encoded UTF-8, only RFC 3986's unreserved bytes raw. Kind 1 VIEW has one field, an absolute
 * URL of scheme http, https, tel or maps; 2 MAIL one, a mailto: URL (RFC 6068); 3 SMS one, an sms: URL; 4 SHARE
 * 3 + 2n: the subject, the text, n, n names of the files the guest put into the share folder, n MIME types. The app
 * chooses every value and the host parses the text, so each is brought inside the limits that the runtime enforces
 * (guest_interface.h): what is outside them is cut or left out, never sent, and a value that cannot be sent at all is
 * answered with null. Plain Java, for tools/framework-host-check.sh.
 */
public final class OpenRequest {
    public static final int VIEW = 1;
    public static final int MAIL = 2;
    public static final int SMS = 3;
    public static final int SHARE = 4;

    /** The limits of APKRT_REQUEST_OPEN's text, in bytes decoded unless said otherwise. */
    public static final int URL_BYTES = 4096;
    public static final int SUBJECT_BYTES = 1000;
    public static final int TEXT_BYTES = 65536;
    public static final int NAME_BYTES = 255;
    public static final int MIME_BYTES = 100;
    public static final int SHARE_FILES = 10;
    /** The most addresses the guest sends in one list of a mail. */
    public static final int ADDRESSES = 50;
    /** The whole text, encoded. */
    public static final int TOTAL_BYTES = 204800;

    private OpenRequest() {
    }

    /** `value` as UTF-8 with every byte but the unreserved ones (letters, digits, "-._~") as %XX. */
    public static String encode(String value) {
        return encode(value, "");
    }

    /** As encode, but the characters of `keep` (ASCII) stay as they are. */
    static String encode(String value, String keep) {
        final byte[] bytes = value.getBytes(StandardCharsets.UTF_8);
        final StringBuilder out = new StringBuilder(bytes.length);
        for (byte b : bytes) {
            final int c = b & 0xff;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.'
                    || c == '_' || c == '~' || (c < 0x80 && keep.indexOf(c) >= 0)) {
                out.append((char) c);
            } else {
                out.append('%').append("0123456789ABCDEF".charAt(c >> 4)).append("0123456789ABCDEF".charAt(c & 15));
            }
        }
        return out.toString();
    }

    /** The longest start of `value` of at most `bytes` bytes as UTF-8, cut between characters. */
    public static String truncate(String whole, int bytes) {
        // A character is at least one byte: nothing past `bytes` characters can stay, so a huge value is cut first.
        final String value = whole.length() > bytes ? whole.substring(0, bytes) : whole;
        if (value.getBytes(StandardCharsets.UTF_8).length <= bytes) {
            return value;
        }
        int used = 0;
        int end = 0;
        while (end < value.length()) {
            final int point = value.codePointAt(end);
            final int size = point < 0x80 ? 1 : point < 0x800 ? 2 : point < 0x10000 ? 3 : 4;
            if (used + size > bytes) {
                break;
            }
            used += size;
            end += Character.charCount(point);
        }
        return value.substring(0, end);
    }

    /** Whether `text` holds a control character, which the runtime refuses in a URL. */
    static boolean hasControl(String text) {
        for (int at = 0; at < text.length(); ) {
            final int point = text.codePointAt(at);
            if (Character.getType(point) == Character.CONTROL || point == 0x2028 || point == 0x2029) {
                return true;
            }
            at += Character.charCount(point);
        }
        return false;
    }

    /**
     * Kind 1 for an absolute URL of scheme http, https, tel or maps; null for any other, one that is too long, one with
     * a control character, an http(s) URL without a host, or a tel URL with anything but a number's characters.
     */
    public static String view(String url) {
        if (url == null) {
            return null;
        }
        final String checked = viewUrl(url);
        return checked == null ? null : VIEW + "\t" + encode(checked);
    }

    /**
     * `url` with every space, separator and format character (U+0020, Zs, Cf) written as %XX of its UTF-8, as a browser
     * does with a space: the runtime refuses them raw in a URL.
     */
    static String escapeSpaces(String url) {
        final StringBuilder out = new StringBuilder(url.length());
        for (int at = 0; at < url.length(); ) {
            final int point = url.codePointAt(at);
            at += Character.charCount(point);
            final int type = Character.getType(point);
            if (point == ' ' || type == Character.SPACE_SEPARATOR || type == Character.FORMAT) {
                out.append(encode(new String(Character.toChars(point))));
            } else {
                out.appendCodePoint(point);
            }
        }
        return out.toString();
    }

    /** `url` when it may be sent as a VIEW, else null; its spaces are escaped, a tel number's taken out. */
    static String viewUrl(String given) {
        // As a browser reads a link: the controls and spaces around it go, and a tab or a line break inside it.
        final String stripped = given.strip().replaceAll("[\\t\\r\\n]", "");
        if (stripped.isEmpty() || hasControl(stripped)) {
            return null;
        }
        final String trimmed = stripped;
        final int telColon = trimmed.indexOf(':');
        final String url = telColon > 0 && trimmed.substring(0, telColon).equalsIgnoreCase("tel")
                ? trimmed.replace(" ", "") : escapeSpaces(trimmed);
        if (url.getBytes(StandardCharsets.UTF_8).length > URL_BYTES) {
            return null;
        }
        final int colon = url.indexOf(':');
        if (colon <= 0) {
            return null;
        }
        final String scheme = url.substring(0, colon).toLowerCase(java.util.Locale.ROOT);
        final String rest = url.substring(colon + 1);
        switch (scheme) {
            case "http":
            case "https": {
                if (!rest.startsWith("//")) {
                    return null;
                }
                int end = rest.length();
                for (int at = 2; at < rest.length(); at++) {
                    final char c = rest.charAt(at);
                    if (c == '/' || c == '?' || c == '#') {
                        end = at;
                        break;
                    }
                }
                final String authority = rest.substring(2, end);
                final String host = authority.substring(authority.lastIndexOf('@') + 1);
                return host.isEmpty() || host.startsWith(":") ? null : url;
            }
            case "tel":
                return rest.length() >= 1 && rest.length() <= 64 && rest.matches("[0-9+*#,;.()\\-]+") ? url : null;
            case "maps":
                return url;
            default:
                return null;
        }
    }

    /**
     * Kind 2: a mailto: URL of the addresses and the query cc, bcc, subject, body; null for an empty one or when the
     * URL cannot be brought inside URL_BYTES (the body is cut first, then the subject).
     */
    public static String mail(String[] to, String[] cc, String[] bcc, String subject, String body) {
        String cutBody = body == null ? "" : body;
        String cutSubject = subject == null ? "" : subject;
        for (int attempt = 0; attempt < 64; attempt++) {
            final String url = mailUrl(to, cc, bcc, cutSubject, cutBody);
            if (url.getBytes(StandardCharsets.UTF_8).length <= URL_BYTES) {
                return MAIL + "\t" + encode(url);
            }
            if (!cutBody.isEmpty()) {
                cutBody = truncate(cutBody, cutBody.getBytes(StandardCharsets.UTF_8).length / 2);
            } else if (!cutSubject.isEmpty()) {
                cutSubject = truncate(cutSubject, cutSubject.getBytes(StandardCharsets.UTF_8).length / 2);
            } else {
                return null;
            }
        }
        return null;
    }

    /** The addresses as one comma-separated, encoded list: at most ADDRESSES of them, none empty. */
    private static String list(String[] addresses) {
        final List<String> parts = new ArrayList<>();
        if (addresses != null) {
            for (String address : addresses) {
                if (parts.size() == ADDRESSES) {
                    break;
                }
                if (address != null && !address.trim().isEmpty()) {
                    parts.add(encode(truncate(address.trim(), 500), "@+"));
                }
            }
        }
        return String.join(",", parts);
    }

    private static String mailUrl(String[] to, String[] cc, String[] bcc, String subject, String body) {
        final StringBuilder url = new StringBuilder("mailto:").append(list(to));
        char separator = '?';
        final String[] names = {"cc", "bcc", "subject", "body"};
        final String[] values = {list(cc), list(bcc), encode(subject), encode(body)};
        for (int at = 0; at < names.length; at++) {
            if (!values[at].isEmpty()) {
                url.append(separator).append(names[at]).append('=').append(values[at]);
                separator = '&';
            }
        }
        return url.toString();
    }

    /**
     * Kind 3: "sms:<number>&body=<body>" (what iOS takes); the number keeps its digits and + * # , ; only. Null for a
     * number with another character once the common separators are taken out.
     */
    public static String sms(String number, String body) {
        final StringBuilder digits = new StringBuilder();
        for (char c : (number == null ? "" : number).toCharArray()) {
            if (c == ' ' || c == '-' || c == '.' || c == '(' || c == ')') {
                continue;
            }
            if (!((c >= '0' && c <= '9') || c == '+' || c == '*' || c == '#' || c == ',' || c == ';')) {
                return null;
            }
            digits.append(c);
        }
        if (digits.length() > 64) {
            return null;
        }
        String cut = body == null ? "" : body;
        for (int attempt = 0; attempt < 64; attempt++) {
            final String url = "sms:" + encode(digits.toString(), "+") + (cut.isEmpty() ? "" : "&body=" + encode(cut));
            if (url.getBytes(StandardCharsets.UTF_8).length <= URL_BYTES) {
                return SMS + "\t" + encode(url);
            }
            cut = truncate(cut, cut.getBytes(StandardCharsets.UTF_8).length / 2);
        }
        return null;
    }

    /**
     * Kind 4. `names` are the files in the share folder, `types` their MIME types (the same length; a type that is
     * none becomes application/octet-stream). A name that is not a plain name, a repeated one and every name beyond
     * SHARE_FILES are left out; the subject and the text are cut to their limits, the text further if the whole would
     * pass TOTAL_BYTES.
     */
    public static String share(String subject, String text, List<String> names, List<String> types) {
        final List<String> keptNames = new ArrayList<>();
        final List<String> keptTypes = new ArrayList<>();
        for (int at = 0; names != null && at < names.size() && keptNames.size() < SHARE_FILES; at++) {
            final String name = names.get(at);
            if (name == null || !plainName(name) || keptNames.contains(name)) {
                continue;
            }
            final String type = types != null && at < types.size() ? types.get(at) : null;
            keptNames.add(name);
            keptTypes.add(type != null && mimeType(type) ? type : "application/octet-stream");
        }
        final StringBuilder tail = new StringBuilder("\t").append(keptNames.size());
        for (String name : keptNames) {
            tail.append('\t').append(encode(name));
        }
        for (String type : keptTypes) {
            tail.append('\t').append(encode(type));
        }
        final String head = SHARE + "\t" + encode(truncate(noNul(subject), SUBJECT_BYTES)) + "\t";
        String cut = truncate(noNul(text), TEXT_BYTES);
        while (head.length() + encode(cut).length() + tail.length() > TOTAL_BYTES) {
            cut = truncate(cut, cut.getBytes(StandardCharsets.UTF_8).length * 3 / 4);
        }
        return head + encode(cut) + tail;
    }

    /** `value` without NUL, which the runtime refuses in a field, or "" for null. */
    private static String noNul(String value) {
        return value == null ? "" : value.replace("\u0000", "");
    }

    /** A plain file name: 1 to NAME_BYTES bytes, no "/", no control, separator or formatting character, not "." or "..". */
    public static boolean plainName(String name) {
        if (name.isEmpty() || name.equals(".") || name.equals("..")
                || name.getBytes(StandardCharsets.UTF_8).length > NAME_BYTES) {
            return false;
        }
        for (int at = 0; at < name.length(); ) {
            final int point = name.codePointAt(at);
            if (!plainPoint(point)) {
                return false;
            }
            at += Character.charCount(point);
        }
        return true;
    }

    /** Whether a name may have `point`: no slash, NUL, control, format, line or paragraph separator, space separator
     *  other than ' ', unassigned point or lone surrogate. */
    public static boolean plainPoint(int point) {
        final int type = Character.getType(point);
        return !(point == '/' || point == 0 || type == Character.CONTROL || type == Character.FORMAT
                || type == Character.LINE_SEPARATOR || type == Character.PARAGRAPH_SEPARATOR
                || type == Character.SPACE_SEPARATOR && point != ' ' || type == Character.UNASSIGNED
                || type == Character.SURROGATE);
    }

    /** Whether `type` is "<type>/<subtype>" of RFC 6838's characters, 1 to MIME_BYTES bytes. */
    public static boolean mimeType(String type) {
        if (type.length() < 3 || type.length() > MIME_BYTES) {
            return false;
        }
        final int slash = type.indexOf('/');
        if (slash <= 0 || slash == type.length() - 1 || type.indexOf('/', slash + 1) >= 0) {
            return false;
        }
        for (int at = 0; at < type.length(); at++) {
            final char c = type.charAt(at);
            final boolean token = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                    || "!#$&^_.+-".indexOf(c) >= 0;
            if (at != slash && !token) {
                return false;
            }
        }
        return true;
    }
}
