package io.github.kisame76.minato.test.binder;

import android.app.Application;
import android.os.Binder;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.Process;
import android.os.RemoteException;
import android.os.ResultReceiver;
import android.os.ShellCallback;
import android.util.Log;
import java.io.FileDescriptor;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * ADR-0043 step d2b)'s test, the app's own binders. Each of the app's classes below meets ART's check of the
 * interfaces it has from the boot class path, which resolves every type of IBinder.shellCommand, ShellCallback and
 * ResultReceiver among them: a local binder that extends Binder, as a bound service's, whose transactions a proxy
 * makes; a stub and its proxy in the form AIDL generates; an IBinder of the app's own that is no Binder; and
 * ResultReceivers, without a Handler, with one and through a Parcel, where the copy sends to the original. It logs
 * under the tag binder-test what each answers. The test needs no window: as in drawables-test, the launcher activity is
 * the framework's own android.app.Activity and the driver loads only the Application class. A failing part logs
 * "failed" and its stack under binder-test-device, which differs by device and is only shown.
 * tools/android-reference.sh takes the log from Android 15, and tools/binder-check.sh compares Minato's with it.
 */
public class BinderApplication extends Application {
    private static final String TAG = "binder-test";
    private static final String DESCRIPTOR = "io.github.kisame76.minato.test.binder.IAdder";
    private static final int ADD = IBinder.FIRST_CALL_TRANSACTION;

    /** One part of the test, which may throw. */
    private interface Part {
        void run() throws Exception;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        part("local", BinderApplication::local);
        part("transact", BinderApplication::transact);
        part("stub", BinderApplication::stub);
        part("parcel", BinderApplication::parcel);
        part("own", BinderApplication::own);
        part("receiver", BinderApplication::receiver);
        part("parceled receiver", BinderApplication::parceledReceiver);
        Log.i(TAG, "done");
    }

    private static void part(String name, Part part) {
        try {
            part.run();
        } catch (Throwable t) {
            Log.i(TAG, name + ": failed: " + t);
            // Its stack, which differs by device, is only shown.
            Log.i(TAG + "-device", Log.getStackTraceString(t));
        }
    }

    /** A bound service's local binder: what it answers before and after an interface is attached. */
    private static void local() {
        LocalBinder binder = new LocalBinder();
        Log.i(TAG, "local: descriptor " + binder.getInterfaceDescriptor() + ", ping " + binder.pingBinder()
                + ", alive " + binder.isBinderAlive() + ", local interface " + binder.queryLocalInterface(DESCRIPTOR));
        IInterface owner = () -> binder;
        binder.attachInterface(owner, DESCRIPTOR);
        Log.i(TAG, "local: attached, descriptor " + binder.getInterfaceDescriptor() + ", local interface the owner "
                + (binder.queryLocalInterface(DESCRIPTOR) == owner) + ", another descriptor's "
                + binder.queryLocalInterface("other"));
        Log.i(TAG, "local: calling uid the app's " + (Binder.getCallingUid() == Process.myUid())
                + ", calling pid the process's " + (Binder.getCallingPid() == Process.myPid()));
        IBinder.DeathRecipient recipient = () -> Log.i(TAG, "local: died");
        binder.linkToDeath(recipient, 0);
        Log.i(TAG, "local: linked to death, unlinked " + binder.unlinkToDeath(recipient, 0));
    }

    /** Transactions on the local binder as a proxy makes them: its code, one-way too, the interface's and another. */
    private static void transact() throws RemoteException {
        LocalBinder binder = new LocalBinder();
        binder.attachInterface(() -> binder, DESCRIPTOR);
        Parcel data = Parcel.obtain();
        Parcel reply = Parcel.obtain();
        try {
            data.writeInterfaceToken(DESCRIPTOR);
            data.writeInt(2);
            data.writeInt(3);
            boolean handled = binder.transact(ADD, data, reply, 0);
            reply.readException();
            Log.i(TAG, "transact: add " + handled + ", sum " + reply.readInt());
            handled = binder.transact(ADD, data, null, IBinder.FLAG_ONEWAY);
            Log.i(TAG, "transact: one-way add " + handled);
        } finally {
            reply.recycle();
            data.recycle();
        }
        data = Parcel.obtain();
        reply = Parcel.obtain();
        try {
            boolean handled = binder.transact(IBinder.INTERFACE_TRANSACTION, data, reply, 0);
            Log.i(TAG, "transact: interface " + handled + ", " + reply.readString());
        } finally {
            reply.recycle();
            data.recycle();
        }
        data = Parcel.obtain();
        reply = Parcel.obtain();
        try {
            boolean handled = binder.transact(ADD + 41, data, reply, 0);
            Log.i(TAG, "transact: another code " + handled + ", reply size " + reply.dataSize());
        } finally {
            reply.recycle();
            data.recycle();
        }
    }

    /** An AIDL-style stub: asInterface finds it in its binder, and its proxy calls it through transactions. */
    private static void stub() throws RemoteException {
        IAdder.Stub stub = new Adder();
        IAdder local = IAdder.Stub.asInterface(stub);
        Log.i(TAG, "stub: asInterface gives the stub " + (local == stub) + ", add " + local.add(4, 5) + ", descriptor "
                + stub.getInterfaceDescriptor());
        IAdder proxy = new IAdder.Proxy(stub);
        Log.i(TAG, "stub: through the proxy, add " + proxy.add(6, 7) + ", its binder the stub "
                + (proxy.asBinder() == stub));
    }

    /** Binders in a Parcel, as an app passes one in a Bundle or an Intent: in one process each comes back itself. */
    private static void parcel() {
        LocalBinder binder = new LocalBinder();
        IAdder.Stub stub = new Adder();
        Parcel parcel = Parcel.obtain();
        try {
            parcel.writeStrongBinder(binder);
            parcel.writeStrongBinder(stub);
            parcel.writeStrongBinder(null);
            parcel.setDataPosition(0);
            IBinder first = parcel.readStrongBinder();
            IBinder second = parcel.readStrongBinder();
            IBinder third = parcel.readStrongBinder();
            Log.i(TAG, "parcel: the local binder back " + (first == binder) + ", the stub " + (second == stub)
                    + ", null " + third + ", the stub's interface " + (IAdder.Stub.asInterface(second) == stub));
        } finally {
            parcel.recycle();
        }
    }

    /** An IBinder of the app's own, no Binder: in a Parcel it is a null binder, as the parcel takes only Binders. */
    private static void own() {
        OwnBinder own = new OwnBinder();
        Log.i(TAG, "own: descriptor " + own.getInterfaceDescriptor() + ", ping " + own.pingBinder());
        Parcel parcel = Parcel.obtain();
        try {
            parcel.writeStrongBinder(own);
            parcel.setDataPosition(0);
            Log.i(TAG, "own: back from a parcel " + parcel.readStrongBinder());
        } finally {
            parcel.recycle();
        }
    }

    /** A ResultReceiver of the app's own: without a Handler its result comes at once, with one on its thread. */
    private static void receiver() throws InterruptedException {
        Thread sender = Thread.currentThread();
        Receiver direct = new Receiver(null, null);
        direct.send(1, bundle("first"));
        Log.i(TAG, "receiver: without a Handler " + direct.mResult + ", on the sender's thread "
                + (direct.mThread == sender) + ", contents " + direct.describeContents());
        HandlerThread thread = new HandlerThread("receiver");
        thread.start();
        try {
            CountDownLatch latch = new CountDownLatch(1);
            Receiver posted = new Receiver(new Handler(thread.getLooper()), latch);
            posted.send(2, bundle("second"));
            boolean came = latch.await(5, TimeUnit.SECONDS);
            Log.i(TAG, "receiver: with a Handler, came " + came + ", " + posted.mResult + ", on its thread "
                    + (posted.mThread == thread));
        } finally {
            thread.quitSafely();
        }
    }

    /** A ResultReceiver through a Parcel, as one goes to another component in an Intent: the copy sends to it. */
    private static void parceledReceiver() throws InterruptedException {
        HandlerThread thread = new HandlerThread("receiver");
        thread.start();
        try {
            CountDownLatch latch = new CountDownLatch(1);
            Receiver original = new Receiver(new Handler(thread.getLooper()), latch);
            ResultReceiver copy;
            Parcel parcel = Parcel.obtain();
            try {
                original.writeToParcel(parcel, 0);
                parcel.setDataPosition(0);
                copy = ResultReceiver.CREATOR.createFromParcel(parcel);
            } finally {
                parcel.recycle();
            }
            copy.send(3, bundle("third"));
            boolean came = latch.await(5, TimeUnit.SECONDS);
            Log.i(TAG, "parceled receiver: copy " + copy.getClass().getName() + ", came " + came + ", "
                    + original.mResult + ", on the original's thread " + (original.mThread == thread));
        } finally {
            thread.quitSafely();
        }
    }

    private static Bundle bundle(String text) {
        Bundle bundle = new Bundle();
        bundle.putString("text", text);
        return bundle;
    }

    /** As a bound service's binder: it adds two ints and leaves every other code to Binder. */
    private static final class LocalBinder extends Binder {
        @Override
        protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
            if (code != ADD) {
                return super.onTransact(code, data, reply, flags);
            }
            data.enforceInterface(DESCRIPTOR);
            int sum = data.readInt() + data.readInt();
            if (reply != null) {
                reply.writeNoException();
                reply.writeInt(sum);
            }
            return true;
        }
    }

    /** The app's interface in the form AIDL generates. */
    private interface IAdder extends IInterface {
        int add(int a, int b) throws RemoteException;

        /** As AIDL's Stub: a Binder that answers the interface's calls. */
        abstract class Stub extends Binder implements IAdder {
            Stub() {
                attachInterface(this, DESCRIPTOR);
            }

            static IAdder asInterface(IBinder binder) {
                if (binder == null) {
                    return null;
                }
                IInterface local = binder.queryLocalInterface(DESCRIPTOR);
                return local instanceof IAdder ? (IAdder) local : new Proxy(binder);
            }

            @Override
            public IBinder asBinder() {
                return this;
            }

            @Override
            protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
                if (code == INTERFACE_TRANSACTION) {
                    reply.writeString(DESCRIPTOR);
                    return true;
                }
                if (code != ADD) {
                    return super.onTransact(code, data, reply, flags);
                }
                data.enforceInterface(DESCRIPTOR);
                int result = add(data.readInt(), data.readInt());
                reply.writeNoException();
                reply.writeInt(result);
                return true;
            }
        }

        /** As AIDL's Proxy: each call is a transaction on the binder. */
        final class Proxy implements IAdder {
            private final IBinder mRemote;

            Proxy(IBinder remote) {
                mRemote = remote;
            }

            @Override
            public IBinder asBinder() {
                return mRemote;
            }

            @Override
            public int add(int a, int b) throws RemoteException {
                Parcel data = Parcel.obtain();
                Parcel reply = Parcel.obtain();
                try {
                    data.writeInterfaceToken(DESCRIPTOR);
                    data.writeInt(a);
                    data.writeInt(b);
                    mRemote.transact(ADD, data, reply, 0);
                    reply.readException();
                    return reply.readInt();
                } finally {
                    reply.recycle();
                    data.recycle();
                }
            }
        }
    }

    private static final class Adder extends IAdder.Stub {
        @Override
        public int add(int a, int b) {
            return a + b;
        }
    }

    /** An IBinder that is no Binder, as an app writes one for a test double. */
    private static final class OwnBinder implements IBinder {
        @Override
        public String getInterfaceDescriptor() {
            return "own";
        }

        @Override
        public boolean pingBinder() {
            return true;
        }

        @Override
        public boolean isBinderAlive() {
            return true;
        }

        @Override
        public IInterface queryLocalInterface(String descriptor) {
            return null;
        }

        @Override
        public void dump(FileDescriptor fd, String[] args) {
        }

        @Override
        public void dumpAsync(FileDescriptor fd, String[] args) {
        }

        @Override
        public void shellCommand(FileDescriptor in, FileDescriptor out, FileDescriptor err, String[] args,
                ShellCallback shellCallback, ResultReceiver resultReceiver) {
        }

        @Override
        public boolean transact(int code, Parcel data, Parcel reply, int flags) {
            return false;
        }

        @Override
        public void linkToDeath(DeathRecipient recipient, int flags) {
        }

        @Override
        public boolean unlinkToDeath(DeathRecipient recipient, int flags) {
            return true;
        }
    }

    /** A ResultReceiver of the app's own, which keeps its result and the thread it came on. */
    private static final class Receiver extends ResultReceiver {
        private final CountDownLatch mLatch;
        volatile String mResult;
        volatile Thread mThread;

        Receiver(Handler handler, CountDownLatch latch) {
            super(handler);
            mLatch = latch;
        }

        @Override
        protected void onReceiveResult(int resultCode, Bundle resultData) {
            mResult = resultCode + " " + resultData.getString("text");
            mThread = Thread.currentThread();
            if (mLatch != null) {
                mLatch.countDown();
            }
        }
    }
}
