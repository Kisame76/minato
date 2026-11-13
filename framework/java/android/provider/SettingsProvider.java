/*
 * The settings provider (ADR-0045 decision 4; Godot's stop 10): Android's SettingsProvider, local in the
 * app's process, which ActivityThread.acquireProvider gives for the authority "settings" as system_server publishes
 * Android's. Settings' NameValueCache calls it (GET_, PUT_ and DELETE_ of the three tables), and ContentResolver's
 * queries and writes of a table's URI reach it too. It answers as the tag's SettingsProvider does [verified: its
 * call, query, insert, update and delete]:
 *   - its values are SettingsDefaults', the reference AVD's, kept for the process;
 *   - android_id is ActivityThread's, the host's --android-id; without one, a random value for this start, logged
 *     (a listed gap until 13c, where the library keeps one per signer);
 *   - a read of a hidden key without @Readable, or of one readable only up to a lower target SDK, throws its
 *     SecurityException (enforceSettingReadable), and a table's query leaves such keys out;
 *   - writes need its permissions and throw its exceptions: System's WRITE_SETTINGS through the app op
 *     (Settings.checkAndNoteWriteSettingsOperation) unless the app holds WRITE_SECURE_SETTINGS, which Secure's and
 *     Global's need. Decision 6 grants neither to an app; a write that passed would change the process's value
 *     only, where Android keeps it (a listed gap);
 *   - it gives no generation tracker (MemoryIntArray), so NameValueCache asks it at every read, as it asks a
 *     provider without one.
 * Its caller is always the app itself. Not under com.android.minato, whose classes tools/framework-host-check.sh
 * compiles without android.*.
 */
package android.provider;

import android.Manifest;
import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.text.TextUtils;
import android.util.ArrayMap;
import android.util.ArraySet;
import android.util.Log;

import com.android.internal.util.ArrayUtils;

import java.io.FileNotFoundException;
import java.security.SecureRandom;
import java.util.Arrays;
import java.util.Set;
import java.util.regex.Pattern;

import libcore.util.HexEncoding;

/** @hide */
public final class SettingsProvider extends ContentProvider {
    private static final String TAG = "SettingsProvider";

    private static final String TABLE_SYSTEM = "system";
    private static final String TABLE_SECURE = "secure";
    private static final String TABLE_GLOBAL = "global";

    /** SettingsProvider's: a delete's row count. */
    private static final String RESULT_ROWS_DELETED = "result_rows_deleted";

    /** SettingsProvider's columns, which a query without a projection answers. */
    private static final String[] ALL_COLUMNS = {Settings.NameValueTable._ID, Settings.NameValueTable.NAME,
            Settings.NameValueTable.VALUE, Settings.NameValueTable.IS_PRESERVED_IN_RESTORE};

    /** SettingsProvider's Arguments: a where clause that names one key, by argument or quoted. */
    private static final Pattern WHERE_PATTERN_WITH_PARAM_NO_BRACKETS =
            Pattern.compile("[\\s]*name[\\s]*=[\\s]*\\?[\\s]*");
    private static final Pattern WHERE_PATTERN_WITH_PARAM_IN_BRACKETS =
            Pattern.compile("[\\s]*\\([\\s]*name[\\s]*=[\\s]*\\?[\\s]*\\)[\\s]*");
    private static final Pattern WHERE_PATTERN_NO_PARAM_IN_BRACKETS =
            Pattern.compile("[\\s]*\\([\\s]*name[\\s]*=[\\s]*['\"].*['\"][\\s]*\\)[\\s]*");
    private static final Pattern WHERE_PATTERN_NO_PARAM_NO_BRACKETS =
            Pattern.compile("[\\s]*name[\\s]*=[\\s]*['\"].*['\"][\\s]*");

    private final String mAndroidId;
    /** Each table's values by name, under the provider's lock. */
    private final ArrayMap<String, ArrayMap<String, String>> mTables = new ArrayMap<>();
    /** Each table's keys and their readability, as Settings' classes declare them, made at a table's first read. */
    private final ArrayMap<String, Readability> mReadability = new ArrayMap<>();

    /** A table's keys that Settings declares, those annotated @Readable, and those readable only up to a target. */
    private static final class Readability {
        final Set<String> all = new ArraySet<>();
        final Set<String> readable = new ArraySet<>();
        final ArrayMap<String, Integer> maxTargetSdk = new ArrayMap<>();
    }

    private SettingsProvider(String androidId) {
        mAndroidId = androidId;
    }

    /**
     * The provider of ActivityThread.acquireProvider, attached to the context under the authority "settings", with
     * the host's android_id, or null for a random one.
     */
    public static SettingsProvider create(Context context, String androidId) {
        if (androidId == null) {
            byte[] bytes = new byte[8];
            new SecureRandom().nextBytes(bytes);
            androidId = HexEncoding.encodeToString(bytes, false);
            Log.w(TAG, "No android_id from the host: a random one for this start (ADR-0045)");
        }
        SettingsProvider provider = new SettingsProvider(androidId);
        ProviderInfo info = new ProviderInfo();
        info.authority = Settings.AUTHORITY;
        info.exported = true;
        provider.attachInfo(context, info);
        return provider;
    }

    @Override
    public boolean onCreate() {
        synchronized (mTables) {
            mTables.put(TABLE_SYSTEM, table(SettingsDefaults.SYSTEM));
            mTables.put(TABLE_SECURE, table(SettingsDefaults.SECURE));
            mTables.get(TABLE_SECURE).put(Settings.Secure.ANDROID_ID, mAndroidId);
            mTables.put(TABLE_GLOBAL, table(SettingsDefaults.GLOBAL));
        }
        return true;
    }

    private static ArrayMap<String, String> table(String[] pairs) {
        ArrayMap<String, String> table = new ArrayMap<>(pairs.length / 2);
        for (int i = 0; i < pairs.length; i += 2) {
            table.put(pairs[i], pairs[i + 1]);
        }
        return table;
    }

    @Override
    public Bundle call(String method, String name, Bundle args) {
        switch (method) {
            case Settings.CALL_METHOD_GET_SYSTEM:
                return Bundle.forPair(Settings.NameValueTable.VALUE, get(TABLE_SYSTEM, name));
            case Settings.CALL_METHOD_GET_SECURE:
                return Bundle.forPair(Settings.NameValueTable.VALUE, get(TABLE_SECURE, name));
            case Settings.CALL_METHOD_GET_GLOBAL:
                return Bundle.forPair(Settings.NameValueTable.VALUE, get(TABLE_GLOBAL, name));
            case Settings.CALL_METHOD_PUT_SYSTEM:
                put(TABLE_SYSTEM, name, value(args));
                return null;
            case Settings.CALL_METHOD_PUT_SECURE:
                put(TABLE_SECURE, name, value(args));
                return null;
            case Settings.CALL_METHOD_PUT_GLOBAL:
                put(TABLE_GLOBAL, name, value(args));
                return null;
            case Settings.CALL_METHOD_DELETE_SYSTEM:
                return rowsDeleted(remove(TABLE_SYSTEM, name));
            case Settings.CALL_METHOD_DELETE_SECURE:
                return rowsDeleted(remove(TABLE_SECURE, name));
            case Settings.CALL_METHOD_DELETE_GLOBAL:
                return rowsDeleted(remove(TABLE_GLOBAL, name));
            default:
                // As SettingsProvider's: the device config's, reset and list methods are system ones.
                Log.w(TAG, "call() with invalid method: " + method);
                return null;
        }
    }

    /** As SettingsProvider's getSettingValue: a write's value, null without one. */
    private static String value(Bundle args) {
        return args != null ? args.getString(Settings.NameValueTable.VALUE) : null;
    }

    private static Bundle rowsDeleted(boolean deleted) {
        Bundle result = new Bundle();
        result.putInt(RESULT_ROWS_DELETED, deleted ? 1 : 0);
        return result;
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String where, String[] whereArgs, String order) {
        String[] table = arguments(uri, where, whereArgs, true);
        String[] columns = normalizeProjection(projection);
        MatrixCursor cursor;
        synchronized (mTables) {
            ArrayMap<String, String> values = mTables.get(table[0]);
            if (table[1] != null) {
                String value = get(table[0], table[1]);
                cursor = new MatrixCursor(columns, 1);
                if (value != null) {
                    addRow(cursor, values.indexOfKey(table[1]), table[1], value);
                }
                return cursor;
            }
            cursor = new MatrixCursor(columns, values.size());
            for (int i = 0; i < values.size(); i++) {
                try {
                    enforceReadable(table[0], values.keyAt(i));
                } catch (SecurityException e) {
                    continue;
                }
                addRow(cursor, i, values.keyAt(i), values.valueAt(i));
            }
        }
        return cursor;
    }

    /** As SettingsProvider's appendSettingToCursor: a row of the projection's columns. */
    private static void addRow(MatrixCursor cursor, int id, String name, String value) {
        String[] row = new String[cursor.getColumnCount()];
        for (int i = 0; i < row.length; i++) {
            switch (cursor.getColumnName(i)) {
                case Settings.NameValueTable._ID:
                    row[i] = Integer.toString(id);
                    break;
                case Settings.NameValueTable.NAME:
                    row[i] = name;
                    break;
                case Settings.NameValueTable.VALUE:
                    row[i] = value;
                    break;
                case Settings.NameValueTable.IS_PRESERVED_IN_RESTORE:
                    row[i] = "false";
                    break;
            }
        }
        cursor.addRow(row);
    }

    private static String[] normalizeProjection(String[] projection) {
        if (projection == null) {
            return ALL_COLUMNS;
        }
        for (String column : projection) {
            if (!ArrayUtils.contains(ALL_COLUMNS, column)) {
                throw new IllegalArgumentException("Invalid column: " + column);
            }
        }
        return projection;
    }

    @Override
    public String getType(Uri uri) {
        String[] table = arguments(uri, null, null, true);
        return (TextUtils.isEmpty(table[1]) ? "vnd.android.cursor.dir/" : "vnd.android.cursor.item/") + table[0];
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        String table = validTable(uri);
        String name = values.getAsString(Settings.NameValueTable.NAME);
        if (TextUtils.isEmpty(name)) {
            return null;
        }
        put(table, name, values.getAsString(Settings.NameValueTable.VALUE));
        return Uri.withAppendedPath(uri, name);
    }

    @Override
    public int update(Uri uri, ContentValues values, String where, String[] whereArgs) {
        String[] table = arguments(uri, where, whereArgs, false);
        String name = values.getAsString(Settings.NameValueTable.NAME);
        if (TextUtils.isEmpty(name) || !name.equals(table[1])) {
            return 0;
        }
        put(table[0], name, values.getAsString(Settings.NameValueTable.VALUE));
        return 1;
    }

    @Override
    public int delete(Uri uri, String where, String[] whereArgs) {
        String[] table = arguments(uri, where, whereArgs, false);
        return TextUtils.isEmpty(table[1]) ? 0 : remove(table[0], table[1]) ? 1 : 0;
    }

    /** A key's value, checked as SettingsProvider's getters check it. */
    private String get(String table, String name) {
        enforceReadable(table, name);
        synchronized (mTables) {
            return mTables.get(table).get(name);
        }
    }

    private void put(String table, String name, String value) {
        enforceWritable(table);
        synchronized (mTables) {
            mTables.get(table).put(name, value);
        }
    }

    private boolean remove(String table, String name) {
        enforceWritable(table);
        synchronized (mTables) {
            return mTables.get(table).remove(name) != null;
        }
    }

    /** As SettingsProvider's enforceSettingReadable for an app that is neither a system app nor test-only. */
    private void enforceReadable(String table, String name) {
        Readability readability;
        synchronized (mReadability) {
            readability = mReadability.get(table);
            if (readability == null) {
                readability = new Readability();
                switch (table) {
                    case TABLE_SYSTEM:
                        Settings.System.getPublicSettings(readability.all, readability.readable,
                                readability.maxTargetSdk);
                        break;
                    case TABLE_SECURE:
                        Settings.Secure.getPublicSettings(readability.all, readability.readable,
                                readability.maxTargetSdk);
                        break;
                    default:
                        Settings.Global.getPublicSettings(readability.all, readability.readable,
                                readability.maxTargetSdk);
                        break;
                }
                mReadability.put(table, readability);
            }
        }
        if (!readability.all.contains(name)) {
            return;
        }
        if (!readability.readable.contains(name)) {
            throw new SecurityException("Settings key: <" + name + "> is not readable. From S+, settings keys "
                    + "annotated with @hide are restricted to system_server and system apps only, unless they are "
                    + "annotated with @Readable.");
        }
        Integer maxTargetSdk = readability.maxTargetSdk.get(name);
        if (maxTargetSdk != null && getContext().getApplicationInfo().targetSdkVersion > maxTargetSdk) {
            throw new SecurityException("Settings key: <" + name + "> is only readable to apps with "
                    + "targetSdkVersion lower than or equal to: " + maxTargetSdk);
        }
    }

    /**
     * As SettingsProvider's openFile: a write needs WRITE_SETTINGS (its app op, the SecurityException without it), and
     * only the three cached sounds are files, each the copy of its sound made when the sound was set. No sound is set
     * here (SettingsDefaults has none, and decision 6 grants no app WRITE_SETTINGS), so none has its copy: the
     * FileNotFoundException that ParcelFileDescriptor.open throws for the missing file.
     */
    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (mode.contains("w")) {
            Context context = getContext();
            Settings.checkAndNoteWriteSettingsOperation(context, Binder.getCallingUid(), context.getOpPackageName(),
                    null, true);
        }
        uri = ContentProvider.getUriWithoutUserId(uri);
        if (!Settings.System.RINGTONE_CACHE_URI.equals(uri)
                && !Settings.System.NOTIFICATION_SOUND_CACHE_URI.equals(uri)
                && !Settings.System.ALARM_ALERT_CACHE_URI.equals(uri)) {
            throw new FileNotFoundException("Direct file access no longer supported; "
                    + "ringtone playback is available through android.media.Ringtone");
        }
        throw new FileNotFoundException("open failed: ENOENT (No such file or directory)");
    }

    /** As SettingsProvider's mutate*Setting: a table's write permission, or its SecurityException. */
    private void enforceWritable(String table) {
        Context context = getContext();
        if (context.checkCallingOrSelfPermission(Manifest.permission.WRITE_SECURE_SETTINGS)
                == PackageManager.PERMISSION_GRANTED) {
            return;
        }
        if (TABLE_SYSTEM.equals(table)) {
            // WRITE_SETTINGS through its app op; it throws without the permission.
            Settings.checkAndNoteWriteSettingsOperation(context, Binder.getCallingUid(), context.getOpPackageName(),
                    null, true);
            return;
        }
        throw new SecurityException("Permission denial, must have one of: "
                + Arrays.toString(new String[] {Manifest.permission.WRITE_SECURE_SETTINGS}));
    }

    /**
     * As SettingsProvider's Arguments: the table and the key a URI and a where clause name, the key null for the whole
     * table where {@code all} allows it, the table that of a key moved to another.
     */
    private static String[] arguments(Uri uri, String where, String[] whereArgs, boolean all) {
        int segments = uri.getPathSegments().size();
        if (segments == 1) {
            if (where != null && (WHERE_PATTERN_WITH_PARAM_NO_BRACKETS.matcher(where).matches()
                    || WHERE_PATTERN_WITH_PARAM_IN_BRACKETS.matcher(where).matches())
                    && whereArgs.length == 1) {
                return new String[] {tableFor(uri, whereArgs[0]), whereArgs[0]};
            } else if (where != null && (WHERE_PATTERN_NO_PARAM_NO_BRACKETS.matcher(where).matches()
                    || WHERE_PATTERN_NO_PARAM_IN_BRACKETS.matcher(where).matches())) {
                int start = Math.max(where.indexOf("'"), where.indexOf("\"")) + 1;
                int end = Math.max(where.lastIndexOf("'"), where.lastIndexOf("\""));
                String name = where.substring(start, end);
                return new String[] {tableFor(uri, name), name};
            } else if (all && where == null && whereArgs == null) {
                return new String[] {validTable(uri), null};
            }
        } else if (segments == 2 && where == null && whereArgs == null) {
            String name = uri.getPathSegments().get(1);
            return new String[] {tableFor(uri, name), name};
        }
        throw new IllegalArgumentException(String.format("Supported SQL:\n"
                + "  uri content://some_table/some_property with null where and where args\n"
                + "  uri content://some_table with query name=? and single name as arg\n"
                + "  uri content://some_table with query name=some_name and null args\n"
                + "  but got - uri:%1s, where:%2s whereArgs:%3s", uri, where, Arrays.toString(whereArgs)));
    }

    /** As SettingsProvider's computeTableForSetting: the table a key moved to, else the URI's. */
    private static String tableFor(Uri uri, String name) {
        String table = validTable(uri);
        Set<String> moved = new ArraySet<>();
        Settings.System.getMovedToSecureSettings(moved);
        Settings.Global.getMovedToSecureSettings(moved);
        if (moved.contains(name)) {
            table = TABLE_SECURE;
        }
        moved.clear();
        Settings.System.getMovedToGlobalSettings(moved);
        Settings.Secure.getMovedToGlobalSettings(moved);
        if (moved.contains(name)) {
            table = TABLE_GLOBAL;
        }
        moved.clear();
        Settings.Global.getMovedToSystemSettings(moved);
        if (moved.contains(name)) {
            table = TABLE_SYSTEM;
        }
        return table;
    }

    /** As SettingsProvider's getValidTableOrThrow, without the legacy tables Minato's provider never had. */
    private static String validTable(Uri uri) {
        if (uri.getPathSegments().isEmpty()) {
            throw new IllegalArgumentException("Invalid URI:" + uri);
        }
        String table = uri.getPathSegments().get(0);
        if (!TABLE_SYSTEM.equals(table) && !TABLE_SECURE.equals(table) && !TABLE_GLOBAL.equals(table)) {
            throw new IllegalArgumentException("Bad root path: " + table);
        }
        return table;
    }
}
