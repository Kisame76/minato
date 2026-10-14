package io.github.kisame76.minato.test.components;

import android.app.Activity;
import android.app.ProgressDialog;
import android.app.SearchManager;
import android.appwidget.AppWidgetManager;
import android.content.ComponentName;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.os.SystemClock;
import android.provider.DocumentsContract;
import android.provider.SearchRecentSuggestions;
import android.util.Log;
import java.io.FileInputStream;
import java.util.Arrays;

/**
 * The test of framework breadth batch F4a: the components an app declares in its manifest (a DocumentsProvider and a
 * SearchRecentSuggestionsProvider made at its start, an AppWidgetProvider, a DelegatedAdminReceiver, a
 * RestrictionsReceiver and a CameraPrewarmService) and the dialog and search classes that come with them, as an app
 * uses them. It runs on the main thread when the window first has the focus, part after part with a pause, and logs
 * under the tag f4a-components-test; tools/android-reference.sh takes the log from Android 15, and
 * tools/f4a-components-check.sh compares Minato's with it.
 */
public class ComponentsActivity extends Activity {
    static final String TAG = "f4a-components-test";
    private static final String DOCS = "io.github.kisame76.minato.test.components.docs";
    private static final int SETTLE_MS = 700;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mStarted;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private interface Part {
        void run() throws Exception;
    }

    private static void part(String name, Part part) {
        try {
            part.run();
        } catch (Throwable t) {
            StringBuilder line = new StringBuilder(name);
            for (Throwable c = t; c != null; c = c.getCause()) {
                line.append(": ").append(c.getClass().getName()).append(": ").append(c.getMessage());
            }
            log(line.toString());
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus || mStarted) {
            return;
        }
        mStarted = true;
        final Runnable[] steps = {
            () -> part("documentsProvider", this::documentsProvider),
            () -> part("documentsCalls", this::documentsCalls),
            () -> part("recentSuggestions", this::recentSuggestions),
            () -> part("appWidgetProvider", this::appWidgetProvider),
            () -> part("remoteCollection", this::remoteCollection),
            () -> part("declaredClasses", this::declaredClasses),
            () -> part("progressDialog", this::progressDialog),
            () -> part("searchableInfo", this::searchableInfo),
            () -> {
                log("done");
                finish();
            },
        };
        final int[] index = {0};
        Runnable next = new Runnable() {
            @Override
            public void run() {
                steps[index[0]++].run();
                if (index[0] < steps.length) {
                    mHandler.postDelayed(this, SETTLE_MS);
                }
            }
        };
        mHandler.post(next);
    }

    private static String columns(Cursor cursor) {
        String[] names = cursor.getColumnNames();
        Arrays.sort(names);
        return Arrays.toString(names);
    }

    private void documentsProvider() throws Exception {
        ContentResolver resolver = getContentResolver();
        Uri roots = DocumentsContract.buildRootsUri(DOCS);
        try (Cursor cursor = resolver.query(roots, null, null, null, null)) {
            cursor.moveToFirst();
            log("documents roots: count=" + cursor.getCount() + " id="
                    + cursor.getString(cursor.getColumnIndex(DocumentsContract.Root.COLUMN_ROOT_ID)) + " title="
                    + cursor.getString(cursor.getColumnIndex(DocumentsContract.Root.COLUMN_TITLE)));
        }
        Uri dir = DocumentsContract.buildDocumentUri(DOCS, "dir");
        try (Cursor cursor = resolver.query(dir, null, null, null, null)) {
            cursor.moveToFirst();
            log("documents folder: " + cursor.getString(cursor.getColumnIndex(
                    DocumentsContract.Document.COLUMN_DISPLAY_NAME)) + " type=" + resolver.getType(dir));
        }
        Uri children = DocumentsContract.buildChildDocumentsUri(DOCS, "dir");
        try (Cursor cursor = resolver.query(children, null, null, null, null)) {
            cursor.moveToFirst();
            log("documents children: count=" + cursor.getCount() + " first=" + cursor.getString(cursor
                    .getColumnIndex(DocumentsContract.Document.COLUMN_DISPLAY_NAME)) + " columns=" + columns(cursor));
        }
        Uri note = DocumentsContract.buildDocumentUri(DOCS, "dir/note.txt");
        log("documents note: type=" + resolver.getType(note) + " isDocumentUri="
                + DocumentsContract.isDocumentUri(this, note) + " id=" + DocumentsContract.getDocumentId(note));
        try (ParcelFileDescriptor fd = resolver.openFileDescriptor(note, "r");
                FileInputStream in = new FileInputStream(fd.getFileDescriptor())) {
            byte[] bytes = new byte[16];
            int read = in.read(bytes);
            log("documents read: " + new String(bytes, 0, read));
        }
        try (Cursor cursor = resolver.query(DocumentsContract.buildDocumentUri(DOCS, "missing"), null, null, null,
                null)) {
            log("documents missing: " + (cursor == null ? "null" : "cursor"));
        } catch (RuntimeException e) {
            log("documents missing: " + e.getClass().getName());
        }
    }

    private void documentsCalls() throws Exception {
        ContentResolver resolver = getContentResolver();
        Uri dir = DocumentsContract.buildDocumentUri(DOCS, "dir");
        Uri note = DocumentsContract.buildDocumentUri(DOCS, "dir/note.txt");
        try {
            Uri created = DocumentsContract.createDocument(resolver, dir, "text/plain", "new.txt");
            log("documents create: " + created);
        } catch (Exception e) {
            log("documents create: " + e.getClass().getName() + ": " + e.getMessage());
        }
        try {
            boolean deleted = DocumentsContract.deleteDocument(resolver, note);
            log("documents delete: " + deleted);
        } catch (Exception e) {
            log("documents delete: " + e.getClass().getName() + ": " + e.getMessage());
        }
        try {
            DocumentsContract.Path path = DocumentsContract.findDocumentPath(resolver, note);
            log("documents path: " + (path == null ? "null" : path.getPath()));
        } catch (Exception e) {
            // The message names the app's uid, which differs between runs and devices.
            log("documents path: " + e.getClass().getName());
        }
        log("documents ischild: " + DocumentsContract.isChildDocument(resolver, dir, note));
    }

    private void recentSuggestions() throws Exception {
        SearchRecentSuggestions suggestions = new SearchRecentSuggestions(this, SuggestProvider.AUTHORITY,
                SuggestProvider.DATABASE_MODE_QUERIES);
        suggestions.saveRecentQuery("hello", null);
        Uri uri = Uri.parse("content://" + SuggestProvider.AUTHORITY + "/" + SearchManager.SUGGEST_URI_PATH_QUERY);
        int count = 0;
        String first = null;
        String cols = null;
        for (int tries = 0; tries < 40 && count == 0; tries++) {
            try (Cursor cursor = getContentResolver().query(uri, null, null, new String[] {"hel"}, null)) {
                count = cursor.getCount();
                cols = columns(cursor);
                if (count > 0) {
                    cursor.moveToFirst();
                    first = cursor.getString(cursor.getColumnIndex(SearchManager.SUGGEST_COLUMN_TEXT_1));
                }
            }
            if (count == 0) {
                SystemClock.sleep(50);
            }
        }
        log("recent suggestions: count=" + count + " first=" + first + " columns=" + cols);
        suggestions.clearHistory();
        SystemClock.sleep(300);
        try (Cursor cursor = getContentResolver().query(uri, null, null, new String[] {"hel"}, null)) {
            log("recent suggestions cleared: count=" + cursor.getCount());
        }
    }

    private void appWidgetProvider() {
        AppWidgetManager manager = AppWidgetManager.getInstance(this);
        ComponentName provider = new ComponentName(this, WidgetReceiver.class);
        log("appWidget manager: " + (manager != null) + " ids=" + manager.getAppWidgetIds(provider).length
                + " same=" + (manager == getSystemService(AppWidgetManager.class)));
        Intent update = new Intent(AppWidgetManager.ACTION_APPWIDGET_UPDATE).setComponent(provider)
                .putExtra(AppWidgetManager.EXTRA_APPWIDGET_IDS, new int[] {7, 9});
        // Minato delivers no broadcast to a manifest receiver yet (App's receiver step), so the provider is called as
        // the system's broadcast would: onReceive with the update and then with the enable action.
        WidgetReceiver receiver = new WidgetReceiver();
        receiver.onReceive(this, update);
        receiver.onReceive(this, new Intent(AppWidgetManager.ACTION_APPWIDGET_ENABLED).setComponent(provider));
        log("appWidget update heard: " + WidgetReceiver.HEARD);
    }

    /**
     * RemoteViewsAdapter binds a collection's service through the app widget manager's hidden call. The app targets
     * API 30, the last level at which Android lets an app reflect over it. Android's service refuses a widget id it
     * does not know with an IllegalArgumentException, where Minato has no service and the manager answers false as
     * AOSP's does without one; both mean that nothing was bound, which is what the log says.
     */
    private void remoteCollection() throws Exception {
        java.lang.reflect.Method bind = AppWidgetManager.class.getMethod("bindRemoteViewsService",
                android.content.Context.class, int.class, Intent.class, Class.forName("android.app.IServiceConnection"),
                int.class);
        boolean bound;
        try {
            bound = (Boolean) bind.invoke(AppWidgetManager.getInstance(this), this, 1, new Intent(this,
                    PrewarmService.class), null, 0);
        } catch (java.lang.reflect.InvocationTargetException e) {
            if (!(e.getCause() instanceof IllegalArgumentException)) {
                throw e;
            }
            bound = false;
        }
        log("remoteCollection: bound=" + bound);
    }

    private void declaredClasses() throws Exception {
        String[] names = {"android.app.admin.DelegatedAdminReceiver",
            "android.service.restrictions.RestrictionsReceiver", "android.service.media.CameraPrewarmService",
            "android.app.ZygotePreload",
            "android.appwidget.AppWidgetProviderInfo", "android.app.SearchableInfo"};
        for (String name : names) {
            Class<?> type = Class.forName(name);
            log("declared " + name + ": super=" + type.getSuperclass() + " interface=" + type.isInterface());
        }
        log("declared admin: " + (new AdminReceiver() instanceof android.content.BroadcastReceiver) + " restrict="
                + (new RestrictReceiver() instanceof android.content.BroadcastReceiver) + " prewarm="
                + (new PrewarmService() instanceof android.app.Service));
    }

    private void progressDialog() {
        ProgressDialog dialog = new ProgressDialog(this);
        dialog.setProgressStyle(ProgressDialog.STYLE_HORIZONTAL);
        dialog.setMessage("loading");
        dialog.setIndeterminate(false);
        dialog.setMax(100);
        dialog.setProgress(40);
        dialog.show();
        dialog.incrementProgressBy(10);
        log("progressDialog: showing=" + dialog.isShowing() + " progress=" + dialog.getProgress() + " max="
                + dialog.getMax() + " indeterminate=" + dialog.isIndeterminate());
        dialog.dismiss();
        log("progressDialog dismissed: showing=" + dialog.isShowing());
    }

    private void searchableInfo() {
        SearchManager manager = (SearchManager) getSystemService(SEARCH_SERVICE);
        log("searchableInfo: manager=" + (manager != null) + " info="
                + (manager.getSearchableInfo(new ComponentName(this, ComponentsActivity.class)) != null));
    }
}
