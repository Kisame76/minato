/*
 * Binder, answered locally (ADR-0039 decision 10, step c1): our class under AOSP's name, as decision 5 has it for
 * ActivityThread. The process has no binder driver and no other process to call, so the caller is always the app
 * itself, with its own pid and uid, and a transaction is a call of onTransact in the same process. AOSP's Binder
 * holds a native BBinder and asks libbinder for the calling identity; ours keeps the identity and the thread's
 * StrictMode policy in Java. Apps extend it for their local services (a bound service's LocalBinder), and AIDL's Stub
 * classes extend it with attachInterface, through which ADR-0045's local services find their stub.
 */
package android.os;

import java.io.FileDescriptor;
import java.io.PrintWriter;

public class Binder implements IBinder {
    /** As AOSP's: off. StrictMode reads it to check the size of a parcel it writes. */
    public static final boolean CHECK_PARCEL_SIZE = false;

    /** The thread's StrictMode policy, which AOSP keeps in libbinder's IPCThreadState. */
    private static final ThreadLocal<Integer> sThreadStrictModePolicy = ThreadLocal.withInitial(() -> 0);

    private IInterface mOwner;
    private String mDescriptor;

    public Binder() {
        this(null);
    }

    public Binder(String descriptor) {
        mDescriptor = descriptor;
    }

    /** The caller's pid: the app's own, since every call comes from the process itself. */
    public static final int getCallingPid() {
        return Process.myPid();
    }

    /** The caller's uid: the app's own (decision 10). */
    public static final int getCallingUid() {
        return Process.myUid();
    }

    public static final int getCallingUidOrThrow() {
        return getCallingUid();
    }

    public static final UserHandle getCallingUserHandle() {
        return UserHandle.of(UserHandle.getUserId(getCallingUid()));
    }

    /**
     * AOSP's packs the caller's uid and pid into the token; ours does the same, so that restoreCallingIdentity takes
     * back what it was given. The identity itself never changes.
     */
    public static final long clearCallingIdentity() {
        return ((long) getCallingUid() << 32) | (getCallingPid() & 0xffffffffL);
    }

    public static final void restoreCallingIdentity(long token) {
    }

    /** No driver holds commands to flush. */
    public static final void flushPendingCommands() {
    }

    /** @hide */
    public static final void setThreadStrictModePolicy(int policyMask) {
        sThreadStrictModePolicy.set(policyMask);
    }

    /** @hide */
    public static final int getThreadStrictModePolicy() {
        return sThreadStrictModePolicy.get();
    }

    /** No pool of binder threads exists to join. */
    public static final void joinThreadPool() {
    }

    /** A local binder blocks anyway: it is returned as it is. */
    public static IBinder allowBlocking(IBinder binder) {
        return binder;
    }

    public void attachInterface(IInterface owner, String descriptor) {
        mOwner = owner;
        mDescriptor = descriptor;
    }

    @Override
    public String getInterfaceDescriptor() {
        return mDescriptor;
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
        return mDescriptor != null && mDescriptor.equals(descriptor) ? mOwner : null;
    }

    protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
        return false;
    }

    /** A transaction is a call of onTransact in this process, as AOSP's Binder makes it for a local caller. */
    @Override
    public final boolean transact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
        return onTransact(code, data, reply, flags);
    }

    /** A local binder dies with the process, so a death recipient is never called, as with AOSP's. */
    @Override
    public void linkToDeath(DeathRecipient recipient, int flags) {
    }

    @Override
    public boolean unlinkToDeath(DeathRecipient recipient, int flags) {
        return true;
    }

    @Override
    public void dump(FileDescriptor fd, String[] args) {
    }

    @Override
    public void dumpAsync(FileDescriptor fd, String[] args) {
    }

    protected void dump(FileDescriptor fd, PrintWriter fout, String[] args) {
    }

    /**
     * No shell reaches the app, so a shell command does nothing.
     *
     * @hide
     */
    @Override
    public void shellCommand(FileDescriptor in, FileDescriptor out, FileDescriptor err, String[] args,
            ShellCallback shellCallback, ResultReceiver resultReceiver) {
    }
}
