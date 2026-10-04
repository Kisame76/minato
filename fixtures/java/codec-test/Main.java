// codec-test: ART's reference codec at its edges (ADR-0033 test 2). dalvikvm runs it as dalvikvm64 -Xint does on
// Android 15, with its JNI library libcodec-test.so (fixtures/suite/libcodec-test.c), and both print the same lines.
// Each part holds references across explicit collections, at each of which SS moves every live object:
//   a) a graph of objects, and non-movable arrays up to the end of the non-moving space;
//   b) identity hash codes, weak and soft references, and finalizers;
//   c) JNI global, weak global and local references, with the collections called from native code;
//   d) classes found again through the class tables of two class loaders;
//   e) System.arraycopy, clone, and Unsafe's compare-and-swap on a reference field and on array elements;
//   f) a synchronized method's lock, which a stack walk reads from the method's frame (ADR-0033 test 3);
//   g) the library that answers the JNI library's sigaction: libsigchain.so, which linker64 puts first into every
//      namespace's global group, the class loader's of the JNI library too (ADR-0040 test 2, ADR-0033 e2b3).
// The offsets of the objects it saw, which differ by device, go to stderr as its last line; on Minato,
// runtime/tests/art_run.py --offsets checks them against the heap window.
import dalvik.system.PathClassLoader;
import dalvik.system.VMRuntime;
import java.lang.ref.ReferenceQueue;
import java.lang.ref.SoftReference;
import java.lang.ref.WeakReference;
import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicInteger;
import sun.misc.Unsafe;

public class Main {
    static final Unsafe UNSAFE = unsafe();
    static final long REF = fieldOffset(Holder.class, "ref");

    // The offsets seen so far, as unsigned 32-bit values.
    static long lowest = 0xffffffffL;
    static long highest = 0;

    static class Holder {
        Object ref;
    }

    static class Node implements Cloneable {
        final int index;
        Node next;
        Node other;

        Node(int index) {
            this.index = index;
        }

        @Override
        public Node clone() {
            try {
                return (Node) super.clone();
            } catch (CloneNotSupportedException e) {
                throw new AssertionError(e);
            }
        }
    }

    static class Finalizable {
        static final AtomicInteger finalized = new AtomicInteger();

        @Override
        protected void finalize() {
            finalized.incrementAndGet();
        }
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("codec-test");
        offsets();
        references();
        jni();
        classes();
        copies();
        stacks();
        sigchain();
        System.err.println(String.format("codec-test offsets: lowest 0x%08x, highest 0x%08x", lowest, highest));
    }

    // --- a) Objects across the window ---------------------------------------------------------------------

    static void offsets() {
        final int count = 100_000;
        Node[] nodes = new Node[count];
        for (int i = 0; i < count; i++) {
            nodes[i] = new Node(i);
        }
        for (int i = 0; i < count; i++) {
            nodes[i].next = nodes[(i + 1) % count];
            nodes[i].other = nodes[(int) (i * 7919L % count)];
        }
        collect(3);
        boolean intact = true;
        for (int i = 0; i < count; i++) {
            Node node = nodes[i];
            intact &= node.index == i && node.next.index == (i + 1) % count && node.other == nodes[(int) (i * 7919L % count)];
            if (i % 1000 == 0) {
                offset(node);
            }
        }
        print("a) a graph of " + count + " objects after 3 collections: intact " + yes(intact));

        // Non-movable arrays of references, ever smaller, until the non-moving space has no room left: the last
        // of them lie at its end, the highest offsets the heap has.
        VMRuntime runtime = VMRuntime.getRuntime();
        ArrayList<Object[]> arrays = new ArrayList<>();
        for (int length : new int[] {1 << 18, 1 << 14, 1 << 10, 1 << 6}) {
            try {
                while (true) {
                    arrays.add((Object[]) runtime.newNonMovableArray(Object.class, length));
                }
            } catch (OutOfMemoryError e) {
                // The next, smaller length fills what is left.
            }
        }
        final int filled = arrays.size();
        for (int k = 0; k < filled; k++) {
            Object[] array = arrays.get(k);
            array[0] = arrays.get((k + 1) % filled);
            array[array.length / 2] = String.valueOf(k);
            array[array.length - 1] = nodes[k % count];
            offset(array);
        }
        collect(3);
        boolean kept = true;
        for (int k = 0; k < filled; k++) {
            Object[] array = arrays.get(k);
            kept &= array[0] == arrays.get((k + 1) % filled) && String.valueOf(k).equals(array[array.length / 2])
                    && array[array.length - 1] == nodes[k % count];
        }
        print("a) non-movable arrays until the non-moving space was full: their references after 3 collections: kept "
                + yes(kept));
        arrays.clear();
        collect(1);
        boolean again;
        try {
            again = runtime.newNonMovableArray(Object.class, 1 << 18) != null;
        } catch (OutOfMemoryError e) {
            again = false;
        }
        print("a) a non-movable array once they were collected: allocated " + yes(again));
    }

    // --- b) Identity hash codes, weak and soft references, finalizers -------------------------------------------

    static void references() throws InterruptedException {
        final int count = 10_000;
        Object[] objects = new Object[count];
        int[] hashes = new int[count];
        for (int i = 0; i < count; i++) {
            objects[i] = new Object();
            hashes[i] = System.identityHashCode(objects[i]);
        }
        ReferenceQueue<Object> queue = new ReferenceQueue<>();
        WeakReference<Object>[] live = weakReferences(objects, queue);
        WeakReference<Object>[] dead = weakReferencesToNew(count, queue);
        SoftReference<Object>[] soft = softReferences(objects);
        collect(3);
        int same = 0, kept = 0, cleared = 0, softKept = 0;
        for (int i = 0; i < count; i++) {
            same += System.identityHashCode(objects[i]) == hashes[i] && objects[i].hashCode() == hashes[i] ? 1 : 0;
            kept += live[i].get() == objects[i] ? 1 : 0;
            cleared += dead[i].get() == null ? 1 : 0;
            softKept += soft[i].get() == objects[i] ? 1 : 0;
            if (i % 100 == 0) {
                offset(objects[i]);
            }
        }
        int enqueued = 0;
        while (enqueued < count && queue.remove(5000) != null) {
            enqueued++;
        }
        print("b) identity hash codes after 3 collections: the same " + same + " of " + count);
        print("b) weak references to live objects: kept " + kept + " of " + count + "; to dead ones: cleared " + cleared
                + " of " + count + ", enqueued " + enqueued);
        print("b) soft references to live objects: kept " + softKept + " of " + count);

        final int finalizable = 1000;
        makeFinalizable(finalizable);
        for (int round = 0; round < 20 && Finalizable.finalized.get() < finalizable; round++) {
            collect(1);
            System.runFinalization();
        }
        print("b) finalizers of " + finalizable + " objects no longer reachable: run " + Finalizable.finalized.get());
    }

    // Each helper makes its objects in a frame of its own, so that no register of the caller's frame keeps one.
    @SuppressWarnings("unchecked")
    static WeakReference<Object>[] weakReferences(Object[] objects, ReferenceQueue<Object> queue) {
        WeakReference<Object>[] references = new WeakReference[objects.length];
        for (int i = 0; i < objects.length; i++) {
            references[i] = new WeakReference<>(objects[i], queue);
        }
        return references;
    }

    @SuppressWarnings("unchecked")
    static WeakReference<Object>[] weakReferencesToNew(int count, ReferenceQueue<Object> queue) {
        WeakReference<Object>[] references = new WeakReference[count];
        for (int i = 0; i < count; i++) {
            references[i] = new WeakReference<>(new Object(), queue);
        }
        return references;
    }

    @SuppressWarnings("unchecked")
    static SoftReference<Object>[] softReferences(Object[] objects) {
        SoftReference<Object>[] references = new SoftReference[objects.length];
        for (int i = 0; i < objects.length; i++) {
            references[i] = new SoftReference<>(objects[i]);
        }
        return references;
    }

    static void makeFinalizable(int count) {
        for (int i = 0; i < count; i++) {
            new Finalizable();
        }
    }

    // --- c) JNI references ----------------------------------------------------------------------------------

    static native int globalReferences(Object[] objects);

    static native void weakGlobalReferences(Object[] objects, int dead, int[] result);

    static native int localReferences(Object[] objects);

    static native int nativeObjects(int count);

    static void jni() {
        final int count = 1000;
        Object[] objects = new Object[count];
        for (int i = 0; i < count; i++) {
            objects[i] = new Node(i);
        }
        print("c) JNI global references across 3 collections: the same object " + globalReferences(objects) + " of "
                + count);
        int[] weak = new int[2];
        weakGlobalReferences(objects, count, weak);
        print("c) JNI weak global references: to live objects kept " + weak[0] + " of " + count + "; to dead ones "
                + "cleared " + weak[1] + " of " + count);
        print("c) JNI local references across 3 collections: the same object " + localReferences(objects) + " of "
                + count);
        print("c) objects only JNI local references hold, after 3 collections: intact " + nativeObjects(count) + " of "
                + count);
        offset(objects[0]);
    }

    // --- d) Classes through the class tables ------------------------------------------------------------------

    static void classes() throws Exception {
        String[] names = {"Main", "Main$Node", "Main$Holder", "Main$Finalizable", "java.lang.String",
                "java.util.ArrayList", "java.util.HashMap", "java.lang.ref.WeakReference", "dalvik.system.VMRuntime",
                "sun.misc.Unsafe"};
        ClassLoader app = Main.class.getClassLoader();
        Class<?>[] before = new Class<?>[names.length];
        for (int i = 0; i < names.length; i++) {
            before[i] = Class.forName(names[i], false, app);
        }
        // A second loader over the same jar defines a Main$Node of its own, in a class table of its own.
        ClassLoader second = new PathClassLoader(System.getProperty("java.class.path"), null);
        Class<?> secondNode = second.loadClass("Main$Node");
        Class<?> nodeArray = Node[].class, nodeMatrix = Node[][].class, intMatrix = int[][].class;
        collect(3);
        int same = 0;
        for (int i = 0; i < names.length; i++) {
            same += Class.forName(names[i], false, app) == before[i] ? 1 : 0;
            offset(before[i]);
        }
        print("d) classes found again by name after 3 collections: the same " + same + " of " + names.length);
        print("d) a second class loader's Main$Node: its own " + yes(secondNode != Node.class
                && secondNode.getClassLoader() == second) + ", found again " + yes(second.loadClass("Main$Node") == secondNode));
        print("d) array classes after 3 collections: the same " + yes(Class.forName("[LMain$Node;", false, app) == nodeArray
                && Class.forName("[[LMain$Node;", false, app) == nodeMatrix && Class.forName("[[I", false, app) == intMatrix));
    }

    // --- e) Copies and compare-and-swap -------------------------------------------------------------------------

    static void copies() {
        final int count = 1000;
        Object[] source = new Object[count];
        for (int i = 0; i < count; i++) {
            source[i] = i % 10 == 0 ? null : new Node(i);
        }
        Object[] copy = new Object[count];
        System.arraycopy(source, 0, copy, 0, count);
        Object[] forward = source.clone();
        System.arraycopy(forward, 0, forward, 1, count - 1);
        Object[] backward = source.clone();
        System.arraycopy(backward, 1, backward, 0, count - 1);
        Object[] mixed = {"a", Integer.valueOf(1), "b"};
        String[] strings = new String[3];
        boolean refused = false;
        try {
            System.arraycopy(mixed, 0, strings, 0, 3);
        } catch (ArrayStoreException e) {
            refused = true;
        }
        Node node = new Node(1);
        node.next = new Node(2);
        node.other = node;
        Node cloned = node.clone();
        Object[] clonedArray = source.clone();
        collect(3);
        boolean copied = true, shifted = true, cloneSame = clonedArray != source;
        for (int i = 0; i < count; i++) {
            copied &= copy[i] == source[i];
            shifted &= forward[i] == source[i == 0 ? 0 : i - 1] && backward[i] == source[i == count - 1 ? i : i + 1];
            cloneSame &= clonedArray[i] == source[i];
        }
        print("e) System.arraycopy after 3 collections: copied " + yes(copied) + ", overlapping both ways " + yes(shifted)
                + ", into String[] up to the Integer " + yes(refused && strings[0] == mixed[0] && strings[1] == null
                && strings[2] == null));
        print("e) clone after 3 collections: an array " + yes(cloneSame) + ", an object " + yes(cloned != node
                && cloned.index == 1 && cloned.next == node.next && cloned.other == node));
        offset(source[1]);

        Holder holder = new Holder();
        Object a = new Object(), b = new Object();
        holder.ref = a;
        boolean field = UNSAFE.compareAndSwapObject(holder, REF, a, b) && holder.ref == b
                && !UNSAFE.compareAndSwapObject(holder, REF, a, null) && holder.ref == b;
        collect(1);
        field &= UNSAFE.compareAndSwapObject(holder, REF, b, a) && holder.ref == a;
        long base = UNSAFE.arrayBaseOffset(Object[].class), scale = UNSAFE.arrayIndexScale(Object[].class);
        Object[] slots = new Object[16];
        slots[5] = a;
        boolean elements = UNSAFE.compareAndSwapObject(slots, base + 5 * scale, a, b) && slots[5] == b
                && !UNSAFE.compareAndSwapObject(slots, base + 6 * scale, a, b) && slots[6] == null
                && UNSAFE.compareAndSwapObject(slots, base + 6 * scale, null, a) && slots[6] == a;
        collect(1);
        elements &= UNSAFE.getObject(slots, base + 5 * scale) == b && UNSAFE.compareAndSwapObject(slots, base + 6 * scale, a, null)
                && slots[6] == null;
        print("e) Unsafe compare-and-swap across collections: a reference field " + yes(field) + ", array elements "
                + yes(elements));
        offset(a);
        offset(b);
    }

    // --- f) Stack walks ---------------------------------------------------------------------------------------

    static class Lockable {
        // Holds this object's monitor, then blocks on `other`, which the main thread holds.
        synchronized void hold(Object other, CountDownLatch inside) {
            inside.countDown();
            synchronized (other) {
                other.hashCode();
            }
        }
    }

    // A synchronized method's lock, which a stack walk takes from the method's frame, as an annotated stack trace
    // lists it after the lock moved.
    static void stacks() throws Exception {
        final Lockable lockable = new Lockable();
        final Object other = new Object();
        final CountDownLatch inside = new CountDownLatch(1);
        Thread thread = new Thread(new Runnable() {
            @Override
            public void run() {
                lockable.hold(other, inside);
            }
        });
        boolean found = false;
        synchronized (other) {
            thread.start();
            inside.await();
            while (thread.getState() != Thread.State.BLOCKED) {
                Thread.sleep(10);
            }
            collect(3);
            Object[] frames = (Object[]) Class.forName("dalvik.system.VMStack")
                    .getDeclaredMethod("getAnnotatedThreadStackTrace", Thread.class).invoke(null, thread);
            for (Object frame : frames) {
                StackTraceElement element =
                        (StackTraceElement) frame.getClass().getDeclaredMethod("getStackTraceElement").invoke(frame);
                Object[] locks = (Object[]) frame.getClass().getDeclaredMethod("getHeldLocks").invoke(frame);
                if (element.getMethodName().equals("hold") && locks != null) {
                    found |= Arrays.asList(locks).contains(lockable);
                }
            }
        }
        thread.join();
        print("f) a synchronized method's lock in an annotated stack trace after 3 collections: found " + yes(found));
        offset(lockable);
    }

    // --- g) The JNI library's sigaction -----------------------------------------------------------------------

    static native String sigactionLibrary();

    static void sigchain() {
        print("g) the JNI library's sigaction, from " + sigactionLibrary());
    }

    // --- Helpers ----------------------------------------------------------------------------------------------

    // Called from libcodec-test too.
    static void collect(int times) {
        for (int i = 0; i < times; i++) {
            Runtime.getRuntime().gc();
        }
    }

    // The offset ART keeps for `object`, the reference as a field holds it, noted for the last line.
    static long offset(Object object) {
        Holder holder = new Holder();
        holder.ref = object;
        long offset = UNSAFE.getInt(holder, REF) & 0xffffffffL;
        lowest = Math.min(lowest, offset);
        highest = Math.max(highest, offset);
        return offset;
    }

    static void print(String line) {
        System.out.println(line);
    }

    static String yes(boolean value) {
        return value ? "yes" : "no";
    }

    static Unsafe unsafe() {
        try {
            Field field = Unsafe.class.getDeclaredField("theUnsafe");
            field.setAccessible(true);
            return (Unsafe) field.get(null);
        } catch (ReflectiveOperationException e) {
            throw new AssertionError(e);
        }
    }

    static long fieldOffset(Class<?> type, String name) {
        try {
            return UNSAFE.objectFieldOffset(type.getDeclaredField(name));
        } catch (NoSuchFieldException e) {
            throw new AssertionError(e);
        }
    }
}
