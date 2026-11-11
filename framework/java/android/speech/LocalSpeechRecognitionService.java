/*
 * The speech recognition manager service (group 3 of the framework breadth, speech-test): IRecognitionServiceManager of
 * system_server, local in the app's process, which ServiceManager's table gives as "speech_recognition" and
 * SpeechRecognizer asks. iOS gives an Android app no recognition service it could bind, so the service answers as an
 * Android 15 phone without one does [verified: speech-test on apk-runner-35]: the recognizer finds the manager but no
 * recognition component (isRecognitionAvailable is false), so createSession is never asked, and startListening ends in
 * onError(ERROR_CLIENT). It is AIDL's Default class (every answer is nothing). Recognition through iOS's own speech
 * framework would be an ADR of its own.
 */
package android.speech;

import android.os.Binder;
import android.os.IBinder;

public final class LocalSpeechRecognitionService extends IRecognitionServiceManager.Default {
    private final Binder mBinder = new Binder();

    private LocalSpeechRecognitionService() {
        mBinder.attachInterface(this, IRecognitionServiceManager.Stub.DESCRIPTOR);
    }

    /** ServiceManager's maker for "speech_recognition": the Binder that carries a new service. */
    public static IBinder newBinder() {
        return new LocalSpeechRecognitionService().asBinder();
    }

    @Override
    public IBinder asBinder() {
        return mBinder;
    }
}
