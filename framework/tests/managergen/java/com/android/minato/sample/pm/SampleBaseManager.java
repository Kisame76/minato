/*
 * A sample of an AOSP abstract class for tools/managergen's --implement (ADR-0039 b3.5a), with PackageManager's
 * shapes: abstract methods with annotations, generics, varargs, arrays, throws, hidden and deprecated ones; nested
 * types whose abstract methods are not the outer class's; and concrete methods a subclass may override.
 */
package com.android.minato.sample.pm;

import static java.lang.annotation.RetentionPolicy.SOURCE;

import java.io.File;
import java.lang.annotation.Retention;
import java.util.List;

public abstract class SampleBaseManager {
    /** A '{' in a comment, and an abstract method in a string: "public abstract void notOne();". */
    public static final String TEXT = "public abstract void notOne(); {";

    public static final int FLAG_A = 1 << 0;

    private static final Listener QUIET = name -> {
        if (name.isEmpty()) {
            throw new IllegalArgumentException("(");
        }
    };

    static {
        if (QUIET == null) {
            throw new AssertionError();
        }
    }

    public SampleBaseManager() {
    }

    /** Thrown for an unknown package, as PackageManager's NameNotFoundException. */
    public static class NotFoundException extends Exception {
        private static final long serialVersionUID = 1;

        public NotFoundException(String name) {
            super(name);
        }
    }

    /** Its abstract method is not SampleBaseManager's. */
    public abstract static class Callback {
        public abstract void onDone(int code);
    }

    public interface Listener {
        void onChange(String name);
    }

    @Retention(SOURCE)
    public @interface Flags {
        int[] value() default {1, 2};
    }

    public abstract void clear(String name);

    public abstract boolean hasFeature(@Flags({3, 4}) String name, int version);

    public abstract SampleInfo getInfo(String name, @Flags int flags) throws NotFoundException;

    public abstract List<SampleInfo> getInfos(int flags);

    /**
     * @hide
     */
    @SuppressWarnings("HiddenAbstractMethod")
    public abstract SampleInfo getInfoAsUser(String name, int flags, int userId)
            throws NotFoundException, SecurityException;

    @Deprecated
    public abstract int getOld(final String name);

    public abstract <T extends SampleInfo> T first(Class<T> type, String... names);

    public abstract void register(Listener listener, Callback callback);

    public abstract long[] sizes(File[] files);

    protected abstract char letter();

    /** Concrete: SampleAppManager's hand file overrides it. */
    public SampleInfo getInfo(String name, long flags) throws NotFoundException {
        throw new UnsupportedOperationException("getInfo not implemented in subclass");
    }

    public int count() {
        return TEXT.length() > 0 ? FLAG_A : 0;
    }
}
