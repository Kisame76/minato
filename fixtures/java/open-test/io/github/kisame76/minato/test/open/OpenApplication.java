package io.github.kisame76.minato.test.open;

import android.app.Application;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.net.Uri;
import android.util.Log;

import java.util.ArrayList;

/**
 * The intents that leave the app for iOS (ADR-0062), as an app that handles none of them meets them: whether each
 * resolves (resolveActivity and queryIntentActivities), and what startActivity throws for those that resolve to
 * nothing. The emulator has a browser, a dialer and messaging but no mail and no maps app, an iPhone has all of them,
 * so the cases where the two differ by design are logged under the device tag and only the others are compared. Nothing
 * is started that resolves: the app would leave the screen.
 */
public class OpenApplication extends Application {
    static final String TAG = "open-test";
    static final String DEVICE_TAG = "open-test-device";

    private PackageManager mPackages;

    private Intent view(String uri) {
        return new Intent(Intent.ACTION_VIEW, Uri.parse(uri));
    }

    private Intent send(String type) {
        Intent send = new Intent(Intent.ACTION_SEND).setType(type).putExtra(Intent.EXTRA_TEXT, "text");
        return send;
    }

    /** One line: whether the intent resolves and how many activities it queries, by the tag `tag`. */
    private void resolves(String tag, String name, Intent intent) {
        ResolveInfo resolved = mPackages.resolveActivity(intent, 0);
        int count = mPackages.queryIntentActivities(intent, 0).size();
        Log.i(tag, name + ": resolve " + (resolved != null) + ", query " + (count > 0));
    }

    /** What startActivity throws for an intent that resolves to nothing. */
    private void starts(String tag, String name, Intent intent) {
        String outcome;
        try {
            startActivity(intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            outcome = "started";
        } catch (ActivityNotFoundException e) {
            outcome = "ActivityNotFoundException: " + e.getMessage();
        } catch (RuntimeException e) {
            outcome = e.getClass().getName() + ": " + e.getMessage();
        }
        Log.i(tag, name + ": start " + outcome);
    }

    @Override
    public void onCreate() {
        super.onCreate();
        mPackages = getPackageManager();

        // What Android 15 and iOS both answer.
        resolves(TAG, "view http", view("http://example.com/"));
        resolves(TAG, "view https", view("https://example.com/path?q=1#frag"));
        resolves(TAG, "view tel", view("tel:+4930123456"));
        resolves(TAG, "dial tel", new Intent(Intent.ACTION_DIAL, Uri.parse("tel:123")));
        resolves(TAG, "view sms", view("sms:123"));
        resolves(TAG, "sendto smsto", new Intent(Intent.ACTION_SENDTO, Uri.parse("smsto:123")));
        resolves(TAG, "send text", send("text/plain"));
        resolves(TAG, "chooser of send text", Intent.createChooser(send("text/plain"), "Share"));
        Intent several = new Intent(Intent.ACTION_SEND_MULTIPLE).setType("image/*");
        several.putParcelableArrayListExtra(Intent.EXTRA_STREAM, new ArrayList<Uri>());
        resolves(TAG, "send multiple", several);

        // What neither answers, and what startActivity throws for it.
        String[] none = {"market://details?id=com.example", "file:///sdcard/a.txt",
                "javascript:alert(1)", "intent://x#Intent;scheme=http;end", "ftp://example.com/", "data:text/plain,x"};
        for (String uri : none) {
            resolves(TAG, "view " + uri, view(uri));
            // StrictMode refuses a file: URI in an intent with its own exception on Android, which ours has not.
            starts(uri.startsWith("file:") ? DEVICE_TAG : TAG, "view " + uri, view(uri));
        }
        Intent pinned = view("http://example.com/").setPackage("com.example.nowhere");
        resolves(TAG, "view http of a package that is not there", pinned);
        starts(TAG, "view http of a package that is not there", pinned);
        Intent named = view("http://example.com/").setClassName("com.example.nowhere", "com.example.nowhere.Main");
        resolves(TAG, "view http of a component that is not there", named);
        resolves(TAG, "send without a type", new Intent(Intent.ACTION_SEND).putExtra(Intent.EXTRA_TEXT, "text"));
        resolves(DEVICE_TAG, "view without data", new Intent(Intent.ACTION_VIEW));
        // The emulator's dialer opens without a number; iOS cannot open its phone app without one.
        resolves(DEVICE_TAG, "dial without data", new Intent(Intent.ACTION_DIAL));

        // What the emulator answers with an app of its own and an iPhone with its apps, or not at all.
        resolves(DEVICE_TAG, "view mailto", view("mailto:a@example.com?subject=s"));
        resolves(DEVICE_TAG, "sendto mailto", new Intent(Intent.ACTION_SENDTO, Uri.parse("mailto:a@example.com")));
        resolves(DEVICE_TAG, "send rfc822 with addresses", send("message/rfc822")
                .putExtra(Intent.EXTRA_EMAIL, new String[] {"a@example.com"}));
        resolves(DEVICE_TAG, "view geo", view("geo:52.52,13.40?q=Berlin"));
        resolves(DEVICE_TAG, "view google.navigation", view("google.navigation:q=Berlin&mode=d"));
        resolves(DEVICE_TAG, "call tel", new Intent(Intent.ACTION_CALL, Uri.parse("tel:123")));
        ResolveInfo browser = mPackages.resolveActivity(view("https://example.com/"), 0);
        Log.i(DEVICE_TAG, "browser: " + (browser == null ? "none" : browser.activityInfo.packageName));
        Log.i(TAG, "done");
    }
}
