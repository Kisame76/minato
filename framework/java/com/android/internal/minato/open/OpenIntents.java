package com.android.internal.minato.open;

import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.ResolveInfo;
import android.database.Cursor;
import android.net.Uri;
import android.util.Log;

import com.android.minato.open.OpenRequest;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * The intents that Android answers with an app of the phone and that Minato answers with iOS (ADR-0062 decision 1):
 * links to a browser, mail, calls and texts, maps and the share sheet. What they resolve to, the activity that stands in
 * for the app, and the text of the request the host gets (APKRT_REQUEST_OPEN, built by com.android.minato.open.OpenRequest).
 * The intent decides by action, data scheme and type, for an intent that names no component and no package.
 */
public final class OpenIntents {
    private static final String TAG = "OpenIntents";

    /** The framework class that stands in for the phone's apps. */
    static final String OPEN_CLASS = "com.android.internal.minato.open.SystemOpenActivity";
    /** What a chooser resolves to, and a send that is none, on AOSP. */
    static final String CHOOSER_CLASS = "com.android.internal.app.ChooserActivity";
    static final String RESOLVER_CLASS = "com.android.internal.app.ResolverActivity";

    /** The most bytes of one shared file, and of all of them, that the guest copies for the host. */
    static final long FILE_LIMIT = 32L << 20;
    static final long TOTAL_LIMIT = 96L << 20;
    /** The most streams looked at, whatever they come to: a list of a million bad URIs costs nothing more. */
    static final int MAX_STREAMS = 20;
    /** The most addresses of one list. */
    static final int MAX_ADDRESSES = 50;

    private OpenIntents() {
    }

    /** Whether `intent` names a component other than this class's own stand-in, which ActivityThread sets at the start. */
    private static boolean explicit(Intent intent) {
        final android.content.ComponentName component = intent.getComponent();
        return component != null && !OPEN_CLASS.equals(component.getClassName());
    }

    /** The intent the request is about: an ACTION_CHOOSER's inner intent, else the intent itself; null for a chooser without one. */
    static Intent target(Intent intent) {
        if (Intent.ACTION_CHOOSER.equals(intent.getAction())) {
            return intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent.class);
        }
        return intent;
    }

    private static String scheme(Intent intent) {
        final Uri data = intent.getData();
        return data == null || data.getScheme() == null ? null : data.getScheme().toLowerCase(Locale.ROOT);
    }

    /** Which kind of request this intent is, a kind of OpenRequest, or 0 when it is not one that iOS answers. */
    public static int kindOf(Intent intent) {
        if (explicit(intent) || intent.getPackage() != null) {
            return 0;
        }
        final Intent target = target(intent);
        if (target == null || explicit(target) || target.getPackage() != null
                || Intent.ACTION_CHOOSER.equals(target.getAction())) {
            return 0;
        }
        final String action = target.getAction();
        final String scheme = scheme(target);
        if (action == null) {
            return 0;
        }
        switch (action) {
            case Intent.ACTION_VIEW:
                if (scheme == null) {
                    return 0;
                }
                switch (scheme) {
                    case "http":
                    case "https":
                    case "tel":
                    case "geo":
                    case "google.navigation":
                        return OpenRequest.VIEW;
                    case "mailto":
                        return OpenRequest.MAIL;
                    case "sms":
                    case "smsto":
                        return OpenRequest.SMS;
                    default:
                        return 0;
                }
            case Intent.ACTION_DIAL:
                return "tel".equals(scheme) ? OpenRequest.VIEW : 0;
            case Intent.ACTION_SENDTO:
                return "mailto".equals(scheme) ? OpenRequest.MAIL
                        : "sms".equals(scheme) || "smsto".equals(scheme) ? OpenRequest.SMS : 0;
            case Intent.ACTION_SEND:
            case Intent.ACTION_SEND_MULTIPLE:
                return target.getType() == null ? 0
                        : "message/rfc822".equals(target.getType()) && target.hasExtra(Intent.EXTRA_EMAIL)
                        ? OpenRequest.MAIL : OpenRequest.SHARE;
            default:
                return 0;
        }
    }

    /** The activity of the framework class, in the app's package as an activity of the app is. */
    public static ActivityInfo activityInfo(ApplicationInfo app) {
        final ActivityInfo info = new ActivityInfo();
        info.packageName = app.packageName;
        info.name = OPEN_CLASS;
        info.applicationInfo = app;
        info.processName = app.processName;
        info.exported = false;
        info.launchMode = ActivityInfo.LAUNCH_MULTIPLE;
        // A change of the configuration does not recreate it, which would ask the host a second time.
        info.configChanges = -1;
        info.theme = com.android.internal.R.style.Theme_Translucent_NoTitleBar;
        return info;
    }

    /** What the intent resolves to when the app has no component for it: the phone's app, by the name of iOS's. */
    public static ResolveInfo resolveInfo(Intent intent) {
        final int kind = kindOf(intent);
        final boolean chooser = Intent.ACTION_CHOOSER.equals(intent.getAction());
        final ResolveInfo resolved = new ResolveInfo();
        final ActivityInfo info = new ActivityInfo();
        if (chooser) {
            info.packageName = "android";
            info.name = CHOOSER_CLASS;
        } else if (kind == OpenRequest.SHARE) {
            info.packageName = "android";
            info.name = RESOLVER_CLASS;
        } else {
            final Intent target = target(intent);
            final String scheme = target == null ? null : scheme(target);
            info.packageName = kind == OpenRequest.MAIL ? "com.apple.mobilemail"
                    : kind == OpenRequest.SMS ? "com.apple.MobileSMS"
                    : "tel".equals(scheme) ? "com.apple.mobilephone"
                    : "geo".equals(scheme) || "google.navigation".equals(scheme) ? "com.apple.Maps"
                    : "com.apple.mobilesafari";
            info.name = info.packageName + ".Main";
        }
        info.exported = true;
        info.applicationInfo = new ApplicationInfo();
        info.applicationInfo.packageName = info.packageName;
        resolved.activityInfo = info;
        resolved.match = 0x108000;
        resolved.priority = 0;
        resolved.preferredOrder = 0;
        resolved.isDefault = true;
        return resolved;
    }

    /** The decoded query parameter `name` of a hierarchical or opaque URI's query part, or null. */
    private static String parameter(Uri uri, String name) {
        final String spec = uri.getEncodedSchemeSpecificPart();
        final int question = spec == null ? -1 : spec.indexOf('?');
        if (question < 0) {
            return null;
        }
        for (String pair : spec.substring(question + 1).split("&")) {
            final int equals = pair.indexOf('=');
            if (equals > 0 && Uri.decode(pair.substring(0, equals)).equalsIgnoreCase(name)) {
                return Uri.decode(pair.substring(equals + 1));
            }
        }
        return null;
    }

    /** The decoded part of an opaque URI before its query: the addresses of a mailto:, the number of an sms:. */
    private static String path(Uri uri) {
        final String spec = uri.getEncodedSchemeSpecificPart();
        if (spec == null) {
            return "";
        }
        final int question = spec.indexOf('?');
        return Uri.decode(question < 0 ? spec : spec.substring(0, question));
    }

    private static String[] split(String addresses) {
        return addresses == null || addresses.isEmpty() ? new String[0] : addresses.split("[,;]");
    }

    private static String[] join(String[] first, String[] second) {
        final List<String> all = new ArrayList<>();
        for (String[] part : new String[][] {first, second}) {
            if (part != null) {
                for (String one : part) {
                    all.add(one);
                }
            }
        }
        return all.toArray(new String[0]);
    }

    private static String text(Intent intent, String key) {
        final CharSequence value = intent.getCharSequenceExtra(key);
        return value == null ? null : value.toString();
    }

    /**
     * The text of the request for `intent` as request `id`, or null when there is none to send: the intent is not one
     * that iOS answers, or what it holds cannot be sent. A share copies the streams the app may read into the share
     * folder before it returns.
     */
    public static String requestText(Context context, Intent intent, long id) {
        final int kind = kindOf(intent);
        final Intent target = target(intent);
        if (kind == 0 || target == null) {
            return null;
        }
        final Uri data = target.getData();
        switch (kind) {
            case OpenRequest.VIEW: {
                final String scheme = scheme(target);
                if ("geo".equals(scheme)) {
                    return OpenRequest.view(mapsGeo(data));
                }
                if ("google.navigation".equals(scheme)) {
                    return OpenRequest.view(mapsNavigation(data));
                }
                return OpenRequest.view(data.toString());
            }
            case OpenRequest.MAIL: {
                if (data != null && "mailto".equals(scheme(target))) {
                    return OpenRequest.mail(
                            join(join(split(path(data)), split(parameter(data, "to"))),
                                    target.getStringArrayExtra(Intent.EXTRA_EMAIL)),
                            join(split(parameter(data, "cc")), target.getStringArrayExtra(Intent.EXTRA_CC)),
                            join(split(parameter(data, "bcc")), target.getStringArrayExtra(Intent.EXTRA_BCC)),
                            parameter(data, "subject") != null ? parameter(data, "subject")
                                    : text(target, Intent.EXTRA_SUBJECT),
                            parameter(data, "body") != null ? parameter(data, "body") : text(target, Intent.EXTRA_TEXT));
                }
                return OpenRequest.mail(target.getStringArrayExtra(Intent.EXTRA_EMAIL),
                        target.getStringArrayExtra(Intent.EXTRA_CC), target.getStringArrayExtra(Intent.EXTRA_BCC),
                        text(target, Intent.EXTRA_SUBJECT), text(target, Intent.EXTRA_TEXT));
            }
            case OpenRequest.SMS: {
                String body = data == null ? null : parameter(data, "body");
                if (body == null) {
                    body = text(target, "sms_body");
                }
                if (body == null) {
                    body = text(target, Intent.EXTRA_TEXT);
                }
                return OpenRequest.sms(data == null ? null : path(data), body);
            }
            default:
                return share(context, target, id);
        }
    }

    /** maps://?ll=<lat,lon>&q=<query> for geo:<lat>,<lon>?q=<query>; a lone 0,0 point is none. */
    static String mapsGeo(Uri geo) {
        final String point = path(geo).split(";")[0];
        final String query = parameter(geo, "q");
        final StringBuilder url = new StringBuilder("maps://?");
        boolean any = false;
        if (point.matches("-?[0-9]+(\\.[0-9]+)?,-?[0-9]+(\\.[0-9]+)?") && !point.matches("-?0+(\\.0+)?,-?0+(\\.0+)?")) {
            url.append("ll=").append(OpenRequest.encode(point));
            any = true;
        }
        if (query != null && !query.isEmpty()) {
            url.append(any ? "&" : "").append("q=").append(OpenRequest.encode(query));
            any = true;
        }
        return any ? url.toString() : "maps://";
    }

    /** maps://?daddr=<destination>&dirflg=<mode> for google.navigation:q=<destination>&mode=<d|w|b|l>. */
    static String mapsNavigation(Uri navigation) {
        final String destination = parameter(navigation, "q");
        if (destination == null || destination.isEmpty()) {
            return "maps://";
        }
        final String mode = parameter(navigation, "mode");
        return "maps://?daddr=" + OpenRequest.encode(destination) + ("w".equals(mode) ? "&dirflg=w" : "&dirflg=d");
    }

    /** The folder the share of request `id` is put into: <data>/.minato/share/<id>. */
    public static File shareFolder(Context context, long id) {
        return new File(shareRoot(context), Long.toString(id));
    }

    public static File shareRoot(Context context) {
        return new File(new File(context.getApplicationInfo().dataDir, ".minato"), "share");
    }

    private static String share(Context context, Intent intent, long id) {
        final String type = intent.getType();
        final List<Uri> streams = new ArrayList<>();
        if (Intent.ACTION_SEND_MULTIPLE.equals(intent.getAction())) {
            final ArrayList<?> several = intent.getParcelableArrayListExtra(Intent.EXTRA_STREAM, android.os.Parcelable.class);
            for (int at = 0; several != null && at < several.size() && at < MAX_STREAMS; at++) {
                if (several.get(at) instanceof Uri) {
                    streams.add((Uri) several.get(at));
                }
            }
        } else {
            final Uri one = intent.getParcelableExtra(Intent.EXTRA_STREAM, Uri.class);
            if (one != null) {
                streams.add(one);
            }
        }
        final List<String> names = new ArrayList<>();
        final List<String> types = new ArrayList<>();
        final File folder = shareFolder(context, id);
        long total = 0;
        int looked = 0;
        for (Uri stream : streams) {
            if (names.size() == OpenRequest.SHARE_FILES || ++looked > MAX_STREAMS) {
                break;
            }
            if (stream == null || !ContentResolver.SCHEME_CONTENT.equals(stream.getScheme())) {
                continue;  // a file: URI is a file the app may not give away through iOS; Android's own refuses it
            }
            final String name = fileName(context, stream, names);
            final long copied = copy(context, stream, folder, name, Math.min(FILE_LIMIT, TOTAL_LIMIT - total));
            if (copied < 0) {
                continue;
            }
            total += copied;
            names.add(name);
            String streamType = null;
            try {
                streamType = context.getContentResolver().getType(stream);
            } catch (RuntimeException e) {
                Log.w(TAG, "No type for a shared stream: " + e.getClass().getSimpleName());
            }
            types.add(streamType != null ? streamType : type);
        }
        return OpenRequest.share(text(intent, Intent.EXTRA_SUBJECT), text(intent, Intent.EXTRA_TEXT), names, types);
    }

    /** A plain file name for `stream`, none of `taken`: its display name, else its last path segment, else "file". */
    static String fileName(Context context, Uri stream, List<String> taken) {
        String name = null;
        try (Cursor cursor = context.getContentResolver().query(stream, new String[] {"_display_name"}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst() && !cursor.isNull(0)) {
                name = cursor.getString(0);
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "No display name for a shared stream: " + e.getClass().getSimpleName());
        }
        if (name == null || name.isEmpty()) {
            name = stream.getLastPathSegment();
        }
        return uniqueName(name == null ? "file" : name, taken);
    }

    /** `name` with every character a plain name may not have turned into '_', cut to 255 bytes, and made new among `taken`. */
    static String uniqueName(String name, List<String> taken) {
        final StringBuilder plain = new StringBuilder();
        for (int at = 0; at < name.length(); ) {
            final int point = name.codePointAt(at);
            at += Character.charCount(point);
            plain.appendCodePoint(OpenRequest.plainPoint(point) ? point : '_');
        }
        String candidate = OpenRequest.truncate(plain.toString(), 240);
        if (candidate.isEmpty() || candidate.equals(".") || candidate.equals("..")) {
            candidate = "file";
        }
        String unique = candidate;
        for (int number = 2; hasName(taken, unique); number++) {
            unique = candidate + "-" + number;
        }
        return unique;
    }

    /** Whether `names` has `name` under the file system's folding: case and Unicode normalisation (iOS's does both). */
    private static boolean hasName(List<String> names, String name) {
        final String key = fold(name);
        for (String taken : names) {
            if (fold(taken).equals(key)) {
                return true;
            }
        }
        return false;
    }

    private static String fold(String name) {
        return java.text.Normalizer.normalize(name, java.text.Normalizer.Form.NFC).toLowerCase(Locale.ROOT);
    }

    /** The stream's bytes into folder/name, at most `limit` of them: the bytes copied, or -1 when it is not copied. */
    private static long copy(Context context, Uri stream, File folder, String name, long limit) {
        final File file = new File(folder, name);
        boolean created = false;
        try (InputStream in = context.getContentResolver().openInputStream(stream)) {
            if (in == null || (!folder.isDirectory() && !folder.mkdirs())) {
                return -1;
            }
            long size = 0;
            try (OutputStream out = Files.newOutputStream(file.toPath(), java.nio.file.StandardOpenOption.CREATE_NEW,
                    java.nio.file.StandardOpenOption.WRITE)) {
                created = true;
                final byte[] buffer = new byte[16384];
                for (int count; (count = in.read(buffer)) > 0; ) {
                    size += count;
                    if (size > limit) {
                        throw new IOException("over the limit");
                    }
                    out.write(buffer, 0, count);
                }
            }
            return size;
        } catch (IOException | RuntimeException e) {
            Log.w(TAG, "A shared stream is not copied: " + e.getClass().getSimpleName());
            if (created) {
                file.delete();
            }
            return -1;
        }
    }

    /** Deletes the share folder of one request, or of every request when `id` is negative; links are deleted, not followed. */
    public static void deleteShare(Context context, long id) {
        final File root = shareRoot(context);
        final File[] folders = id < 0 ? root.listFiles() : new File[] {shareFolder(context, id)};
        if (folders == null) {
            return;
        }
        for (File folder : folders) {
            final File[] files = folder.listFiles();
            if (files != null && !Files.isSymbolicLink(folder.toPath())) {
                for (File file : files) {
                    file.delete();
                }
            }
            folder.delete();
        }
    }
}
