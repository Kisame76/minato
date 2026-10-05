package io.github.kisame76.minato.test.parcel;

import android.app.Application;
import android.content.Context;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.os.Binder;
import android.os.Bundle;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.os.StrictMode;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.text.SpannableString;
import android.text.Spanned;
import android.text.TextUtils;
import android.text.style.ForegroundColorSpan;
import android.text.style.StyleSpan;
import android.text.style.URLSpan;
import android.text.style.UnderlineSpan;
import android.util.Log;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;

/**
 * ADR-0043 d2's test of android.os.Parcel, AOSP's Parcel.java over Minato's natives in libbinder's wire format: from
 * Application.attachBaseContext it writes values, logs marshall()'s bytes in hex and what reading them back gives, so
 * the log holds Android 15's bytes. Primitives, strings, arrays, blobs, an interface token, exceptions, a Bundle and a
 * spanned text (SurroundingText's copy, TextUtils.writeToParcel) are bytes; appendFrom, reads past the end, and
 * unmarshalled bytes that are truncated or garbage (a string longer than the data, a binder object that is no object)
 * show the answers. A binder and a file descriptor come back as the same object and the same file, also through
 * appendFrom; the parcel's own descriptor closes with recycle or a cut before it; a plain read over a binder's bytes
 * reads nothing, and an object list out of order reads as libbinder's search finds it. Only public API, as an app has
 * it on Android. It logs with the tag parcel-test and ends with "done"; tools/android-reference.sh compares it with
 * Android 15's log.
 */
public class ParcelApplication extends Application {
    private static final String TAG = "parcel-test";

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        try {
            report(base);
        } catch (Throwable t) {
            Log.i(TAG, "failed: " + t);
            // Its stack, which differs by device, is only shown.
            Log.i(TAG + "-device", Log.getStackTraceString(t));
        }
        Log.i(TAG, "done");
    }

    static void report(Context context) throws Exception {
        primitives();
        strings();
        arrays();
        blobs();
        interfaceToken();
        exceptions();
        bundle(context);
        spannedText();
        appendFrom();
        pastTheEnd();
        unmarshalled();
        binders();
        descriptors(context);
    }

    static void primitives() {
        Parcel p = Parcel.obtain();
        p.writeInt(0x01020304);
        p.writeLong(0x0102030405060708L);
        p.writeFloat(1.5f);
        p.writeDouble(-2.25);
        p.writeByte((byte) 0x7f);
        p.writeBoolean(true);
        p.writeInt(-1);
        log("primitives", p);
        p.setDataPosition(0);
        Log.i(TAG, "primitives read back: " + p.readInt() + " " + p.readLong() + " " + p.readFloat() + " "
                + p.readDouble() + " " + p.readByte() + " " + p.readBoolean() + " " + p.readInt() + ", position "
                + p.dataPosition() + ", available " + p.dataAvail());
        p.recycle();
    }

    static void strings() {
        String[] values = {null, "", "abc", "é漢😀", "a\u0000b"};
        Parcel p = Parcel.obtain();
        for (String value : values) {
            p.writeString(value);
        }
        log("strings", p);
        p.setDataPosition(0);
        StringBuilder read = new StringBuilder();
        for (int i = 0; i < values.length; i++) {
            read.append(' ').append(quote(p.readString()));
        }
        Log.i(TAG, "strings read back:" + read);
        p.recycle();
    }

    static void arrays() {
        byte[] bytes = {1, 2, 3, 4, 5};
        Parcel p = Parcel.obtain();
        p.writeByteArray(null);
        p.writeByteArray(new byte[0]);
        p.writeByteArray(bytes);
        p.writeByteArray(bytes, 1, 3);
        p.writeIntArray(new int[] {7, -7});
        p.writeStringArray(new String[] {"x", null});
        p.writeBooleanArray(new boolean[] {true, false, true});
        log("arrays", p);
        p.setDataPosition(0);
        Log.i(TAG, "arrays read back: " + Arrays.toString(p.createByteArray()) + " "
                + Arrays.toString(p.createByteArray()) + " " + Arrays.toString(p.createByteArray()) + " "
                + Arrays.toString(p.createByteArray()) + " " + Arrays.toString(p.createIntArray()) + " "
                + Arrays.toString(p.createStringArray()) + " " + Arrays.toString(p.createBooleanArray()));
        // readByteArray wants an array of the stored length.
        p.setDataPosition(8);
        byte[] fits = new byte[5];
        p.readByteArray(fits);
        Log.i(TAG, "readByteArray into 5 bytes: " + Arrays.toString(fits));
        p.setDataPosition(8);
        try {
            p.readByteArray(new byte[4]);
            Log.i(TAG, "readByteArray into 4 bytes: read");
        } catch (RuntimeException e) {
            Log.i(TAG, "readByteArray into 4 bytes: " + e);
        }
        try {
            p.writeByteArray(bytes, 3, 3);
            Log.i(TAG, "writeByteArray past the array: written");
        } catch (ArrayIndexOutOfBoundsException e) {
            Log.i(TAG, "writeByteArray past the array: " + e);
        }
        p.recycle();
    }

    static void blobs() {
        Parcel p = Parcel.obtain();
        p.writeBlob(new byte[] {9, 8, 7, 6, 5});
        p.writeBlob(null);
        log("blobs", p);
        p.setDataPosition(0);
        Log.i(TAG, "blobs read back: " + Arrays.toString(p.readBlob()) + " " + Arrays.toString(p.readBlob()));
        p.recycle();
        // Above 16 KiB Android moves a blob into ashmem, where Minato keeps it in the data (a named deviation): only
        // its way back is compared.
        byte[] large = new byte[20000];
        for (int i = 0; i < large.length; i++) {
            large[i] = (byte) (i * 7);
        }
        Parcel q = Parcel.obtain();
        q.writeBlob(large);
        q.setDataPosition(0);
        Log.i(TAG, "a blob of 20000 bytes reads back the same: " + Arrays.equals(large, q.readBlob()));
        q.recycle();
    }

    static void interfaceToken() {
        // The token starts with the thread's StrictMode policy, set here so that it is the same on every device.
        StrictMode.ThreadPolicy policy = StrictMode.getThreadPolicy();
        StrictMode.setThreadPolicy(StrictMode.ThreadPolicy.LAX);
        try {
            Parcel p = Parcel.obtain();
            p.writeInterfaceToken("android.test.IParcelTest");
            p.writeInt(42);
            log("interface token", p);
            p.setDataPosition(0);
            p.enforceInterface("android.test.IParcelTest");
            Log.i(TAG, "enforceInterface takes its own, then " + p.readInt());
            p.setDataPosition(0);
            try {
                p.enforceInterface("android.test.IOther");
                Log.i(TAG, "enforceInterface takes another");
            } catch (SecurityException e) {
                Log.i(TAG, "enforceInterface refuses another: " + e.getMessage());
            }
            p.recycle();
        } finally {
            StrictMode.setThreadPolicy(policy);
        }
    }

    static void exceptions() {
        Parcel p = Parcel.obtain();
        p.writeException(new IllegalStateException("bad state"));
        p.writeNoException();
        log("exceptions", p);
        p.setDataPosition(0);
        try {
            p.readException();
            Log.i(TAG, "readException threw nothing");
        } catch (IllegalStateException e) {
            Log.i(TAG, "readException threw " + e);
        }
        p.readException();
        Log.i(TAG, "and no exception after it, position " + p.dataPosition());
        p.recycle();
    }

    static void bundle(Context context) {
        Bundle bundle = new Bundle();
        bundle.putInt("int", 7);
        bundle.putLong("long", -3L);
        bundle.putString("string", "text");
        bundle.putBoolean("boolean", true);
        bundle.putDouble("double", 0.5);
        bundle.putIntArray("ints", new int[] {1, 2});
        ArrayList<String> list = new ArrayList<>();
        list.add("a");
        list.add("b");
        bundle.putStringArrayList("list", list);
        bundle.putParcelable("rect", new Rect(1, 2, 3, 4));
        Bundle inner = new Bundle();
        inner.putString("inner", "value");
        bundle.putBundle("bundle", inner);
        Parcel p = Parcel.obtain();
        p.writeBundle(bundle);
        log("bundle", p);
        // Through bytes, as a saved state goes.
        byte[] bytes = p.marshall();
        p.recycle();
        Parcel q = Parcel.obtain();
        q.unmarshall(bytes, 0, bytes.length);
        q.setDataPosition(0);
        Bundle read = q.readBundle(context.getClassLoader());
        q.recycle();
        Log.i(TAG, "bundle read back: " + read.size() + " keys, int " + read.getInt("int") + ", long "
                + read.getLong("long") + ", string " + read.getString("string") + ", boolean "
                + read.getBoolean("boolean") + ", double " + read.getDouble("double") + ", ints "
                + Arrays.toString(read.getIntArray("ints")) + ", list " + read.getStringArrayList("list")
                + ", rect " + read.getParcelable("rect", Rect.class) + ", inner "
                + read.getBundle("bundle").getString("inner"));
    }

    static void spannedText() {
        SpannableString text = new SpannableString("Hello parcel");
        text.setSpan(new StyleSpan(Typeface.BOLD), 0, 5, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        text.setSpan(new ForegroundColorSpan(0xffff0000), 6, 12, Spanned.SPAN_INCLUSIVE_INCLUSIVE);
        text.setSpan(new UnderlineSpan(), 0, 12, 0);
        text.setSpan(new URLSpan("https://example.com/"), 6, 12, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        Parcel p = Parcel.obtain();
        TextUtils.writeToParcel(text, p, 0);
        TextUtils.writeToParcel("plain é", p, 0);
        log("spanned text", p);
        p.setDataPosition(0);
        CharSequence spanned = TextUtils.CHAR_SEQUENCE_CREATOR.createFromParcel(p);
        CharSequence plain = TextUtils.CHAR_SEQUENCE_CREATOR.createFromParcel(p);
        Log.i(TAG, "spanned text read back: " + quote(spanned) + " with " + spans(spanned) + "; " + quote(plain)
                + " with " + spans(plain));
        p.recycle();
    }

    static void appendFrom() {
        Parcel a = Parcel.obtain();
        a.writeInt(1);
        a.writeInt(2);
        a.writeInt(3);
        Parcel b = Parcel.obtain();
        b.writeInt(9);
        b.appendFrom(a, 4, 8);
        log("appendFrom", b);
        a.appendFrom(a, 0, 8);
        log("appendFrom itself", a);
        b.appendFrom(a, 1000, 0);
        Log.i(TAG, "appendFrom of nothing: size " + b.dataSize());
        try {
            b.appendFrom(a, 8, 100);
            Log.i(TAG, "appendFrom past the source's end: appended");
        } catch (IllegalArgumentException e) {
            Log.i(TAG, "appendFrom past the source's end: " + e);
        }
        a.recycle();
        b.recycle();
    }

    static void pastTheEnd() {
        Parcel p = Parcel.obtain();
        p.writeInt(5);
        p.setDataPosition(0);
        int first = p.readInt();
        int second = p.readInt();
        Log.i(TAG, "past the end: " + first + ", then " + second + " at position " + p.dataPosition() + ", a long "
                + p.readLong() + ", a string " + p.readString() + ", bytes " + Arrays.toString(p.createByteArray())
                + ", position " + p.dataPosition());
        p.setDataPosition(2);
        Log.i(TAG, "an int at 2 of 4 bytes: " + p.readInt() + ", position " + p.dataPosition());
        p.setDataPosition(12);
        Log.i(TAG, "a position beyond the data: size " + p.dataSize() + ", available " + p.dataAvail());
        p.writeInt(6);
        Log.i(TAG, "written there: size " + p.dataSize() + ", position " + p.dataPosition());
        p.setDataSize(2);
        Log.i(TAG, "cut to 2 bytes: size " + p.dataSize() + ", position " + p.dataPosition() + ", capacity "
                + p.dataCapacity());
        p.setDataCapacity(400);
        Log.i(TAG, "capacity 400: " + p.dataCapacity() + ", size " + p.dataSize());
        try {
            p.setDataSize(-1);
            Log.i(TAG, "setDataSize(-1): done");
        } catch (IllegalArgumentException e) {
            Log.i(TAG, "setDataSize(-1): " + e);
        }
        try {
            p.setDataCapacity(-1);
            Log.i(TAG, "setDataCapacity(-1): done");
        } catch (IllegalArgumentException e) {
            Log.i(TAG, "setDataCapacity(-1): " + e);
        }
        p.recycle();
    }

    static void unmarshalled() {
        // A string that claims more characters than the data holds.
        report("a string longer than the data", bytes(5, 0, 0, 0, 'a', 0, 'b', 0), p -> quote(p.readString()));
        // The largest length, and a negative one.
        report("a string of length 0x7fffffff", bytes(0xff, 0xff, 0xff, 0x7f, 0, 0, 0, 0), p -> quote(p.readString()));
        report("a string of length -2", bytes(0xfe, 0xff, 0xff, 0xff, 'a', 0, 0, 0), p -> quote(p.readString()));
        // A string without its terminator.
        report("a string without its terminator", bytes(2, 0, 0, 0, 'a', 0, 'b', 0, 'c', 0, 'd', 0),
                p -> quote(p.readString()));
        report("bytes longer than the data", bytes(9, 0, 0, 0, 1, 2, 3, 4), p -> Arrays.toString(p.createByteArray()));
        // Five bytes: unmarshall pads the data to eight.
        report("five bytes", bytes(1, 0, 0, 0, 9), p -> p.readInt() + " " + p.readInt());
        // A binder object that is no object: unmarshalled bytes hold none, whatever they say.
        int[] garbage = flatObject(0x73622a85, 0x113, 0x41414141, 0x42424242);
        report("a binder in unmarshalled bytes", concat(garbage, bytes(12, 0, 0, 0)),
                p -> String.valueOf(p.readStrongBinder()));
        report("a null binder", concat(flatObject(0x73622a85, 0x13, 0, 0), bytes(0, 0, 0, 0)),
                p -> String.valueOf(p.readStrongBinder()));
        report("a null binder with a stability", concat(flatObject(0x73622a85, 0x13, 0, 0), bytes(7, 0, 0, 0)),
                p -> String.valueOf(p.readStrongBinder()));
        report("a binder object cut short", Arrays.copyOf(garbage, 20), p -> String.valueOf(p.readStrongBinder()));
        report("a descriptor object", flatObject(0x66642a85, 0, 0, 1), p -> String.valueOf(p.readFileDescriptor()));
    }

    static void binders() {
        Binder binder = new Binder();
        Parcel p = Parcel.obtain();
        p.writeStrongBinder(null);
        log("a null binder", p);
        p.setDataPosition(0);
        Log.i(TAG, "a null binder reads back " + p.readStrongBinder() + ", position " + p.dataPosition());
        p.recycle();

        Parcel q = Parcel.obtain();
        q.writeInt(7);
        q.writeStrongBinder(binder);
        q.writeInt(8);
        Log.i(TAG, "a binder: size " + q.dataSize() + ", file descriptors " + q.hasFileDescriptors());
        try {
            q.marshall();
            Log.i(TAG, "marshall with a binder: bytes");
        } catch (RuntimeException e) {
            Log.i(TAG, "marshall with a binder: " + e);
        }
        q.setDataPosition(0);
        Log.i(TAG, "a binder read back: " + q.readInt() + ", the same binder " + (q.readStrongBinder() == binder)
                + ", " + q.readInt());
        q.setDataPosition(4);
        Log.i(TAG, "an int read over the binder: " + q.readInt() + ", position " + q.dataPosition());
        q.setDataPosition(4);
        Log.i(TAG, "a string read over the binder: " + q.readString() + ", position " + q.dataPosition());
        q.setDataPosition(28);
        Log.i(TAG, "the binder's stability: " + q.readInt());

        Parcel r = Parcel.obtain();
        r.appendFrom(q, 0, q.dataSize());
        q.recycle();
        r.setDataPosition(4);
        Log.i(TAG, "appended, then the first recycled: the same binder " + (r.readStrongBinder() == binder));
        r.setDataSize(16);
        r.setDataPosition(4);
        Log.i(TAG, "cut to 16 bytes: " + r.readStrongBinder() + ", position " + r.dataPosition());
        r.recycle();

        // An object whose offset lies past the data's size: appendFrom at a position beyond the data copies it
        // there, and the data's size grows only by the appended length.
        Parcel s = Parcel.obtain();
        s.writeStrongBinder(binder);
        Parcel t = Parcel.obtain();
        t.writeInt(1);
        t.setDataPosition(64);
        t.appendFrom(s, 0, s.dataSize());
        s.recycle();
        Log.i(TAG, "appended at 64: size " + t.dataSize() + ", position " + t.dataPosition());
        t.setDataPosition(64);
        Log.i(TAG, "an object past the data's size: " + t.readStrongBinder() + ", position " + t.dataPosition()
                + ", size " + t.dataSize() + ", file descriptors " + t.hasFileDescriptors());
        t.recycle();

        // An object list out of the data's order: a binder written before one that is there already. The search
        // from the list's hint finds it only once a plain read has sorted the list.
        Binder second = new Binder();
        Parcel order = Parcel.obtain();
        for (int i = 0; i < 16; i++) {
            order.writeInt(i);
        }
        order.setDataPosition(32);
        order.writeStrongBinder(binder);
        order.setDataPosition(0);
        order.writeStrongBinder(second);
        order.setDataPosition(0);
        boolean atOnce = order.readStrongBinder() == second;
        Log.i(TAG, "a binder written before another, read at once: the same " + atOnce + ", position "
                + order.dataPosition() + ", then " + order.readInt());
        order.setDataPosition(0);
        // A binder reads with its stability, so the int after it follows.
        boolean sameSecond = order.readStrongBinder() == second;
        int afterSecond = order.dataPosition();
        int seven = order.readInt();
        boolean sameFirst = order.readStrongBinder() == binder;
        Log.i(TAG, "read again: the same " + sameSecond + " up to " + afterSecond + ", then " + seven + ", the same "
                + sameFirst + " up to " + order.dataPosition() + ", then " + order.readInt());
        order.setDataPosition(36);
        Log.i(TAG, "an int read inside the object at 32: " + order.readInt() + ", position " + order.dataPosition());
        order.recycle();

        // Cut before its binder: setDataSize drops the object and releases it, and marshall() then works.
        Parcel cut = Parcel.obtain();
        cut.writeInt(1);
        cut.writeStrongBinder(binder);
        cut.writeInt(2);
        cut.setDataSize(4);
        Log.i(TAG, "cut before its binder: size " + cut.dataSize() + ", bytes " + hex(cut.marshall()));
        cut.recycle();
    }

    static void descriptors(Context context) throws IOException, ErrnoException {
        File file = new File(context.getCacheDir(), "parcel-test.txt");
        try (FileOutputStream out = new FileOutputStream(file)) {
            out.write("parcel".getBytes(StandardCharsets.UTF_8));
        }
        ParcelFileDescriptor source = ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY);
        // The lowest free descriptor, which the parcel's dup then takes.
        ParcelFileDescriptor probe = ParcelFileDescriptor.dup(source.getFileDescriptor());
        int lowest = probe.getFd();
        probe.close();

        Parcel p = Parcel.obtain();
        p.writeInt(1);
        p.writeFileDescriptor(source.getFileDescriptor());
        Log.i(TAG, "a descriptor: size " + p.dataSize() + ", file descriptors " + p.hasFileDescriptors()
                + ", in the first 4 bytes " + p.hasFileDescriptors(0, 4) + ", in the next 24 "
                + p.hasFileDescriptors(4, 24));
        try {
            p.marshall();
            Log.i(TAG, "marshall with a descriptor: bytes");
        } catch (RuntimeException e) {
            Log.i(TAG, "marshall with a descriptor: " + e);
        }
        p.setDataPosition(4);
        ParcelFileDescriptor copy = p.readFileDescriptor();
        byte[] read = new byte[16];
        int length;
        try (FileInputStream in = new FileInputStream(copy.getFileDescriptor())) {
            length = in.read(read);
        }
        copy.close();
        Log.i(TAG, "a descriptor read back: " + new String(read, 0, length, StandardCharsets.UTF_8)
                + ", the parcel's own open " + isOpen(lowest));
        p.recycle();
        Log.i(TAG, "after recycle the parcel's own is closed: " + !isOpen(lowest));

        // Cut before its descriptor: setDataSize drops the object and closes the parcel's own, and the parcel then
        // has no descriptor.
        probe = ParcelFileDescriptor.dup(source.getFileDescriptor());
        lowest = probe.getFd();
        probe.close();
        Parcel cut = Parcel.obtain();
        cut.writeInt(1);
        cut.writeFileDescriptor(source.getFileDescriptor());
        cut.writeInt(2);
        cut.setDataSize(4);
        Log.i(TAG, "cut before its descriptor: size " + cut.dataSize() + ", file descriptors " + cut.hasFileDescriptors()
                + ", the parcel's own closed " + !isOpen(lowest));
        cut.recycle();

        // A descriptor through appendFrom: the copy holds a dup of its own, which outlives the source's recycle.
        Parcel from = Parcel.obtain();
        from.writeFileDescriptor(source.getFileDescriptor());
        Parcel to = Parcel.obtain();
        to.appendFrom(from, 0, from.dataSize());
        from.recycle();
        to.setDataPosition(0);
        ParcelFileDescriptor appended = to.readFileDescriptor();
        // From the file's start: every dup shares the one file offset, which the read above moved to the end.
        length = Os.pread(appended.getFileDescriptor(), read, 0, read.length, 0);
        appended.close();
        Log.i(TAG, "a descriptor appended, its source recycled: " + new String(read, 0, length, StandardCharsets.UTF_8)
                + ", file descriptors " + to.hasFileDescriptors());
        to.recycle();

        // A descriptor object in unmarshalled bytes is no object of the parcel's: it neither reads nor owns the
        // number, so recycle leaves that file open, and its owner closes it (fdsan would stop a close by another).
        ParcelFileDescriptor other = ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY);
        Parcel forged = Parcel.obtain();
        forged.unmarshall(bytes(flatObject(0x66642a85, 0, other.getFd(), 1)), 0, 24);
        forged.setDataPosition(0);
        Log.i(TAG, "a descriptor object in unmarshalled bytes: " + forged.readFileDescriptor() + ", file descriptors "
                + forged.hasFileDescriptors());
        forged.recycle();
        Log.i(TAG, "after recycle its file is open " + isOpen(other));
        other.close();
        Log.i(TAG, "and its owner closes it");
        source.close();
        file.delete();
    }

    interface Read {
        String from(Parcel p);
    }

    static void report(String name, byte[] bytes, Read read) {
        Parcel p = Parcel.obtain();
        p.unmarshall(bytes, 0, bytes.length);
        int size = p.dataSize();
        p.setDataPosition(0);
        String value = read.from(p);
        Log.i(TAG, name + ": " + value + ", size " + size + ", position " + p.dataPosition());
        p.recycle();
    }

    static void log(String name, Parcel p) {
        Log.i(TAG, name + ": size " + p.dataSize() + ", position " + p.dataPosition() + ", capacity "
                + p.dataCapacity() + ", bytes " + hex(p.marshall()));
    }

    // Whether a descriptor is open: fromFd dups it without taking it from its owner. The parcel owns its own, which
    // libbinder tags for fdsan, so adopting it would end the process.
    static boolean isOpen(int fd) {
        try {
            ParcelFileDescriptor.fromFd(fd).close();
            return true;
        } catch (IOException e) {
            return false;
        }
    }

    // Whether a ParcelFileDescriptor's file is open, asked through its own descriptor.
    static boolean isOpen(ParcelFileDescriptor owner) {
        try {
            Os.fcntlInt(owner.getFileDescriptor(), OsConstants.F_GETFD, 0);
            return true;
        } catch (ErrnoException e) {
            return false;
        }
    }

    // A flat_binder_object: type, flags, then binder and cookie (64 bits each), little-endian.
    static int[] flatObject(int type, int flags, int binder, int cookie) {
        int[] object = new int[24];
        put(object, 0, type);
        put(object, 4, flags);
        put(object, 8, binder);
        put(object, 12, binder);
        put(object, 16, cookie);
        put(object, 20, cookie);
        return object;
    }

    static void put(int[] bytes, int at, int value) {
        for (int i = 0; i < 4; i++) {
            bytes[at + i] = (value >>> (8 * i)) & 0xff;
        }
    }

    static int[] concat(int[] a, byte[] b) {
        int[] all = Arrays.copyOf(a, a.length + b.length);
        for (int i = 0; i < b.length; i++) {
            all[a.length + i] = b[i] & 0xff;
        }
        return all;
    }

    static byte[] bytes(int... values) {
        byte[] bytes = new byte[values.length];
        for (int i = 0; i < values.length; i++) {
            bytes[i] = (byte) values[i];
        }
        return bytes;
    }

    static void report(String name, int[] values, Read read) {
        report(name, bytes(values), read);
    }

    static String hex(byte[] bytes) {
        StringBuilder hex = new StringBuilder();
        for (int i = 0; i < bytes.length; i++) {
            if (i > 0 && i % 4 == 0) {
                hex.append(' ');
            }
            hex.append(Character.forDigit((bytes[i] >> 4) & 0xf, 16)).append(Character.forDigit(bytes[i] & 0xf, 16));
        }
        return hex.toString();
    }

    static String quote(CharSequence text) {
        if (text == null) {
            return "null";
        }
        StringBuilder quoted = new StringBuilder("\"");
        for (int i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') {
                quoted.append(c);
            } else {
                quoted.append(String.format("\\u%04x", (int) c));
            }
        }
        return quoted.append('"').toString();
    }

    static String spans(CharSequence text) {
        if (!(text instanceof Spanned)) {
            return "no spans";
        }
        Spanned spanned = (Spanned) text;
        StringBuilder spans = new StringBuilder();
        for (Object span : spanned.getSpans(0, spanned.length(), Object.class)) {
            if (spans.length() > 0) {
                spans.append(", ");
            }
            spans.append(span.getClass().getSimpleName()).append(' ').append(spanned.getSpanStart(span)).append('-')
                    .append(spanned.getSpanEnd(span)).append(" flags ").append(spanned.getSpanFlags(span));
            if (span instanceof StyleSpan) {
                spans.append(" style ").append(((StyleSpan) span).getStyle());
            } else if (span instanceof ForegroundColorSpan) {
                spans.append(" color ").append(Integer.toHexString(((ForegroundColorSpan) span).getForegroundColor()));
            } else if (span instanceof URLSpan) {
                spans.append(" url ").append(((URLSpan) span).getURL());
            }
        }
        return spans.toString();
    }
}
