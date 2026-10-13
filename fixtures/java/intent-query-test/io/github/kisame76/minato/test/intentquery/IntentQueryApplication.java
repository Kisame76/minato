package io.github.kisame76.minato.test.intentquery;

import android.app.Application;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ComponentInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.net.Uri;
import android.util.Log;

import java.util.List;

/**
 * PackageManager's intent resolution over the app's own components (ADR-0045 step SV1), as SDKs ask it before they
 * start a service, send a broadcast or open a link: queryIntentActivities, resolveActivity, queryIntentServices,
 * resolveService, queryBroadcastReceivers and queryIntentContentProviders with implicit and explicit intents, data,
 * MIME types, MATCH_DEFAULT_ONLY, GET_META_DATA, GET_RESOLVED_FILTER, MATCH_DISABLED_COMPONENTS and the component
 * states setComponentEnabledSetting leaves. Implicit intents name the app's package, so that the emulator's other apps
 * do not answer; a package that is not installed answers nothing.
 */
public class IntentQueryApplication extends Application {
    private static final String TAG = "intent-query-test";
    private static final String ACTION = "io.github.kisame76.minato.test.intentquery.";

    private static void log(String line) {
        Log.i(TAG, line);
    }

    @Override
    public void onCreate() {
        super.onCreate();
        PackageManager pm = getPackageManager();
        String own = getPackageName();

        Intent view = new Intent(Intent.ACTION_VIEW, Uri.parse("minato://example.test/items/7")).setPackage(own);
        list("activities view", pm.queryIntentActivities(view, 0));
        list("activities view default", pm.queryIntentActivities(view, PackageManager.MATCH_DEFAULT_ONLY));
        list("activities view browsable", pm.queryIntentActivities(
                new Intent(view).addCategory(Intent.CATEGORY_BROWSABLE), PackageManager.MATCH_DEFAULT_ONLY));
        list("activities view other host", pm.queryIntentActivities(
                new Intent(Intent.ACTION_VIEW, Uri.parse("minato://other.test/items/7")).setPackage(own), 0));
        list("activities view other path", pm.queryIntentActivities(
                new Intent(Intent.ACTION_VIEW, Uri.parse("minato://example.test/shop")).setPackage(own), 0));
        Intent send = new Intent(Intent.ACTION_SEND).setType("text/plain").setPackage(own);
        list("activities send text/plain", pm.queryIntentActivities(send, PackageManager.MATCH_DEFAULT_ONLY));
        list("activities send text/html", pm.queryIntentActivities(
                new Intent(Intent.ACTION_SEND).setType("text/html").setPackage(own), PackageManager.MATCH_DEFAULT_ONLY));
        list("activities send image/png", pm.queryIntentActivities(
                new Intent(Intent.ACTION_SEND).setType("image/png").setPackage(own), 0));
        Intent plain = new Intent(ACTION + "PLAIN").setPackage(own);
        list("activities plain", pm.queryIntentActivities(plain, 0));
        list("activities plain default", pm.queryIntentActivities(plain, PackageManager.MATCH_DEFAULT_ONLY));
        list("activities launcher", pm.queryIntentActivities(
                new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER).setPackage(own), 0));
        list("activities explicit", pm.queryIntentActivities(
                new Intent().setClassName(own, ViewActivity.class.getName()), 0));
        list("activities explicit missing", pm.queryIntentActivities(
                new Intent().setClassName(own, own + ".Missing"), 0));
        one("resolveActivity view", pm.resolveActivity(view, PackageManager.MATCH_DEFAULT_ONLY));
        one("resolveActivity send", pm.resolveActivity(send, PackageManager.MATCH_DEFAULT_ONLY));
        one("resolveActivity plain default", pm.resolveActivity(plain, PackageManager.MATCH_DEFAULT_ONLY));

        Intent sync = new Intent(ACTION + "SYNC");
        list("services sync", pm.queryIntentServices(sync, 0));
        list("services sync own", pm.queryIntentServices(new Intent(sync).setPackage(own), 0));
        list("services sync meta-data", pm.queryIntentServices(sync, PackageManager.GET_META_DATA));
        list("services sync filter", pm.queryIntentServices(sync, PackageManager.GET_RESOLVED_FILTER));
        list("services sync disabled too", pm.queryIntentServices(sync, PackageManager.MATCH_DISABLED_COMPONENTS));
        list("services sync other package", pm.queryIntentServices(
                new Intent(sync).setPackage("com.google.android.gms"), 0));
        list("services explicit", pm.queryIntentServices(new Intent().setClassName(own, PlainService.class.getName()), 0));
        list("services explicit disabled", pm.queryIntentServices(
                new Intent().setClassName(own, OffService.class.getName()), 0));
        one("resolveService sync", pm.resolveService(sync, 0));
        one("resolveService explicit", pm.resolveService(new Intent().setClassName(own, PlainService.class.getName()), 0));
        one("resolveService nothing", pm.resolveService(new Intent(ACTION + "NOTHING"), 0));
        one("resolveService gms", pm.resolveService(
                new Intent("com.google.android.gms.measurement.START").setPackage("com.google.android.gms"), 0));

        list("receivers ping", pm.queryBroadcastReceivers(new Intent(ACTION + "PING"), 0));
        list("receivers ping own", pm.queryBroadcastReceivers(new Intent(ACTION + "PING").setPackage(own), 0));
        list("receivers boot", pm.queryBroadcastReceivers(new Intent(Intent.ACTION_BOOT_COMPLETED).setPackage(own), 0));
        list("receivers package added", pm.queryBroadcastReceivers(
                new Intent(Intent.ACTION_PACKAGE_ADDED, Uri.parse("package:com.example.other")).setPackage(own), 0));
        list("receivers package added no data", pm.queryBroadcastReceivers(
                new Intent(Intent.ACTION_PACKAGE_ADDED).setPackage(own), 0));

        list("providers provide", pm.queryIntentContentProviders(new Intent(ACTION + "PROVIDE"), 0));
        list("providers provide own", pm.queryIntentContentProviders(
                new Intent(ACTION + "PROVIDE").setPackage(own), 0));

        ComponentName off = new ComponentName(own, OffService.class.getName());
        ComponentName high = new ComponentName(own, HighService.class.getName());
        pm.setComponentEnabledSetting(off, PackageManager.COMPONENT_ENABLED_STATE_ENABLED,
                PackageManager.DONT_KILL_APP);
        pm.setComponentEnabledSetting(high, PackageManager.COMPONENT_ENABLED_STATE_DISABLED,
                PackageManager.DONT_KILL_APP);
        list("services sync after enable and disable", pm.queryIntentServices(sync, 0));
        one("resolveService sync after", pm.resolveService(sync, 0));
        pm.setComponentEnabledSetting(off, PackageManager.COMPONENT_ENABLED_STATE_DEFAULT,
                PackageManager.DONT_KILL_APP);
        pm.setComponentEnabledSetting(high, PackageManager.COMPONENT_ENABLED_STATE_DEFAULT,
                PackageManager.DONT_KILL_APP);
        list("services sync after reset", pm.queryIntentServices(sync, 0));
        log("done");
    }

    private static void list(String what, List<ResolveInfo> infos) {
        StringBuilder line = new StringBuilder(what).append(": ").append(infos.size());
        for (ResolveInfo info : infos) {
            line.append("; ").append(describe(info));
        }
        log(line.toString());
    }

    private static void one(String what, ResolveInfo info) {
        log(what + ": " + (info == null ? "null" : describe(info)));
    }

    /** What a ResolveInfo says about the match, without the fields that differ by device. */
    private static String describe(ResolveInfo info) {
        ComponentInfo component = info.activityInfo != null ? info.activityInfo
                : info.serviceInfo != null ? info.serviceInfo : info.providerInfo;
        String kind = info.activityInfo != null ? "activity" : info.serviceInfo != null ? "service" : "provider";
        String name = component.name.substring(component.name.lastIndexOf('.') + 1);
        return kind + " " + name + " priority " + info.priority + " match 0x" + Integer.toHexString(info.match)
                + " default " + info.isDefault + " filter " + (info.filter != null)
                + (info.filter != null ? " actions " + info.filter.countActions() : "")
                + " meta-data " + (component.metaData == null ? "none" : component.metaData.keySet())
                + " enabled " + component.enabled;
    }
}
