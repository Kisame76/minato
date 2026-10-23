package com.android.minato.media;

import java.util.Set;

/**
 * The rules of MediaStore's inserts (ADR-0057 step e), measured on Android 15 by mediastore-test and checked here on
 * the host's JVM (tools/framework-host-check.sh): the folders a collection allows and the message of the refusal,
 * the default folder, the extension a MIME type needs, the number a taken name gets, the title, and the name of a
 * pending file.
 */
public final class MediaRulesTest {
    private static int failures;

    private static void check(boolean ok, String what) {
        if (!ok) {
            failures++;
            System.out.println("FAIL: " + what);
        }
    }

    private static String refusal(String collection, String path) {
        try {
            MediaRules.relativePath(collection, "external", path);
            return null;
        } catch (IllegalArgumentException e) {
            return e.getMessage();
        }
    }

    public static void main(String[] args) {
        check(MediaRules.relativePath("images", "external", "Pictures/Minato").equals("Pictures/Minato/"),
                "a folder gets its slash");
        check(MediaRules.relativePath("images", "external", null).equals("Pictures/"), "images default to Pictures/");
        check(MediaRules.relativePath("video", "external", "").equals("Movies/"), "video defaults to Movies/");
        check(MediaRules.relativePath("audio", "external", null).equals("Music/"), "audio defaults to Music/");
        check(MediaRules.relativePath("images", "external", "DCIM/x/").equals("DCIM/x/"), "DCIM/x/ is allowed");
        check(("Primary directory Documents not allowed for content://media/external/images/media; "
                + "allowed directories are [DCIM, Pictures]").equals(refusal("images", "Documents/x")),
                "the message of a refused image folder: " + refusal("images", "Documents/x"));
        check(("Primary directory Music not allowed for content://media/external/video/media; "
                + "allowed directories are [DCIM, Movies, Pictures]").equals(refusal("video", "Music/x")),
                "video: " + refusal("video", "Music/x"));
        check(("Primary directory Pictures not allowed for content://media/external/audio/media; allowed directories "
                + "are [Alarms, Audiobooks, Music, Notifications, Podcasts, Recordings, Ringtones]")
                .equals(refusal("audio", "Pictures/x")), "audio: " + refusal("audio", "Pictures/x"));
        check(refusal("images", "Pictures/../../x") != null, "a parent component is refused");
        check(refusal("images", "/Pictures") != null, "a leading slash is refused");
        check(refusal("images", "Pictures//x") != null, "an empty component is refused");

        check(MediaRules.fileName("probe.bin", "audio/mpeg").equals("probe.bin.mp3"), "a foreign extension");
        check(MediaRules.fileName("probe.bin", "image/jpeg").equals("probe.bin.jpg"), "jpeg gets jpg");
        check(MediaRules.fileName("shot.jpg", "image/jpeg").equals("shot.jpg"), "the type's extension stays");
        check(MediaRules.fileName("shot.jpeg", "image/jpeg").equals("shot.jpeg"), "jpeg is a jpeg extension too");
        check(MediaRules.fileName("shot", "image/png").equals("shot.png"), "no extension gets the type's");
        check(MediaRules.fileName("clip.MP4", "video/mp4").equals("clip.MP4"), "the case of an extension is kept");
        check(MediaRules.fileName("x", "application/x-odd").equals("x"), "an unknown type changes nothing");

        check(MediaRules.unique("shot.jpg", Set.of("shot.jpg")).equals("shot (1).jpg"), "a taken name gets (1)");
        check(MediaRules.unique("shot.jpg", Set.of("shot.jpg", "shot (1).jpg")).equals("shot (2).jpg"), "then (2)");
        check(MediaRules.unique("a.b.jpg", Set.of()).equals("a.b.jpg"), "a free name stays");
        check(MediaRules.unique("noext", Set.of("noext")).equals("noext (1)"), "no extension: the number last");

        check(MediaRules.title("shot.jpg").equals("shot"), "the title is the name without extension");
        check(MediaRules.title("probe.bin.mp3").equals("probe.bin"), "only the last extension goes");
        check(MediaRules.title("noext").equals("noext"), "no extension, no change");
        check(MediaRules.pendingName("shot.jpg", 1793372653L).equals(".pending-1793372653-shot.jpg"),
                "a pending file's name");
        check(MediaRules.pendingTitle("shot", 1793372653L).equals(".pending-1793372653-shot"), "a pending title");
        check(MediaRules.unnamed(1792767890804L, "image/jpeg").equals("1792767890804.jpg"), "an unnamed insert");
        check(MediaRules.mediaType("images") == 1 && MediaRules.mediaType("audio") == 2
                && MediaRules.mediaType("video") == 3, "the media types");
        if (failures > 0) {
            throw new AssertionError(failures + " failures");
        }
        System.out.println("MediaRulesTest ok");
    }
}
