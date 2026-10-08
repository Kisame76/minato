package io.github.kisame76.minato.test.sqlite;

import android.app.Activity;
import android.content.ContentValues;
import android.content.Context;
import android.database.AbstractWindowedCursor;
import android.database.Cursor;
import android.database.CursorWindow;
import android.database.DatabaseUtils;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteDoneException;
import android.database.sqlite.SQLiteOpenHelper;
import android.database.sqlite.SQLiteStatement;
import android.os.Handler;
import android.os.Looper;
import android.os.Parcel;
import android.util.Log;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.zip.CRC32;

/**
 * The compatibility plan's gap 7 test: an app's own SQLite databases through android.database.sqlite, on the main
 * thread, as Firebase's datatransport opens its event store there. Its parts run one after the other from the resumed
 * activity: SQLiteOpenHelper's create, configure and open with the defaults Android gives a new database, rows through
 * ContentValues and cursors, constraint errors, transactions, compiled statements and DatabaseUtils, Android's SQL
 * functions and collations, FTS, WAL, an upgrade, an in-memory database, a cursor window through a parcel, windows
 * past their 16 KB start (a big row, a result of more than one window, a big window through a parcel), one database
 * that the context opens itself, and the database files. Each part logs with the tag sqlite-test; a part that
 * throws logs "failed" and the next one runs, so that every run reaches "done". A failure's stack has the tag
 * sqlite-test-device.
 */
public class SqliteActivity extends Activity {
    static final String TAG = "sqlite-test";
    static final String NAME = "app.db";

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mStarted;
    private Helper mHelper;

    private interface Part {
        void run();
    }

    /** The app's database, version 1 with one table, version 2 with a column more. */
    static final class Helper extends SQLiteOpenHelper {
        Helper(Context context, int version) {
            super(context, NAME, null, version);
        }

        @Override
        public void onConfigure(SQLiteDatabase db) {
            Log.i(TAG, "configure: version " + db.getVersion() + ", in transaction " + db.inTransaction());
        }

        @Override
        public void onCreate(SQLiteDatabase db) {
            Log.i(TAG, "create: version " + db.getVersion() + ", in transaction " + db.inTransaction());
            db.execSQL("CREATE TABLE items (_id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL UNIQUE, "
                    + "count INTEGER DEFAULT 0, data BLOB)");
        }

        @Override
        public void onUpgrade(SQLiteDatabase db, int oldVersion, int newVersion) {
            Log.i(TAG, "upgrade: " + oldVersion + " to " + newVersion);
            db.execSQL("ALTER TABLE items ADD COLUMN note TEXT");
        }

        @Override
        public void onOpen(SQLiteDatabase db) {
            Log.i(TAG, "open: version " + db.getVersion() + ", in transaction " + db.inTransaction());
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (!mStarted) {
            mStarted = true;
            mHandler.post(this::runParts);
        }
    }

    private void runParts() {
        List<Part> parts = Arrays.asList(this::begin, this::helper, this::rows, this::constraints, this::transactions,
                this::statements, this::functions, this::fts, this::wal, this::upgrade, this::memory, this::parcel,
                this::bigRow, this::bigResult, this::bigParcel, this::opened, this::removal);
        for (Part part : parts) {
            try {
                part.run();
            } catch (RuntimeException | LinkageError e) {
                Log.i(TAG, "failed: " + describe(e));
                Log.i(TAG + "-device", Log.getStackTraceString(e));
            }
        }
        if (mHelper != null) mHelper.close();
        Log.i(TAG, "done");
    }

    static String describe(Throwable e) {
        return e.getClass().getName() + ": " + e.getMessage();
    }

    /** The first column of each row of `sql`, joined by spaces. */
    static String column(SQLiteDatabase db, String sql, String... args) {
        List<String> values = new ArrayList<>();
        try (Cursor cursor = db.rawQuery(sql, args)) {
            while (cursor.moveToNext()) values.add(cursor.isNull(0) ? "null" : cursor.getString(0));
        }
        return String.join(" ", values);
    }

    /** Where the database goes, and the app's databases before it. */
    private void begin() {
        Log.i(TAG, "path: " + getDatabasePath(NAME).getPath());
        files("before");
    }

    /** The app's database files, by name. */
    private void files(String when) {
        String[] names = databaseList();
        Arrays.sort(names);
        Log.i(TAG, "files " + when + ": " + Arrays.toString(names));
    }

    /** A new database from the helper on the main thread, with the defaults Android gives it. */
    private void helper() {
        mHelper = new Helper(this, 1);
        SQLiteDatabase db = mHelper.getWritableDatabase();
        Log.i(TAG, "database: version " + db.getVersion() + ", open " + db.isOpen() + ", read-only " + db.isReadOnly()
                + ", path " + db.getPath());
        Log.i(TAG, "sizes: page " + db.getPageSize() + ", maximum " + db.getMaximumSize());
        Log.i(TAG, "wal enabled: " + db.isWriteAheadLoggingEnabled());
        for (String pragma : new String[] {"journal_mode", "synchronous", "foreign_keys", "journal_size_limit",
                "auto_vacuum", "user_version", "page_size", "secure_delete", "encoding", "cache_size",
                "wal_autocheckpoint", "locking_mode", "recursive_triggers"}) {
            Log.i(TAG, "pragma " + pragma + ": " + column(db, "PRAGMA " + pragma));
        }
        Log.i(TAG, "tables: " + column(db, "SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name"));
        Log.i(TAG, "metadata: " + column(db, "SELECT locale FROM android_metadata"));
    }

    /** Rows through ContentValues, queries with arguments, and the cursor's types. */
    private void rows() {
        SQLiteDatabase db = mHelper.getWritableDatabase();
        ContentValues values = new ContentValues();
        values.put("name", "apple");
        values.put("count", 3);
        values.put("data", new byte[] {1, 2, 3});
        Log.i(TAG, "insert apple: " + db.insert("items", null, values));
        values.clear();
        values.put("name", "Äpfel");
        Log.i(TAG, "insert Äpfel: " + db.insert("items", null, values));
        values.clear();
        values.put("name", "zebra");
        values.put("count", 7);
        Log.i(TAG, "insert zebra: " + db.insertOrThrow("items", null, values));
        values.clear();
        values.put("name", "apple");
        values.put("count", 9);
        Log.i(TAG, "insert apple again: " + db.insert("items", null, values));
        Log.i(TAG, "replace apple: " + db.insertWithOnConflict("items", null, values, SQLiteDatabase.CONFLICT_REPLACE));
        Log.i(TAG, "ignore apple: " + db.insertWithOnConflict("items", null, values, SQLiteDatabase.CONFLICT_IGNORE));
        values.clear();
        values.put("count", 5);
        Log.i(TAG, "update: " + db.update("items", values, "count < ?", new String[] {"8"}));
        Log.i(TAG, "delete: " + db.delete("items", "name = ?", new String[] {"nothing"}));
        try (Cursor cursor = db.query("items", null, "count >= ?", new String[] {"0"}, null, null, "_id")) {
            Log.i(TAG, "columns: " + String.join(" ", cursor.getColumnNames()) + ", rows " + cursor.getCount());
            while (cursor.moveToNext()) {
                StringBuilder row = new StringBuilder();
                for (int i = 0; i < cursor.getColumnCount(); i++) {
                    int type = cursor.getType(i);
                    row.append(i > 0 ? " " : "").append(type).append(':').append(type == Cursor.FIELD_TYPE_BLOB
                            ? Arrays.toString(cursor.getBlob(i)) : cursor.getString(i));
                }
                Log.i(TAG, "row " + cursor.getPosition() + ": " + row);
            }
        }
    }

    /** The exceptions of constraints and of bad SQL, with their messages. */
    private void constraints() {
        SQLiteDatabase db = mHelper.getWritableDatabase();
        ContentValues values = new ContentValues();
        values.put("name", "zebra");
        try {
            db.insertOrThrow("items", null, values);
        } catch (RuntimeException e) {
            Log.i(TAG, "unique: " + describe(e));
        }
        try {
            db.execSQL("INSERT INTO items (name) VALUES (NULL)");
        } catch (RuntimeException e) {
            Log.i(TAG, "not null: " + describe(e));
        }
        try {
            db.rawQuery("SELECT nothing FROM items", null).close();
        } catch (RuntimeException e) {
            Log.i(TAG, "no column: " + describe(e));
        }
    }

    /** A transaction that ends without success rolls back; one marked successful commits. */
    private void transactions() {
        SQLiteDatabase db = mHelper.getWritableDatabase();
        ContentValues values = new ContentValues();
        db.beginTransaction();
        try {
            values.put("name", "rolled back");
            db.insert("items", null, values);
            Log.i(TAG, "in transaction: " + db.inTransaction() + ", rows "
                    + DatabaseUtils.queryNumEntries(db, "items"));
        } finally {
            db.endTransaction();
        }
        Log.i(TAG, "after rollback: " + DatabaseUtils.queryNumEntries(db, "items"));
        db.beginTransactionNonExclusive();
        try {
            values.put("name", "committed");
            db.insert("items", null, values);
            db.setTransactionSuccessful();
        } finally {
            db.endTransaction();
        }
        Log.i(TAG, "after commit: " + DatabaseUtils.queryNumEntries(db, "items") + ", in transaction "
                + db.inTransaction());
    }

    /** Compiled statements with bound arguments, and DatabaseUtils' helpers. */
    private void statements() {
        SQLiteDatabase db = mHelper.getWritableDatabase();
        try (SQLiteStatement insert = db.compileStatement("INSERT INTO items (name, count) VALUES (?, ?)")) {
            insert.bindString(1, "statement");
            insert.bindLong(2, 11);
            Log.i(TAG, "statement insert: " + insert.executeInsert());
        }
        try (SQLiteStatement update = db.compileStatement("UPDATE items SET count = count + 1 WHERE count > ?")) {
            update.bindLong(1, 6);
            Log.i(TAG, "statement update: " + update.executeUpdateDelete());
        }
        try (SQLiteStatement count = db.compileStatement("SELECT count(*) FROM items")) {
            Log.i(TAG, "simple long: " + count.simpleQueryForLong());
        }
        try (SQLiteStatement name = db.compileStatement("SELECT name FROM items WHERE count = ?")) {
            name.bindLong(1, 12);
            Log.i(TAG, "simple string: " + name.simpleQueryForString());
        }
        Log.i(TAG, "long for query: " + DatabaseUtils.longForQuery(db, "SELECT max(count) FROM items", null));
        Log.i(TAG, "string for query: " + DatabaseUtils.stringForQuery(db, "SELECT name FROM items WHERE _id = ?",
                new String[] {"2"}));
        try {
            DatabaseUtils.stringForQuery(db, "SELECT name FROM items WHERE _id = ?", new String[] {"1"});
        } catch (SQLiteDoneException e) {
            Log.i(TAG, "no row: " + describe(e));
        }
        Log.i(TAG, "entries: " + DatabaseUtils.queryNumEntries(db, "items", "count > ?", new String[] {"4"}));
        Log.i(TAG, "escaped: " + DatabaseUtils.sqlEscapeString("it's"));
    }

    /** SQLite's version, ICU, Android's phone numbers and the collations each connection registers. */
    private void functions() {
        SQLiteDatabase db = mHelper.getWritableDatabase();
        Log.i(TAG, "version: " + column(db, "SELECT sqlite_version()"));
        Log.i(TAG, "upper: " + column(db, "SELECT upper('straße')"));
        Log.i(TAG, "phone numbers: " + column(db, "SELECT PHONE_NUMBERS_EQUAL('+1 650-555-1234', '650 555 1234')"));
        Log.i(TAG, "binary order: " + column(db, "SELECT name FROM items ORDER BY name"));
        Log.i(TAG, "localized order: " + column(db, "SELECT name FROM items ORDER BY name COLLATE LOCALIZED"));
        Log.i(TAG, "unicode order: " + column(db, "SELECT name FROM items ORDER BY name COLLATE UNICODE"));
    }

    /** FTS4, which Android builds in, and FTS5, which it does not. */
    private void fts() {
        SQLiteDatabase db = mHelper.getWritableDatabase();
        db.execSQL("CREATE VIRTUAL TABLE docs USING fts4(body)");
        db.execSQL("INSERT INTO docs (body) VALUES ('the quick brown fox'), ('lazy dogs sleep'), ('quick thinking')");
        Log.i(TAG, "fts4: " + column(db, "SELECT rowid FROM docs WHERE docs MATCH 'quick' ORDER BY rowid"));
        try {
            db.execSQL("CREATE VIRTUAL TABLE docs5 USING fts5(body)");
            Log.i(TAG, "fts5: created");
        } catch (RuntimeException e) {
            Log.i(TAG, "fts5: " + describe(e));
        }
    }

    /** Write-ahead logging switched on and off. */
    private void wal() {
        SQLiteDatabase db = mHelper.getWritableDatabase();
        Log.i(TAG, "enable wal: " + db.enableWriteAheadLogging() + ", enabled " + db.isWriteAheadLoggingEnabled()
                + ", mode " + column(db, "PRAGMA journal_mode"));
        ContentValues values = new ContentValues();
        values.put("name", "in wal");
        Log.i(TAG, "wal insert: " + db.insert("items", null, values));
        db.disableWriteAheadLogging();
        Log.i(TAG, "disable wal: enabled " + db.isWriteAheadLoggingEnabled() + ", mode "
                + column(db, "PRAGMA journal_mode"));
    }

    /** The helper of version 2 upgrades the database. */
    private void upgrade() {
        mHelper.close();
        mHelper = new Helper(this, 2);
        SQLiteDatabase db = mHelper.getWritableDatabase();
        try (Cursor cursor = db.rawQuery("SELECT * FROM items LIMIT 1", null)) {
            Log.i(TAG, "upgraded: version " + db.getVersion() + ", columns "
                    + String.join(" ", cursor.getColumnNames()));
        }
        SQLiteDatabase read = mHelper.getReadableDatabase();
        Log.i(TAG, "readable: same " + (read == db) + ", read-only " + read.isReadOnly());
    }

    /** A database in memory, which has no file. */
    private void memory() {
        SQLiteDatabase db = SQLiteDatabase.create(null);
        try {
            db.execSQL("CREATE TABLE t (x)");
            db.execSQL("INSERT INTO t VALUES (1), (2)");
            Log.i(TAG, "memory: path " + db.getPath() + ", sum " + column(db, "SELECT sum(x) FROM t") + ", mode "
                    + column(db, "PRAGMA journal_mode"));
        } finally {
            db.close();
        }
    }

    /**
     * A cursor window through a parcel within the process, as a provider's cursor goes to another process: the
     * parcel's bytes, then the copy that CREATOR reads back from them. The string with its NUL and the blob are 4 bytes
     * each, so the window's 4-byte alignment leaves no unwritten padding in the bytes.
     */
    private void parcel() {
        CursorWindow window = new CursorWindow("parcel");
        Parcel parcel = Parcel.obtain();
        try {
            window.setNumColumns(2);
            window.allocRow();
            window.putLong(7, 0, 0);
            window.putString("sev", 0, 1);
            window.allocRow();
            window.putNull(1, 0);
            window.putBlob(new byte[] {1, 2, 3, 4}, 1, 1);
            window.writeToParcel(parcel, 0);
            byte[] bytes = parcel.marshall();
            StringBuilder hex = new StringBuilder();
            for (byte b : bytes) hex.append(String.format("%02x", b & 0xff));
            Log.i(TAG, "parcel: " + bytes.length + " bytes " + hex);
            parcel.setDataPosition(0);
            CursorWindow copy = CursorWindow.CREATOR.createFromParcel(parcel);
            try {
                Log.i(TAG, "parcel copy: start " + copy.getStartPosition() + ", rows " + copy.getNumRows() + ", "
                        + copy.getLong(0, 0) + " " + copy.getString(0, 1) + " " + copy.getType(1, 0) + " "
                        + Arrays.toString(copy.getBlob(1, 1)));
            } finally {
                copy.close();
            }
        } finally {
            parcel.recycle();
            window.close();
        }
    }

    /** `length` bytes that differ from their neighbours, so a shifted copy shows. */
    static byte[] pattern(int length) {
        byte[] bytes = new byte[length];
        for (int i = 0; i < length; i++) bytes[i] = (byte) (i * 31 + 7);
        return bytes;
    }

    static long crc(byte[] bytes) {
        CRC32 crc = new CRC32();
        crc.update(bytes);
        return crc.getValue();
    }

    /**
     * A row of 20,000 bytes, more than a cursor window's 16 KB inline start: CursorWindow inflates the window to an
     * ashmem region of 2 MB (memfd on Android 15), mapped shared and then made read-only for later maps.
     */
    private void bigRow() {
        SQLiteDatabase db = SQLiteDatabase.create(null);
        try {
            db.execSQL("CREATE TABLE big (data BLOB)");
            byte[] row = pattern(20000);
            ContentValues values = new ContentValues();
            values.put("data", row);
            db.insert("big", null, values);
            try (Cursor cursor = db.rawQuery("SELECT data FROM big", null)) {
                cursor.moveToFirst();
                byte[] back = cursor.getBlob(0);
                Log.i(TAG, "big row: " + back.length + " bytes, crc " + crc(back) + ", same "
                        + Arrays.equals(row, back));
            }
        } finally {
            db.close();
        }
    }

    /**
     * A result of 40 rows of 64,000 bytes, more than one window of 2 MB holds: the cursor fills a new window from the
     * start position SQLiteCursor picks each time it leaves the last one. Each row's bytes are its number.
     */
    private void bigResult() {
        SQLiteDatabase db = SQLiteDatabase.create(null);
        try {
            db.execSQL("CREATE TABLE big (n INTEGER, data BLOB)");
            db.execSQL("WITH RECURSIVE k(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM k WHERE n < 40) "
                    + "INSERT INTO big SELECT n, unhex(replace(hex(zeroblob(64000)), '00', printf('%02x', n))) FROM k");
            try (Cursor cursor = db.rawQuery("SELECT n, data FROM big ORDER BY n", null)) {
                List<Integer> starts = new ArrayList<>();
                CRC32 crc = new CRC32();
                long bytes = 0;
                while (cursor.moveToNext()) {
                    int start = ((AbstractWindowedCursor) cursor).getWindow().getStartPosition();
                    if (starts.isEmpty() || starts.get(starts.size() - 1) != start) starts.add(start);
                    byte[] data = cursor.getBlob(1);
                    bytes += data.length;
                    crc.update(data);
                }
                Log.i(TAG, "big result: " + cursor.getCount() + " rows, " + bytes + " bytes, crc " + crc.getValue()
                        + ", window starts " + starts);
            }
        } finally {
            db.close();
        }
    }

    /**
     * An inflated window through a parcel within the process: it goes as its ashmem region's file descriptor, which
     * the copy maps read-only.
     */
    private void bigParcel() {
        byte[] row = pattern(20000);
        CursorWindow window = new CursorWindow("big parcel");
        Parcel parcel = Parcel.obtain();
        try {
            window.setNumColumns(1);
            window.allocRow();
            Log.i(TAG, "big put: " + window.putBlob(row, 0, 0));
            window.writeToParcel(parcel, 0);
            Log.i(TAG, "big parcel: file descriptors " + parcel.hasFileDescriptors());
            parcel.setDataPosition(0);
            CursorWindow copy = CursorWindow.CREATOR.createFromParcel(parcel);
            try {
                byte[] back = copy.getBlob(0, 0);
                Log.i(TAG, "big parcel copy: start " + copy.getStartPosition() + ", rows " + copy.getNumRows() + ", "
                        + back.length + " bytes, same " + Arrays.equals(row, back));
            } finally {
                copy.close();
            }
        } finally {
            parcel.recycle();
            window.close();
        }
    }

    /** A database that the context opens itself, as apps did before SQLiteOpenHelper. */
    private void opened() {
        SQLiteDatabase db = openOrCreateDatabase("other.db", MODE_PRIVATE, null);
        try {
            db.execSQL("CREATE TABLE t (x)");
            db.execSQL("INSERT INTO t VALUES (5)");
            Log.i(TAG, "opened: path " + db.getPath() + ", version " + db.getVersion() + ", sum "
                    + column(db, "SELECT sum(x) FROM t") + ", mode " + column(db, "PRAGMA journal_mode"));
        } finally {
            db.close();
        }
        files("with other");
        Log.i(TAG, "delete other: " + deleteDatabase("other.db"));
    }

    /** The database's files, and deleteDatabase. */
    private void removal() {
        mHelper.close();
        files("after close");
        Log.i(TAG, "delete: " + deleteDatabase(NAME) + ", again " + deleteDatabase(NAME));
        files("after delete");
    }
}
