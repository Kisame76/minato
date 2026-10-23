package com.android.minato.media;

import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/**
 * The rules of MediaStore's inserts (ADR-0057 step e), as Android 15's MediaProvider applies them [measured:
 * mediastore-test on apk-runner-35]: the folders a collection allows and the message of a refusal, the folder an insert
 * without one gets, the extension a MIME type needs in the file's name, the number a taken name gets, the title of a
 * name, and the names of a pending file. Plain Java: tools/framework-host-check.sh compiles it without android.*.
 */
public final class MediaRules {
    private static final List<String> IMAGE_DIRS = List.of("DCIM", "Pictures");
    private static final List<String> VIDEO_DIRS = List.of("DCIM", "Movies", "Pictures");
    private static final List<String> AUDIO_DIRS = List.of("Alarms", "Audiobooks", "Music", "Notifications", "Podcasts",
            "Recordings", "Ringtones");
    /** MIME type, then the extensions that belong to it: the first is the one that is added. */
    private static final String[][] TYPES = {
        {"image/jpeg", "jpg", "jpeg", "jpe"}, {"image/png", "png"}, {"image/gif", "gif"}, {"image/webp", "webp"},
        {"image/bmp", "bmp"}, {"image/heic", "heic"}, {"image/heif", "heif"},
        {"video/mp4", "mp4", "m4v"}, {"video/webm", "webm"}, {"video/3gpp", "3gp", "3gpp"},
        {"video/quicktime", "mov"}, {"audio/mpeg", "mp3", "mpga"}, {"audio/mp4", "m4a"}, {"audio/ogg", "ogg", "oga"},
        {"audio/x-wav", "wav"}, {"audio/flac", "flac"}, {"audio/aac", "aac"},
    };

    private MediaRules() {
    }

    private static List<String> dirs(String collection) {
        switch (collection) {
            case "images": return IMAGE_DIRS;
            case "video": return VIDEO_DIRS;
            case "audio": return AUDIO_DIRS;
            default: throw new IllegalArgumentException("Unknown collection " + collection);
        }
    }

    /** The folder an insert gets without one. */
    private static String defaultDir(String collection) {
        return collection.equals("images") ? "Pictures" : collection.equals("video") ? "Movies" : "Music";
    }

    /** MediaColumns.MEDIA_TYPE of a collection: 1 image, 2 audio, 3 video. */
    public static int mediaType(String collection) {
        return collection.equals("images") ? 1 : collection.equals("audio") ? 2 : 3;
    }

    /**
     * The RELATIVE_PATH of an insert into `collection` of `volume`, ended by a slash; the collection's default folder
     * for none. IllegalArgumentException with Android's message when its first folder is not allowed, and for a path
     * that is no relative path of plain components.
     */
    public static String relativePath(String collection, String volume, String path) {
        if (path == null || path.isEmpty()) {
            return defaultDir(collection) + "/";
        }
        final String trimmed = path.endsWith("/") ? path.substring(0, path.length() - 1) : path;
        for (final String component : trimmed.split("/", -1)) {
            if (component.isEmpty() || component.equals(".") || component.equals("..")
                    || component.indexOf('\0') >= 0) {
                throw new IllegalArgumentException("Invalid path " + path);
            }
        }
        final String primary = trimmed.split("/", 2)[0];
        if (!dirs(collection).contains(primary)) {
            throw new IllegalArgumentException("Primary directory " + primary + " not allowed for content://media/"
                    + volume + "/" + collection + "/media; allowed directories are " + dirs(collection));
        }
        return trimmed + "/";
    }

    private static String[] typeOf(String mime) {
        for (final String[] type : TYPES) {
            if (type[0].equals(mime)) {
                return type;
            }
        }
        return null;
    }

    /** The extension of `name` without the dot, "" when there is none. */
    private static String extension(String name) {
        final int dot = name.lastIndexOf('.');
        return dot <= 0 ? "" : name.substring(dot + 1);
    }

    /** `name` with the extension of `mime` added, unless its own already belongs to the type. */
    public static String fileName(String name, String mime) {
        final String[] type = mime == null ? null : typeOf(mime);
        if (type == null) {
            return name;
        }
        final String own = extension(name).toLowerCase(Locale.ROOT);
        return Arrays.asList(type).subList(1, type.length).contains(own) ? name : name + "." + type[1];
    }

    /** The MIME type an extension says, null for none known. */
    public static String mimeOf(String name) {
        final String own = extension(name).toLowerCase(Locale.ROOT);
        for (final String[] type : TYPES) {
            if (Arrays.asList(type).subList(1, type.length).contains(own)) {
                return type[0];
            }
        }
        return null;
    }

    /** `name`, or with " (n)" before its extension for the first n from 1 that no name in `taken` has. */
    public static String unique(String name, Set<String> taken) {
        if (!taken.contains(name)) {
            return name;
        }
        final int dot = name.lastIndexOf('.');
        final String base = dot <= 0 ? name : name.substring(0, dot);
        final String tail = dot <= 0 ? "" : name.substring(dot);
        for (int n = 1; ; n++) {
            final String candidate = base + " (" + n + ")" + tail;
            if (!taken.contains(candidate)) {
                return candidate;
            }
        }
    }

    /** The title of a file name: without its last extension. */
    public static String title(String name) {
        final int dot = name.lastIndexOf('.');
        return dot <= 0 ? name : name.substring(0, dot);
    }

    /** The name of the file of a pending item that expires at `expiresSeconds` (seconds since 1970). */
    public static String pendingName(String name, long expiresSeconds) {
        return ".pending-" + expiresSeconds + "-" + name;
    }

    public static String pendingTitle(String title, long expiresSeconds) {
        return ".pending-" + expiresSeconds + "-" + title;
    }

    /** The name of an insert that has none: the time in milliseconds, with the type's extension. */
    public static String unnamed(long millis, String mime) {
        final String[] type = mime == null ? null : typeOf(mime);
        return millis + (type == null ? "" : "." + type[1]);
    }
}
