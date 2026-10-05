package com.android.internal.inputmethod;

import android.os.IBinder;
import android.os.IInterface;

/** Compile-only stub (ADR-0043 step d)), never in framework.jar: the URI permission of content an input method commits. */
public interface IInputContentUriToken extends IInterface {
    void take() throws android.os.RemoteException;

    void release() throws android.os.RemoteException;

    abstract class Stub extends android.os.Binder implements IInputContentUriToken {
        public static IInputContentUriToken asInterface(IBinder obj) {
            throw new RuntimeException("stub");
        }
    }
}
