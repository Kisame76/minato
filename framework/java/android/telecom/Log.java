/*
 * Telecom's Log as BlockedNumberContract and CallLog call it from an app process: a message with a prefix, a format and
 * arguments, and the way it names a phone number, which is by a mark instead of the digits. Telecom's own Log keeps
 * sessions and events that only its services read; here a line goes to the log under the tag "TelecomFramework", as
 * Telecom's does for a call from an app, and a number is "[PII]" in it.
 */
package android.telecom;

/** @hide */
public final class Log {
    public static final String TAG = "TelecomFramework";
    private static final String PII = "[PII]";

    private Log() {
    }

    /** CallAttributes' toString: any piece of personal information is the mark (batch H4). */
    public static String pii(Object pii) {
        return pii == null ? String.valueOf((Object) null) : PII;
    }

    public static void d(Object prefix, String format, Object... args) {
        android.util.Log.d(TAG, line(prefix, format, args));
    }

    public static void i(Object prefix, String format, Object... args) {
        android.util.Log.i(TAG, line(prefix, format, args));
    }

    public static void w(Object prefix, String format, Object... args) {
        android.util.Log.w(TAG, line(prefix, format, args));
    }

    public static void e(Object prefix, Throwable throwable, String format, Object... args) {
        android.util.Log.e(TAG, line(prefix, format, args), throwable);
    }

    public static String piiHandle(Object pii) {
        return pii == null ? "null" : PII;
    }

    private static String line(Object prefix, String format, Object... args) {
        String text = args == null || args.length == 0 ? format : String.format(format, args);
        return prefix == null ? text : prefix + ": " + text;
    }
}
