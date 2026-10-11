package io.github.kisame76.minato.test.mime;

import android.app.Application;
import android.util.Log;
import android.webkit.MimeTypeMap;
import android.webkit.URLUtil;

import java.net.URLConnection;

/**
 * The test of android.webkit.MimeTypeMap and android.webkit.URLUtil, without a window (retest 8's resolve list,
 * batch B1). It logs under the tag mime-test what the classes answer for extensions, MIME types and URLs, through
 * public API only. tools/android-reference.sh takes the log from Android 15, and tools/mime-check.sh compares Minato's
 * with it.
 */
public class MimeApplication extends Application {
    private static final String TAG = "mime-test";

    private interface Part {
        void run() throws Exception;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        part("extensions", MimeApplication::extensions);
        part("types", MimeApplication::types);
        part("urls", MimeApplication::urls);
        part("urlutil", MimeApplication::urlutil);
        part("guess", MimeApplication::guess);
        Log.i(TAG, "done");
    }

    private static void part(String name, Part part) {
        try {
            part.run();
        } catch (Throwable t) {
            Log.i(TAG, name + ": failed: " + t);
            Log.i(TAG + "-device", Log.getStackTraceString(t));
        }
    }

    private static void extensions() {
        MimeTypeMap map = MimeTypeMap.getSingleton();
        String[] extensions = {"png", "PNG", "jpg", "jpeg", "gif", "webp", "html", "htm", "txt", "text", "json", "xml",
            "pdf", "zip", "apk", "mp3", "mp4", "m4a", "ogg", "wav", "svg", "js", "css", "csv", "md", "ttf", "woff2",
            "bin", "xyz", "", "obb", "heic", "avif", "3gp", "mkv", "webm", "flac", "docx", "xlsx", "epub", "wasm"};
        for (String extension : extensions) {
            Log.i(TAG, "extension '" + extension + "': " + map.getMimeTypeFromExtension(extension) + " has "
                    + map.hasExtension(extension));
        }
        Log.i(TAG, "extension null: " + map.getMimeTypeFromExtension(null) + " has " + map.hasExtension(null));
    }

    private static void types() {
        MimeTypeMap map = MimeTypeMap.getSingleton();
        String[] types = {"text/plain", "text/html", "image/jpeg", "image/png", "image/gif", "image/webp",
            "application/json", "application/xml", "text/xml", "application/pdf", "application/zip", "audio/mpeg",
            "audio/mp4", "video/mp4", "video/webm", "audio/ogg", "audio/x-wav", "image/svg+xml", "text/css",
            "application/javascript", "text/javascript", "text/csv", "application/vnd.android.package-archive",
            "application/octet-stream", "foo/bar", "", "TEXT/PLAIN", "image/heic", "font/ttf", "application/epub+zip"};
        for (String type : types) {
            Log.i(TAG, "type '" + type + "': " + map.getExtensionFromMimeType(type) + " has "
                    + map.hasMimeType(type));
        }
        Log.i(TAG, "type null: " + map.getExtensionFromMimeType(null) + " has " + map.hasMimeType(null));
    }

    private static void urls() {
        String[] urls = {"http://example.com/a/photo.jpg", "http://example.com/a/photo.JPG?x=1", "file:///sdcard/a.tar.gz",
            "http://example.com/", "http://example.com/a.txt#frag", "http://example.com/a/b.html?q=a.b", "", "name",
            "name.", ".hidden", "http://example.com/a b.png", "content://media/external/images/media/12"};
        for (String url : urls) {
            Log.i(TAG, "url '" + url + "': '" + MimeTypeMap.getFileExtensionFromUrl(url) + "'");
        }
        Log.i(TAG, "url null: '" + MimeTypeMap.getFileExtensionFromUrl(null) + "'");
    }

    private static void urlutil() {
        String[] urls = {"http://example.com", "https://example.com/x", "HTTP://EXAMPLE.COM", "ftp://example.com",
            "file:///sdcard/x.txt", "content://media/x", "file:///android_asset/a.html", "file:///android_res/a.png",
            "javascript:alert(1)", "about:blank", "data:text/plain,hi", "example.com", ""};
        for (String url : urls) {
            Log.i(TAG, "urlutil '" + url + "': valid " + URLUtil.isValidUrl(url) + " http " + URLUtil.isHttpUrl(url)
                    + " https " + URLUtil.isHttpsUrl(url) + " network " + URLUtil.isNetworkUrl(url) + " file "
                    + URLUtil.isFileUrl(url) + " content " + URLUtil.isContentUrl(url) + " asset "
                    + URLUtil.isAssetUrl(url) + " about " + URLUtil.isAboutUrl(url) + " data "
                    + URLUtil.isDataUrl(url) + " js " + URLUtil.isJavaScriptUrl(url));
        }
        Log.i(TAG, "urlutil null: valid " + URLUtil.isValidUrl(null) + " http " + URLUtil.isHttpUrl(null));
        Log.i(TAG, "urlutil guessUrl: " + URLUtil.guessUrl("example.com") + " " + URLUtil.guessUrl("HTTP://a.b/c")
                + " " + URLUtil.guessUrl("file:/sdcard/x") + " " + URLUtil.guessUrl("  example.com  ") + " "
                + URLUtil.guessUrl("http:///a") + " " + URLUtil.guessUrl("https:/a.b"));
        Log.i(TAG, "urlutil stripAnchor: " + URLUtil.stripAnchor("http://a.b/c#d") + " "
                + URLUtil.stripAnchor("http://a.b/c"));
        Log.i(TAG, "urlutil search: " + URLUtil.composeSearchUrl("a b", "http://x/?q=%s", "%s"));
        Log.i(TAG, "urlutil decode: " + new String(URLUtil.decode("a%20b%2Fc".getBytes())));
    }

    private static void guess() {
        String[][] names = {
            {"http://example.com/a/photo.jpg", null, null},
            {"http://example.com/a/photo.jpg", null, "image/jpeg"},
            {"http://example.com/a/photo", null, "image/png"},
            {"http://example.com/a/photo", null, "text/html"},
            {"http://example.com/a/photo", null, "text/plain"},
            {"http://example.com/a/photo", null, "application/octet-stream"},
            {"http://example.com/a/photo.bin", null, "application/octet-stream"},
            {"http://example.com/a/photo.php?x=1", null, "text/html"},
            {"http://example.com/", null, "text/html"},
            {"http://example.com/", null, null},
            {"http://example.com/a/b.txt", "attachment; filename=\"c.zip\"", "application/zip"},
            {"http://example.com/a/b.txt", "attachment; filename=c.zip", null},
            {"http://example.com/a/b.txt", "attachment; filename=\"../x.zip\"", null},
            {"http://example.com/a/b.txt", "inline", "text/plain"},
            {"http://example.com/a/b%20c.txt", null, "text/plain"},
            {"http://example.com/a/b.tar.gz", null, "application/x-gzip"},
            {"http://example.com/a/archive.zip", null, "application/zip"},
            {"http://example.com/a/song", null, "audio/mpeg"},
            {"http://example.com/a/video", null, "video/mp4"},
            {"http://example.com/a/app", null, "application/vnd.android.package-archive"},
            {"http://example.com/a/data", null, "application/json"},
            {"http://example.com/a/data", null, "text/csv"},
            {"http://example.com/a/x", null, "application/pdf"},
        };
        for (String[] n : names) {
            Log.i(TAG, "guessFileName " + n[0] + " | " + n[1] + " | " + n[2] + ": "
                    + URLUtil.guessFileName(n[0], n[1], n[2]));
        }
        String[] files = {"a.png", "a.PNG", "a.jpeg", "a.txt", "a.html", "a.json", "a.mp3", "a.mp4", "a.apk", "a.zip",
            "a.pdf", "a.csv", "a.svg", "a.xyz", "a", "a.", ".png", "dir.d/a", "http://x/a.gif", "a b.png"};
        for (String file : files) {
            Log.i(TAG, "guessContentTypeFromName " + file + ": " + URLConnection.guessContentTypeFromName(file));
        }
    }
}
