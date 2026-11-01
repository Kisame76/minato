package io.github.kisame76.minato.test.gmsabsent;

import android.app.Application;
import android.content.ComponentName;
import android.content.Context;
import android.content.ContentProviderClient;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.InstallSourceInfo;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.database.Cursor;
import android.net.Uri;
import android.os.IBinder;
import android.provider.Settings;
import android.util.Log;

/**
 * What an app meets when Google Play services and the Play Store are not installed, the calls that client libraries
 * make before they take their fallback path (the compatibility plan's open gap "Play services absent, gracefully"):
 * package lookups, binds and starts of the services of com.google.android.gms and com.android.vending, links to the
 * Play Store, their content providers, createPackageContext, and what an app can learn about its installer. Each line
 * is the call and its result, an exception as its class and message. The reference is Android 15's emulator without
 * Play.
 */
public class GmsAbsentApplication extends Application {
    private static final String TAG = "gms-absent-test";
    private static final String GMS = "com.google.android.gms";
    private static final String VENDING = "com.android.vending";

    private interface Call {
        Object run() throws Exception;
    }

    private static void check(String name, Call call) {
        String result;
        try {
            final Object value = call.run();
            result = String.valueOf(value);
        } catch (Throwable e) {
            result = e.getClass().getName() + ": " + e.getMessage();
        }
        Log.i(TAG, name + ": " + result);
    }

    private final ServiceConnection mConnection = new ServiceConnection() {
        @Override
        public void onServiceConnected(ComponentName name, IBinder service) {
            Log.i(TAG, "connected " + name);
        }

        @Override
        public void onServiceDisconnected(ComponentName name) {
            Log.i(TAG, "disconnected " + name);
        }
    };

    @Override
    public void onCreate() {
        super.onCreate();
        final PackageManager pm = getPackageManager();
        final String own = getPackageName();

        // The lookups that GoogleApiAvailability's documented SERVICE_MISSING, an installer check and the Play Store
        // links start from.
        check("getPackageInfo gms flags 0", () -> pm.getPackageInfo(GMS, 0));
        check("getPackageInfo gms signing", () -> pm.getPackageInfo(GMS, PackageManager.GET_SIGNING_CERTIFICATES));
        check("getPackageInfo vending meta-data", () -> pm.getPackageInfo(VENDING, PackageManager.GET_META_DATA));
        check("getPackageInfo maps", () -> pm.getPackageInfo("com.google.android.apps.maps", 0));
        check("getPackageInfo play games", () -> pm.getPackageInfo("com.google.android.play.games", 0));
        check("getLaunchIntentForPackage gms", () -> pm.getLaunchIntentForPackage(GMS));
        check("getLaunchIntentForPackage vending", () -> pm.getLaunchIntentForPackage(VENDING));
        check("getInstallerPackageName own", () -> pm.getInstallerPackageName(own));
        check("getInstallerPackageName gms", () -> pm.getInstallerPackageName(GMS));
        check("getInstallSourceInfo own installing", () -> {
            final InstallSourceInfo info = pm.getInstallSourceInfo(own);
            // The emulator's test install is made by adb, whose initiating package is the shell: not an installer an
            // app can see in Minato, so it counts as none, the same as in the reference and on the Mac.
            final String initiating = "com.android.shell".equals(info.getInitiatingPackageName()) ? null
                    : info.getInitiatingPackageName();
            return "initiating " + initiating + ", installing " + info.getInstallingPackageName() + ", originating "
                    + info.getOriginatingPackageName() + ", update owner " + info.getUpdateOwnerPackageName()
                    + ", signing " + (info.getInitiatingPackageSigningInfo() != null) + ", source "
                    + info.getPackageSource();
        });
        check("getInstallSourceInfo gms", () -> pm.getInstallSourceInfo(GMS));
        check("hasSystemFeature GOOGLE_EXPERIENCE", () ->
                pm.hasSystemFeature("com.google.android.feature.GOOGLE_EXPERIENCE"));
        check("hasSystemFeature GOOGLE_BUILD", () -> pm.hasSystemFeature("com.google.android.feature.GOOGLE_BUILD"));
        check("system shared libraries have gms", () -> {
            boolean found = false;
            for (String name : pm.getSystemSharedLibraryNames()) {
                found |= name.startsWith("com.google.android.gms") || name.equals("com.google.android.maps");
            }
            return found;
        });
        check("installed packages have gms or vending", () -> {
            boolean found = false;
            for (android.content.pm.PackageInfo info : pm.getInstalledPackages(0)) {
                found |= info.packageName.equals(GMS) || info.packageName.equals(VENDING);
            }
            return found;
        });
        check("getComponentEnabledSetting gms", () -> pm.getComponentEnabledSetting(
                new ComponentName(GMS, "com.google.android.gms.Missing")));
        check("getPackageUid gms", () -> pm.getPackageUid(GMS, 0));
        check("checkSignatures own gms", () -> pm.checkSignatures(own, GMS));

        // Services: the binds and starts of Billing, FCM, measurement and the install referrer, as the old public
        // documents and the Play client libraries' docs name them.
        check("bindService billing", () -> bindService(
                new Intent("com.android.vending.billing.InAppBillingService.BIND").setPackage(VENDING), mConnection,
                Context.BIND_AUTO_CREATE));
        check("bindService gms measurement", () -> bindService(
                new Intent("com.google.android.gms.measurement.START").setPackage(GMS), mConnection,
                Context.BIND_AUTO_CREATE));
        check("bindService gms component", () -> bindService(
                new Intent().setComponent(new ComponentName(GMS, "com.google.android.gms.Missing")), mConnection,
                Context.BIND_AUTO_CREATE));
        check("bindService implicit", () -> bindService(
                new Intent("com.android.vending.billing.InAppBillingService.BIND"), mConnection,
                Context.BIND_AUTO_CREATE));
        check("unbindService never bound", () -> {
            unbindService(mConnection);
            return "returned";
        });
        check("startService c2dm register", () -> startService(
                new Intent("com.google.android.c2dm.intent.REGISTER").setPackage(GMS)));
        check("startService gms component", () -> startService(
                new Intent().setComponent(new ComponentName(GMS, "com.google.android.gms.Missing"))));
        check("stopService gms component", () -> stopService(
                new Intent().setComponent(new ComponentName(GMS, "com.google.android.gms.Missing"))));
        check("sendBroadcast c2dm", () -> {
            sendBroadcast(new Intent("com.google.android.c2dm.intent.RECEIVE").setPackage(GMS));
            return "returned";
        });

        // Links to the Play Store, as the SERVICE_MISSING dialog and "rate this app" buttons start them.
        final Intent market = new Intent(Intent.ACTION_VIEW, Uri.parse("market://details?id=" + own))
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        check("resolveActivity market", () -> pm.resolveActivity(market, 0));
        check("startActivity market", () -> {
            startActivity(market);
            return "started";
        });
        check("startActivity market vending", () -> {
            startActivity(new Intent(market).setPackage(VENDING));
            return "started";
        });
        check("startActivity market gms", () -> {
            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse("market://details?id=" + GMS))
                    .setPackage(VENDING).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            return "started";
        });
        check("startActivity web play vending", () -> {
            startActivity(new Intent(Intent.ACTION_VIEW,
                    Uri.parse("https://play.google.com/store/apps/details?id=" + own))
                    .setPackage(VENDING).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            return "started";
        });
        check("resolveActivity geo", () -> pm.resolveActivity(new Intent(Intent.ACTION_VIEW,
                Uri.parse("geo:0,0?q=harbor")).setPackage("com.google.android.apps.maps"), 0));

        // Providers.
        check("query gservices", () -> {
            final Cursor cursor = getContentResolver().query(Uri.parse("content://com.google.android.gsf.gservices"),
                    null, null, null, null);
            return cursor;
        });
        check("query chimera", () -> getContentResolver().query(
                Uri.parse("content://com.google.android.gms.chimera/"), null, null, null, null));
        check("getType gservices", () -> getContentResolver().getType(
                Uri.parse("content://com.google.android.gsf.gservices")));
        check("acquireContentProviderClient chimera", () -> {
            final ContentProviderClient client = getContentResolver().acquireContentProviderClient(
                    "com.google.android.gms.chimera");
            return client;
        });
        check("resolveContentProvider gservices", () -> {
            final ProviderInfo info = pm.resolveContentProvider("com.google.android.gsf.gservices", 0);
            return info;
        });

        // Contexts and settings.
        check("createPackageContext gms code", () -> createPackageContext(GMS,
                Context.CONTEXT_INCLUDE_CODE | Context.CONTEXT_IGNORE_SECURITY));
        check("createPackageContext vending", () -> createPackageContext(VENDING, 0));
        check("android_id set", () ->
                Settings.Secure.getString(getContentResolver(), Settings.Secure.ANDROID_ID) != null);
        Log.i(TAG, "done");
    }
}
