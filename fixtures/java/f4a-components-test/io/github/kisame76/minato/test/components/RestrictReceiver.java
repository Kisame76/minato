package io.github.kisame76.minato.test.components;

import android.content.Context;
import android.os.PersistableBundle;
import android.service.restrictions.RestrictionsReceiver;

/** A restrictions receiver, which an app declares and only the system calls. */
public class RestrictReceiver extends RestrictionsReceiver {
    @Override
    public void onRequestPermission(Context context, String packageName, String requestType, String requestId,
            PersistableBundle request) {
    }
}
