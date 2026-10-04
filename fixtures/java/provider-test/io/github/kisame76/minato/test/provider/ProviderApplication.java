package io.github.kisame76.minato.test.provider;

import android.app.Application;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.net.Uri;
import android.util.Log;
import java.io.File;

/**
 * ADR-0039 step c2c1's test: the app's components without the window. The factory makes the Application, then the
 * providers of the main process install, by initOrder, before onCreate, which asks for a provider's meta-data as
 * androidx.startup does and uses Intent and Uri as apps use them. It logs with the tag provider-test and ends with
 * "done"; tools/android-reference.sh compares it with Android 15's log.
 */
public class ProviderApplication extends Application {
    static final String TAG = "provider-test";
    static volatile ProviderApplication sApplication;

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        sApplication = this;
        Log.i(TAG, "application attachBaseContext");
    }

    @Override
    public void onCreate() {
        super.onCreate();
        Log.i(TAG, "application onCreate");
        try {
            metaData();
            intents();
            uris();
        } catch (Throwable t) {
            Log.i(TAG, "failed: " + t);
            // Its stack, which differs by device, is only shown.
            Log.i(TAG + "-device", Log.getStackTraceString(t));
        }
        Log.i(TAG, "done");
    }

    /** As androidx.startup's InitializationProvider reads its initializers. */
    private void metaData() throws PackageManager.NameNotFoundException {
        ProviderInfo info = getPackageManager().getProviderInfo(new ComponentName(this, SecondProvider.class),
                PackageManager.GET_META_DATA);
        Log.i(TAG, "getProviderInfo authority " + info.authority + ", meta-data "
                + info.metaData.getString("io.github.kisame76.minato.test.provider.KEY"));
    }

    private void intents() throws Exception {
        Intent intent = new Intent("io.github.kisame76.minato.test.provider.ACTION")
                .setClassName(getPackageName(), "io.github.kisame76.minato.test.provider.Target")
                .addCategory(Intent.CATEGORY_DEFAULT)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP)
                .setDataAndType(
                        Uri.parse("content://io.github.kisame76.minato.test.provider.first/items/7?sort=name#top"),
                        "vnd.android.cursor.item/test")
                .putExtra("text", "value")
                .putExtra("number", 7)
                .putExtra("flag", true)
                .putExtra("list", new String[] {"a", "b"});
        Log.i(TAG, "intent " + intent);
        Log.i(TAG, "component " + intent.getComponent().flattenToShortString() + ", flags 0x"
                + Integer.toHexString(intent.getFlags()) + ", categories " + intent.getCategories());
        String uri = intent.toUri(Intent.URI_INTENT_SCHEME);
        Log.i(TAG, "toUri " + uri);
        Intent parsed = Intent.parseUri(uri, Intent.URI_INTENT_SCHEME);
        Log.i(TAG, "parseUri filterEquals " + parsed.filterEquals(intent) + ", text " + parsed.getStringExtra("text")
                + ", number " + parsed.getIntExtra("number", 0) + ", flag " + parsed.getBooleanExtra("flag", false));
        Intent copy = (Intent) intent.clone();
        Log.i(TAG, "clone filterEquals " + copy.filterEquals(intent) + ", same filterHashCode "
                + (copy.filterHashCode() == intent.filterHashCode()) + ", extras " + copy.getExtras().size()
                + ", list " + String.join(",", copy.getStringArrayExtra("list")));
        Log.i(TAG, "toUri(0) " + intent.toUri(0));
        Log.i(TAG, "main " + Intent.makeMainActivity(new ComponentName(this, ProviderApplication.class)));
    }

    private void uris() {
        Uri web = Uri.parse("https://user@example.com:8080/a%20b/c?x=1&y=%C3%A4&x=2#frag");
        Log.i(TAG, "uri scheme " + web.getScheme() + ", authority " + web.getAuthority() + ", user " + web.getUserInfo()
                + ", host " + web.getHost() + ", port " + web.getPort());
        Log.i(TAG, "uri path " + web.getPath() + ", encoded " + web.getEncodedPath() + ", segments "
                + web.getPathSegments() + ", last " + web.getLastPathSegment());
        Log.i(TAG, "uri query " + web.getQuery() + ", y " + web.getQueryParameter("y") + ", x "
                + web.getQueryParameters("x") + ", names " + web.getQueryParameterNames() + ", fragment "
                + web.getFragment());
        Uri built = new Uri.Builder().scheme("content").authority("io.github.kisame76.minato.test.provider.first")
                .appendPath("a b").appendEncodedPath("c%2Fd").appendQueryParameter("k", "v w&z").fragment("f g")
                .build();
        Log.i(TAG, "built " + built + ", path " + built.getPath() + ", appended "
                + Uri.withAppendedPath(built, "42"));
        Log.i(TAG, "encode " + Uri.encode("a b/ä&") + ", decode " + Uri.decode("a%20b%2F%C3%A4"));
        Log.i(TAG, "normalizeScheme " + Uri.parse("HTTP://Example.com/").normalizeScheme() + ", opaque "
                + Uri.parse("mailto:someone@example.com").isOpaque() + ", relative " + Uri.parse("a/b").isRelative());
        Log.i(TAG, "fromFile " + Uri.fromFile(new File("/data/x y")));
    }
}
