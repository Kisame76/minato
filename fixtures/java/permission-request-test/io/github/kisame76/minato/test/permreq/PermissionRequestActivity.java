package io.github.kisame76.minato.test.permreq;

import android.app.Activity;
import android.content.pm.PackageManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.util.Arrays;

/**
 * ADR-0063's test of Activity.requestPermissions: the requests that need no dialog, which Android answers at once and
 * Minato answers from its policy and the host's grant. One after the other, each logs what onRequestPermissionsResult
 * brings and what checkSelfPermission and shouldShowRequestPermissionRationale say after it. The twin (-fine) declares
 * the two location permissions, which the harness grants before the start. The tag is the label.
 */
public class PermissionRequestActivity extends Activity {
    private static final String FINE = "android.permission.ACCESS_FINE_LOCATION";
    private static final String COARSE = "android.permission.ACCESS_COARSE_LOCATION";
    private static final String INTERNET = "android.permission.INTERNET";
    private static final String[][] REQUESTS = {
        {INTERNET}, {FINE}, {COARSE, FINE, INTERNET}, {"android.permission.nonsense"}, {},
    };

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private String mTag;
    private boolean mStarted;
    private int mNext;

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && !mStarted) {
            mStarted = true;
            mTag = getApplicationInfo().loadLabel(getPackageManager()).toString();
            Log.i(mTag, "controller: " + getPackageManager().getPermissionControllerPackageName());
            mHandler.post(this::next);
        }
    }

    private void next() {
        if (mNext == REQUESTS.length) {
            Log.i(mTag, "done");
            return;
        }
        final int index = mNext++;
        final String[] names = REQUESTS[index];
        Log.i(mTag, "request " + index + ": " + Arrays.toString(names));
        try {
            requestPermissions(names, index);
        } catch (RuntimeException e) {
            Log.i(mTag, "request " + index + " threw " + e.getClass().getName() + ": " + e.getMessage());
            mHandler.post(this::next);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        final StringBuilder text = new StringBuilder();
        for (int i = 0; i < permissions.length; i++) {
            text.append(i == 0 ? "" : ", ").append(permissions[i]).append(" ")
                    .append(grantResults[i] == PackageManager.PERMISSION_GRANTED ? "granted" : "denied");
        }
        Log.i(mTag, "result " + requestCode + ": " + permissions.length + " [" + text + "]");
        for (String name : permissions) {
            Log.i(mTag, "after " + name + ": check " + checkSelfPermission(name) + ", rationale "
                    + shouldShowRequestPermissionRationale(name));
        }
        mHandler.post(this::next);
    }
}
