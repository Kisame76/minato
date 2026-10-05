package io.github.kisame76.minato.test.parcelforge;

import android.app.Application;
import android.content.Context;
import android.os.Binder;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

/**
 * ADR-0043 d2's second test of android.os.Parcel, for the cases where Android's libbinder crashes or reaches past its
 * buffer, so that no Android log can be its reference: bytes written over a binder or a descriptor, a parcel appended
 * to itself as it grows, an append and a write beyond the data's end, compareData and marshall() with a position
 * beyond the data, and unmarshall with a range past its array. fixtures/expected/parcel-forge-test.txt holds the
 * answers of Minato's natives (minato_parcel.h), which stay inside the buffer and take objects only from the object
 * list. Each line goes to System.out with the prefix "parcel-forge-test: ", from Application.attachBaseContext; the
 * hidden calls are framework.jar's, which this app compiles against.
 */
public class ParcelForgeApplication extends Application {
    private static final String TAG = "parcel-forge-test";

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        try {
            report(base);
        } catch (Throwable e) {
            print("FAILED " + e);
            e.printStackTrace();
        }
        // The last line, after which the run on the Mac ends (framework/tests/java_app_run.py, step c2b).
        print("done");
    }

    static void print(String line) {
        System.out.println(TAG + ": " + line);
    }

    static void report(Context context) throws IOException, ErrnoException {
        writtenOverABinder();
        writtenOverADescriptor(context);
        appendedToItself();
        appendBeyondTheEnd();
        writeBeyondTheEnd();
        compareBeyondTheEnd();
        marshallBeyondTheEnd();
        unmarshallPastTheArray();
        unmarshallOverABinder();
        descriptorWhereNoneIsAllowed(context);
        appendedObjectWrittenOver();
        strings8();
    }

    // unmarshall keeps an object whose offset lies in the new data and writes the bytes over it: libbinder would take
    // the forged cookie for the binder's address. Minato's entry answers only to its own token.
    static void writtenOverABinder() {
        Binder binder = new Binder();
        Parcel p = Parcel.obtain();
        p.writeStrongBinder(binder);
        p.unmarshall(concat(flatObject(0x73622a85, 0x113, 0x41414141, 0x42424242), bytes(12, 0, 0, 0)), 0, 28);
        p.setDataPosition(0);
        print("bytes written over a binder: " + p.readStrongBinder() + ", position " + p.dataPosition()
                + ", objects kept, marshall " + marshallAnswer(p));
        p.recycle();
    }

    // The same over a descriptor, with another open descriptor's number: libbinder would read and close that one.
    static void writtenOverADescriptor(Context context) throws IOException, ErrnoException {
        File file = new File(context.getCacheDir(), "parcel-forge-test.txt");
        try (FileOutputStream out = new FileOutputStream(file)) {
            out.write("forge".getBytes(StandardCharsets.UTF_8));
        }
        ParcelFileDescriptor source = ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY);
        ParcelFileDescriptor other = ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY);
        // The lowest free descriptor, which the parcel's dup then takes.
        ParcelFileDescriptor probe = ParcelFileDescriptor.dup(source.getFileDescriptor());
        int own = probe.getFd();
        probe.close();
        Parcel p = Parcel.obtain();
        p.writeFileDescriptor(source.getFileDescriptor());
        p.unmarshall(flatObject(0x66642a85, 0, other.getFd(), 1), 0, 24);
        p.setDataPosition(0);
        print("bytes written over a descriptor: " + p.readFileDescriptor() + ", position " + p.dataPosition());
        p.recycle();
        print("after recycle the parcel's own is closed " + !isOpen(own) + ", the forged number open "
                + isOpen(other));
        other.close();
        source.close();
        file.delete();
    }

    // Appending to itself grows the buffer first: libbinder copies from the buffer it just reallocated.
    static void appendedToItself() {
        Parcel p = Parcel.obtain();
        for (int i = 0; i < 32; i++) {
            p.writeInt(i);
        }
        int capacity = p.dataCapacity();
        p.appendFrom(p, 0, 128);
        p.setDataPosition(128);
        StringBuilder copy = new StringBuilder();
        for (int i = 0; i < 32; i++) {
            copy.append(i == 0 ? "" : " ").append(p.readInt());
        }
        print("appended to itself from capacity " + capacity + ": size " + p.dataSize() + ", capacity "
                + p.dataCapacity() + ", the copy " + copy);
        p.recycle();
    }

    // libbinder makes room for the data's size, then copies at the position, past its buffer.
    static void appendBeyondTheEnd() {
        Parcel source = Parcel.obtain();
        source.writeInt(5);
        source.writeInt(6);
        Parcel p = Parcel.obtain();
        p.writeInt(1);
        p.setDataPosition(200);
        p.appendFrom(source, 0, 8);
        print("an append at 200 of capacity 128: size " + p.dataSize() + ", position " + p.dataPosition()
                + ", capacity " + p.dataCapacity());
        source.recycle();
        p.recycle();
    }

    // libbinder grows a write from the data's size and writes at the position, past its buffer.
    static void writeBeyondTheEnd() {
        Parcel p = Parcel.obtain();
        p.writeInt(1);
        p.setDataPosition(300);
        p.writeInt(7);
        p.setDataPosition(300);
        print("a write at 300 of capacity 128: " + p.readInt() + ", size " + p.dataSize() + ", capacity "
                + p.dataCapacity());
        p.recycle();
    }

    // dataSize() counts a position beyond the data, which libbinder's memcmp reads past both buffers.
    static void compareBeyondTheEnd() {
        Parcel a = Parcel.obtain();
        a.writeInt(1);
        a.setDataPosition(1000);
        Parcel b = Parcel.obtain();
        b.writeInt(1);
        b.setDataPosition(1000);
        Parcel c = Parcel.obtain();
        c.writeInt(2);
        c.setDataPosition(1000);
        print("compareData at position 1000: equal " + a.compareData(b) + ", unequal " + Integer.signum(a.compareData(c)));
        a.recycle();
        b.recycle();
        c.recycle();
    }

    // marshall() copies dataSize() bytes, which libbinder reads past its buffer, or from none.
    static void marshallBeyondTheEnd() {
        Parcel p = Parcel.obtain();
        p.writeInt(0x01020304);
        p.setDataPosition(200);
        byte[] bytes = p.marshall();
        print("marshall at position 200 of capacity 128: " + bytes.length + " bytes, " + hex(bytes, 4)
                + ", zero after them " + zeroFrom(bytes, 4));
        p.recycle();
        Parcel empty = Parcel.obtain();
        empty.setDataPosition(8);
        byte[] none = empty.marshall();
        print("marshall at position 8 of no data: " + none.length + " bytes, zero " + zeroFrom(none, 0));
        empty.recycle();
    }

    // AOSP's unmarshall copies from the array without a check of the range.
    static void unmarshallPastTheArray() {
        Parcel p = Parcel.obtain();
        try {
            p.unmarshall(new byte[4], 2, 8);
            print("unmarshall past the array: size " + p.dataSize());
        } catch (ArrayIndexOutOfBoundsException e) {
            print("unmarshall past the array: " + e.getMessage() + ", size " + p.dataSize());
        }
        p.recycle();
    }

    // unmarshall shrinks a parcel below its binder: Minato releases the object before it copies any bytes, outside a
    // critical section, where CheckJNI would stop the deletion of its global reference; AOSP's does it inside one.
    static void unmarshallOverABinder() {
        Parcel p = Parcel.obtain();
        p.writeInt(1);
        p.writeStrongBinder(new Binder());
        p.unmarshall(new byte[4], 0, 4);
        print("unmarshall over a binder: size " + p.dataSize() + ", marshall " + marshallAnswer(p));
        p.recycle();
    }

    // A descriptor where the parcel allows none (pushAllowFds, as a Bundle without FLAG_ALLOW_FDS writes): Android's
    // RuntimeException, nothing written, and the parcel's dup closed again.
    static void descriptorWhereNoneIsAllowed(Context context) throws IOException {
        File file = new File(context.getCacheDir(), "parcel-forge-test-fds.txt");
        try (FileOutputStream out = new FileOutputStream(file)) {
            out.write("fds".getBytes(StandardCharsets.UTF_8));
        }
        ParcelFileDescriptor source = ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY);
        ParcelFileDescriptor probe = ParcelFileDescriptor.dup(source.getFileDescriptor());
        int lowest = probe.getFd();
        probe.close();
        Parcel p = Parcel.obtain();
        boolean allowed = p.pushAllowFds(false);
        try {
            p.writeFileDescriptor(source.getFileDescriptor());
            print("a descriptor where none is allowed: written");
        } catch (RuntimeException e) {
            print("a descriptor where none is allowed: " + e.getMessage() + ", size " + p.dataSize()
                    + ", the dup closed " + !isOpen(lowest));
        }
        p.restoreAllowFds(allowed);
        p.recycle();
        source.close();
        file.delete();
    }

    // appendFrom copies an object only while its bytes are still its own: written over, they are plain data, where
    // libbinder would take their cookie for a binder's address.
    static void appendedObjectWrittenOver() {
        // Over its cookie (bytes 16 to 24), so that its type still says binder.
        appendWrittenOver("cookie", 16, 7, 7);
        // Over its type, which then says descriptor, with the cookie still the entry's.
        appendWrittenOver("type", 0, 0x66642a85);
    }

    static void appendWrittenOver(String what, int at, int... values) {
        Parcel p = Parcel.obtain();
        p.writeStrongBinder(new Binder());
        p.setDataPosition(at);
        for (int value : values) {
            p.writeInt(value);
        }
        Parcel q = Parcel.obtain();
        q.appendFrom(p, 0, p.dataSize());
        print("appended binder bytes whose " + what + " was written over: marshall " + marshallAnswer(q));
        q.recycle();
        p.recycle();
    }

    // writeString8 and readString8, hidden API on Android 15, where an app reaches them through TextUtils, whose
    // spanned text parcel-test compares with Android's: the length as ART's GetStringUTFLength counts it, the bytes of
    // GetStringUTFRegion and a terminator; -1 for null.
    static void strings8() {
        final String[] values = {null, "", "abc", "é漢😀", "a\u0000b"};
        Parcel p = Parcel.obtain();
        for (String value : values) {
            p.writeString8(value);
        }
        final byte[] bytes = p.marshall();
        print("strings8: size " + p.dataSize() + ", bytes " + hex(bytes, bytes.length));
        p.setDataPosition(0);
        StringBuilder read = new StringBuilder();
        for (int i = 0; i < values.length; i++) {
            final String value = p.readString8();
            read.append(' ').append(value == null ? "null" : escaped(value));
        }
        print("strings8 read back:" + read);
        p.recycle();
    }

    static String escaped(String text) {
        StringBuilder quoted = new StringBuilder("\"");
        for (int i = 0; i < text.length(); i++) {
            final char c = text.charAt(i);
            if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') {
                quoted.append(c);
            } else {
                quoted.append(String.format("\\u%04x", (int) c));
            }
        }
        return quoted.append('"').toString();
    }

    static String marshallAnswer(Parcel p) {
        try {
            return p.marshall().length + " bytes";
        } catch (RuntimeException e) {
            return "refused";
        }
    }

    // A descriptor that a ParcelFileDescriptor owns, asked through it: fdsan lets no second one adopt it.
    static boolean isOpen(ParcelFileDescriptor owner) {
        try {
            Os.fcntlInt(owner.getFileDescriptor(), OsConstants.F_GETFD, 0);
            return true;
        } catch (ErrnoException e) {
            return false;
        }
    }

    // A descriptor the parcel owns, asked through a dup that fromFd makes: the parcel tags its own for fdsan, as
    // libbinder does, so adopting it would end the process.
    static boolean isOpen(int fd) {
        try {
            ParcelFileDescriptor.fromFd(fd).close();
            return true;
        } catch (IOException e) {
            return false;
        }
    }

    static boolean zeroFrom(byte[] bytes, int from) {
        for (int i = from; i < bytes.length; i++) {
            if (bytes[i] != 0) {
                return false;
            }
        }
        return true;
    }

    // A flat_binder_object: type, flags, then binder (or a descriptor's number) and cookie, 64 bits each.
    static byte[] flatObject(int type, int flags, int binder, int cookie) {
        byte[] object = new byte[24];
        put(object, 0, type);
        put(object, 4, flags);
        put(object, 8, binder);
        put(object, 16, cookie);
        return object;
    }

    static void put(byte[] bytes, int at, int value) {
        for (int i = 0; i < 4; i++) {
            bytes[at + i] = (byte) (value >>> (8 * i));
        }
    }

    static byte[] concat(byte[] a, byte[] b) {
        byte[] all = new byte[a.length + b.length];
        System.arraycopy(a, 0, all, 0, a.length);
        System.arraycopy(b, 0, all, a.length, b.length);
        return all;
    }

    static byte[] bytes(int... values) {
        byte[] bytes = new byte[values.length];
        for (int i = 0; i < values.length; i++) {
            bytes[i] = (byte) values[i];
        }
        return bytes;
    }

    static String hex(byte[] bytes, int length) {
        StringBuilder hex = new StringBuilder();
        for (int i = 0; i < length; i++) {
            hex.append(Character.forDigit((bytes[i] >> 4) & 0xf, 16)).append(Character.forDigit(bytes[i] & 0xf, 16));
        }
        return hex.toString();
    }
}
