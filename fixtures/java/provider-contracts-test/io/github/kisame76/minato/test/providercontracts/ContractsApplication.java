package io.github.kisame76.minato.test.providercontracts;

import android.app.Application;
import android.content.ContentResolver;
import android.database.Cursor;
import android.net.Uri;
import android.provider.BlockedNumberContract;
import android.provider.CalendarContract;
import android.provider.CallLog;
import android.provider.VoicemailContract;
import android.util.Log;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.lang.reflect.Field;
import java.lang.reflect.Member;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * The provider contracts' test (group 2 of the framework breadth). Part 1: each type that assets/types.txt names is
 * loaded, and logs its shape: how many of its API's public and protected fields and methods it declares (assets/
 * members.txt: the other public members of the classes differ between the phone's classes and ours, hidden ones and
 * removed ones) and a digest of their declarations, with the value of each constant that is a string, a number or a
 * Uri, so that a class from another source or version differs. Part 2: what the ContentResolver and
 * the contracts' calls answer for the contracts' own URIs, with the permissions the app holds (none, in this app).
 */
public class ContractsApplication extends Application {
    private static String TAG = "provider-contracts-test";
    /** The tag of lines that are read, not compared. */
    private static String DEVICE = "provider-contracts-test-device";

    /** The API's members, "<type> F <name>" and "<type> M <name> <parameters>", from assets/members.txt. */
    private final Set<String> mApi = new HashSet<>();

    @Override
    public void onCreate() {
        super.onCreate();
        // The twin's sources are these, its label its tag.
        TAG = getApplicationInfo().loadLabel(getPackageManager()).toString();
        DEVICE = TAG + "-device";
        readMembers();
        try (BufferedReader reader = new BufferedReader(
                new InputStreamReader(getAssets().open("types.txt"), StandardCharsets.UTF_8))) {
            for (String line; (line = reader.readLine()) != null;) {
                String name = line.trim();
                if (!name.isEmpty() && !name.startsWith("#")) {
                    Log.i(TAG, name + ": " + shape(name));
                }
            }
        } catch (IOException e) {
            Log.i(TAG, "types.txt: " + e);
        }
        queries();
        Log.i(TAG, "done");
    }

    private void readMembers() {
        try (BufferedReader reader = new BufferedReader(
                new InputStreamReader(getAssets().open("members.txt"), StandardCharsets.UTF_8))) {
            for (String line; (line = reader.readLine()) != null;) {
                if (!line.isEmpty() && !line.startsWith("#")) {
                    mApi.add(line.trim());
                }
            }
        } catch (IOException e) {
            Log.i(TAG, "members.txt: " + e);
        }
    }

    private String shape(String name) {
        Class<?> type;
        try {
            type = Class.forName(name, true, getClassLoader());
        } catch (Throwable t) {
            return t.getClass().getName();
        }
        List<String> lines = new ArrayList<>();
        int fields = 0;
        int methods = 0;
        try {
            for (Field field : type.getDeclaredFields()) {
                if (visible(field) && mApi.contains(name + " F " + field.getName())) {
                    fields++;
                    lines.add("F " + Modifier.toString(field.getModifiers()) + " " + field.getType().getName() + " "
                            + field.getName() + value(field));
                }
            }
            for (Method method : type.getDeclaredMethods()) {
                if (visible(method) && !method.isSynthetic()
                        && mApi.contains(name + " M " + method.getName() + " " + method.getParameterCount())) {
                    methods++;
                    StringBuilder line = new StringBuilder("M " + Modifier.toString(method.getModifiers()) + " "
                            + method.getReturnType().getName() + " " + method.getName() + "(");
                    for (Class<?> parameter : method.getParameterTypes()) {
                        line.append(parameter.getName()).append(',');
                    }
                    lines.add(line.append(')').toString());
                }
            }
        } catch (Throwable t) {
            return "reflection " + t.getClass().getName();
        }
        Collections.sort(lines);
        return "loads, " + fields + " fields, " + methods + " methods, digest " + digest(lines);
    }

    private static boolean visible(Member member) {
        int modifiers = member.getModifiers();
        return Modifier.isPublic(modifiers) || Modifier.isProtected(modifiers);
    }

    private static String value(Field field) {
        int modifiers = field.getModifiers();
        if (!Modifier.isStatic(modifiers) || !Modifier.isFinal(modifiers)) {
            return "";
        }
        try {
            Object value = field.get(null);
            if (value instanceof String || value instanceof Number || value instanceof Boolean
                    || value instanceof Uri) {
                return " = " + value;
            }
        } catch (Throwable t) {
            return " = unreadable";
        }
        return "";
    }

    private static String digest(List<String> lines) {
        try {
            MessageDigest sha = MessageDigest.getInstance("SHA-256");
            for (String line : lines) {
                sha.update(line.getBytes(StandardCharsets.UTF_8));
                sha.update((byte) '\n');
            }
            StringBuilder hex = new StringBuilder();
            byte[] bytes = sha.digest();
            for (int i = 0; i < 6; i++) {
                hex.append(String.format("%02x", bytes[i]));
            }
            return hex.toString();
        } catch (Exception e) {
            return e.toString();
        }
    }

    private void queries() {
        query("calendars", CalendarContract.Calendars.CONTENT_URI);
        query("events", CalendarContract.Events.CONTENT_URI);
        query("attendees", CalendarContract.Attendees.CONTENT_URI);
        query("reminders", CalendarContract.Reminders.CONTENT_URI);
        query("instances", CalendarContract.Instances.CONTENT_URI.buildUpon().appendPath("0").appendPath("1").build());
        query("call log", CallLog.Calls.CONTENT_URI);
        query("voicemails", VoicemailContract.Voicemails.CONTENT_URI);
        query("voicemail status", VoicemailContract.Status.CONTENT_URI);
        query("bookmarks", Uri.parse("content://browser/bookmarks"));
        query("blocked numbers", BlockedNumberContract.BlockedNumbers.CONTENT_URI);
        try {
            Log.i(TAG, "can block numbers: " + BlockedNumberContract.canCurrentUserBlockNumbers(this));
        } catch (Throwable t) {
            Log.i(TAG, "can block numbers: " + describe(t));
        }
        try {
            Log.i(TAG, "is blocked: " + BlockedNumberContract.isBlocked(this, "+4930123456"));
        } catch (Throwable t) {
            Log.i(TAG, "is blocked: " + describe(t));
        }
        try {
            Uri inserted = getContentResolver().insert(CalendarContract.Events.CONTENT_URI,
                    new android.content.ContentValues());
            Log.i(TAG, "insert event without values: " + (inserted == null ? "null" : "uri"));
        } catch (Throwable t) {
            Log.i(TAG, "insert event without values: " + describe(t));
        }
    }

    private void query(String what, Uri uri) {
        try (Cursor cursor = getContentResolver().query(uri, null, null, null, null)) {
            if (cursor == null) {
                Log.i(TAG, "query " + what + ": null");
                return;
            }
            String[] columns = cursor.getColumnNames();
            Log.i(TAG, "query " + what + ": cursor, " + cursor.getCount() + " rows, " + columns.length
                    + " columns, digest " + digest(sorted(columns)));
            // The names, for reading a difference; the line is not compared.
            Log.i(DEVICE, "columns of " + what + ": " + String.join(",", columns));
        } catch (Throwable t) {
            Log.i(TAG, "query " + what + ": " + describe(t));
        }
    }

    private static List<String> sorted(String[] names) {
        List<String> list = new ArrayList<>(java.util.Arrays.asList(names));
        Collections.sort(list);
        return list;
    }

    /** The throwable's class and message, with the process record and the pid and uid of a denial masked. */
    private String describe(Throwable t) {
        String message = String.valueOf(t.getMessage());
        Log.i(DEVICE, "raw: " + t.getClass().getName() + ": " + message);
        message = message.replaceAll("ProcessRecord\\{[^}]*\\}", "ProcessRecord{}")
                .replaceAll("pid=\\d+, uid=\\d+", "pid=N, uid=N");
        return t.getClass().getName() + ": " + message;
    }
}
