package com.android.internal.util;

import android.os.Parcelable;
import java.util.ArrayList;

/**
 * Compile-only stub (ADR-0039 step c2c1), never in framework.jar: content capture stays off on Minato: no Context
 * gives a ContentCaptureManager, as on a device without the service.
 */
public final class SyncResultReceiver extends com.android.internal.os.IResultReceiver.Stub {
    public SyncResultReceiver(int timeoutMs) {
        throw new RuntimeException("stub");
    }

    public int getIntResult() throws TimeoutException {
        throw new RuntimeException("stub");
    }

    public String getStringResult() throws TimeoutException {
        throw new RuntimeException("stub");
    }

    public String[] getStringArrayResult() throws TimeoutException {
        throw new RuntimeException("stub");
    }

    public <P extends Parcelable> P getParcelableResult() throws TimeoutException {
        throw new RuntimeException("stub");
    }

    public <P extends Parcelable> ArrayList<P> getParcelableListResult() throws TimeoutException {
        throw new RuntimeException("stub");
    }

    public int getOptionalExtraIntResult(int defaultValue) throws TimeoutException {
        throw new RuntimeException("stub");
    }

    public static final class TimeoutException extends Exception {
    }

    public void send(int resultCode, android.os.Bundle resultData) {
        throw new RuntimeException("stub");
    }

    public Object getObjectResult() throws TimeoutException {
        throw new RuntimeException("stub");
    }
}
