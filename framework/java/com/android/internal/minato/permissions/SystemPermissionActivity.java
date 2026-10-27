package com.android.internal.minato.permissions;

import android.app.Activity;
import android.app.ActivityThread;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.util.Log;

import com.android.minato.permissions.PermissionAnswers;
import com.android.minato.permissions.PermissionPolicy;
import com.android.minato.permissions.PermissionRequests;

import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicLong;

/**
 * The activity that stands in for the phone's permission dialog (ADR-0063): Activity.requestPermissions starts it with
 * the names, as the permission controller is started. A request that needs no dialog ends at once with PermissionPolicy's
 * answers; one for the location the host decides goes to the host (APKRT_REQUEST_PERMISSION), whose sheet and iOS's prompt
 * are the dialog, and ends with the answer. It has no window of its own, and the translucent theme keeps the app's under it.
 */
public final class SystemPermissionActivity extends Activity {
    private static final String TAG = "SystemPermission";
    /** Linux's EOPNOTSUPP, as ActivityThread's requests answer without a session. */
    private static final int EOPNOTSUPP = 95;
    private static final String CLASS = SystemPermissionActivity.class.getName();

    private static final AtomicLong sNextId = new AtomicLong(1);
    /** The activities waiting for the host's answer, by request ID; the main thread's. */
    private static final Map<Long, SystemPermissionActivity> sWaiting = new HashMap<>();

    private long mId;
    private String[] mNames;

    /** The activity of the framework class, in the app's package as an activity of the app is. */
    public static ActivityInfo activityInfo(ApplicationInfo app) {
        final ActivityInfo info = new ActivityInfo();
        info.packageName = app.packageName;
        info.name = CLASS;
        info.applicationInfo = app;
        info.processName = app.processName;
        info.exported = false;
        info.launchMode = ActivityInfo.LAUNCH_MULTIPLE;
        // A change of the configuration does not recreate it, which would ask the host a second time.
        info.configChanges = -1;
        info.theme = com.android.internal.R.style.Theme_Translucent_NoTitleBar;
        return info;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mNames = getIntent().getStringArrayExtra(PackageManager.EXTRA_REQUEST_PERMISSIONS_NAMES);
        if (mNames == null || mNames.length == 0) {
            setResult(RESULT_CANCELED);
            finish();
            return;
        }
        final PermissionAnswers answers = ActivityThread.permissionAnswers();
        final int detail = answers == null ? 0 : PermissionRequests.detailToAsk(Arrays.asList(mNames),
                ActivityThread::permissionGrant, answers);
        if (detail == 0) {
            finishWithResults();
            return;
        }
        mId = sNextId.getAndIncrement();
        sWaiting.put(mId, this);
        final int error = ActivityThread.requestPermission(mId,
                PermissionRequests.requestText(PermissionPolicy.GROUP_LOCATION, detail));
        if (error != 0) {
            if (error != -EOPNOTSUPP) {
                Log.w(TAG, "The host refused the permission request: " + error);
            }
            sWaiting.remove(mId);
            finishWithResults();
        }
    }

    /**
     * The host's answer: PERMISSION_RESULT's request ID (0 for the foreground re-read of iOS's authorisation), group and
     * outcome. A solicited answer counts as a denial or a grant and ends the request; an unsolicited one only updates the grant.
     */
    public static void onResult(long id, int group, int outcome) {
        final PermissionAnswers answers = ActivityThread.permissionAnswers();
        if (answers != null) {
            answers.answer(group, outcome, id != 0);
        }
        if (id == 0) {
            return;
        }
        final SystemPermissionActivity activity = sWaiting.remove(id);
        if (activity == null) {
            Log.w(TAG, "An answer for permission request " + id + " that nobody waits for");
            return;
        }
        activity.finishWithResults();
    }

    /** Ends with what the app holds now, for each name asked, as the permission controller's result has it. */
    private void finishWithResults() {
        final int[] results = new int[mNames.length];
        for (int i = 0; i < results.length; i++) {
            results[i] = ActivityThread.permissionGranted(mNames[i]) ? PackageManager.PERMISSION_GRANTED
                    : PackageManager.PERMISSION_DENIED;
        }
        final Intent data = new Intent();
        data.putExtra(PackageManager.EXTRA_REQUEST_PERMISSIONS_NAMES, mNames);
        data.putExtra(PackageManager.EXTRA_REQUEST_PERMISSIONS_RESULTS, results);
        data.putExtra(PackageManager.EXTRA_REQUEST_PERMISSIONS_DEVICE_ID, Context.DEVICE_ID_DEFAULT);
        setResult(RESULT_OK, data);
        finish();
    }

    @Override
    protected void onDestroy() {
        // Gone before the answer, for instance finishActivity(requestCode): the host drops what it shows.
        if (sWaiting.remove(mId) != null) {
            ActivityThread.cancelPermission(mId);
        }
        super.onDestroy();
    }
}
