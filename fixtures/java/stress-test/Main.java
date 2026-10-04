// stress-test: ART under stress (ADR-0033 test 4). dalvikvm runs it as dalvikvm64 -Xint does on Android 15, with its
// JNI library libstress-test.so (fixtures/suite/libstress-test.c), and both print the same lines. Three parts run at
// once, while the main thread calls for one collection after another (with SS, each stops the world and moves every
// live object):
//   a) threads, monitors and allocation: eight threads build lists with identity hashes and large arrays, keep them
//      across collections and check them; they count under shared monitors, which they also allocate under, a
//      ReentrantLock and an AtomicLong; and pairs of them hand tokens over through wait and notify;
//   b) a guest signal handler while ART stops the world: SIGUSR2 to part a's threads and to the collecting main
//      thread, one at a time, each handled;
//   c) Boehm's stop-the-world with SIGPWR and SIGXCPU among six native threads, next to ART's collections: four are
//      attached to the VM, and one waits in Java while three call into Java between their stops.
// Parts b and c run until part a is over, each at least to its minimum. What they saw, which differs from run to
// run, goes to stderr as "stress-test stats: ..."; on Minato, runtime/tests/art_run.py --stress checks it.
import java.lang.ref.WeakReference;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.locks.ReentrantLock;

public class Main {
    static final int WORKERS = 8;
    static final int ROUNDS = 60;
    static final int NODES = 1000;       // per list
    static final int KEPT = 4;           // lists a thread keeps across rounds
    static final int LARGE_EVERY = 100;  // every 100th node's payload is large
    static final int LARGE = 16 * 1024;  // ints: 64 KiB, in the large-object space
    static final int HASH_EVERY = 10;    // every 10th node keeps its identity hash
    static final int LOCKS = 4;
    static final int INCREMENTS = 40;    // per thread and round, a multiple of LOCKS
    static final int HANDOFFS = 50;      // tokens per pair, round and direction
    static final int PAUSE_MS = 5;       // between the main thread's collections
    // Parts b's and c's minimums (libstress-test.c), and the threads of part c.
    static final int SIGNALS = 2000;
    static final int STOPS = 300;
    static final int BOEHM_THREADS = 6;
    static final int BOEHM_ATTACHED = 4;
    // What Main.results() holds, in its order.
    static final int RESULT_SENT = 0;
    static final int RESULT_STOPS = 1;
    static final int RESULT_MOVED = 2;
    static final int RESULT_UNBLOCKED = 3;
    static final int RESULT_LATE = 4;
    static final int RESULT_PENDING = 5;
    static final int RESULT_FOUND_COMPUTING = 6;
    static final int RESULT_FOUND_WAITING = 7;
    static final int RESULT_FOUND_IN_JAVA = 8;
    static final int RESULT_JAVA_CALLS = 9;
    static final int RESULT_JAVA_WRONG = 10;

    static final Object[] locks = new Object[LOCKS];
    static final int[] lockHashes = new int[LOCKS];
    static final long[] counts = new long[LOCKS];  // each guarded by its lock
    static final ReentrantLock reentrant = new ReentrantLock();
    static long reentrantCount;                    // guarded by reentrant
    static final AtomicLong atomic = new AtomicLong();
    static final Mailbox[] mailboxes = new Mailbox[WORKERS];
    static final Object waiting = new Object();

    public static void main(String[] args) throws Exception {
        System.loadLibrary("stress-test");
        init();
        registerTarget();
        for (int i = 0; i < LOCKS; i++) {
            locks[i] = new Object();
            lockHashes[i] = System.identityHashCode(locks[i]);
        }
        Worker[] workers = new Worker[WORKERS];
        for (int i = 0; i < WORKERS; i++) {
            mailboxes[i] = new Mailbox();
            workers[i] = new Worker(i);
        }
        startNative();
        for (Worker worker : workers) {
            worker.start();
        }
        int collections = 0;
        boolean ending = false;
        while (failure() == null) {
            if (!ending && !anyAlive(workers)) {
                stopNative();
                ending = true;
            }
            if (ending && nativeDone()) {
                break;
            }
            collecting(true);
            Runtime.getRuntime().gc();
            collecting(false);
            collections++;
            Thread.sleep(PAUSE_MS);
        }
        String failure = failure();
        if (failure != null) {
            print("FAIL: " + failure);
            System.out.flush();
            die(1);
        }
        for (Worker worker : workers) {
            worker.join();
        }
        printA(workers);
        printBC(results());
        print("done");
        writeStats(collections);
    }

    // --- a) Threads, monitors and allocation ------------------------------------------------------------------

    static final class Node {
        final int value;
        final int[] payload;
        final Node next;
        int hash;

        Node(int value, int[] payload, Node next) {
            this.value = value;
            this.payload = payload;
            this.next = next;
        }
    }

    // A box for one token at a time, handed over through wait and notify.
    static final class Mailbox {
        private int token;
        private boolean full;

        synchronized void put(int value) throws InterruptedException {
            while (full) {
                wait();
            }
            token = value;
            full = true;
            notifyAll();
        }

        synchronized int take() throws InterruptedException {
            while (!full) {
                wait();
            }
            full = false;
            notifyAll();
            return token;
        }
    }

    static final class Worker extends Thread {
        final int index;
        String problem;  // the first one, or null
        boolean tokensInPlace = true;
        Object garbage;

        Worker(int index) {
            this.index = index;
        }

        @Override
        public void run() {
            registerTarget();
            try {
                Node[] kept = new Node[KEPT];
                int[] keptRounds = new int[KEPT];
                WeakReference<?>[] weak = new WeakReference<?>[KEPT];
                int[] weakRounds = new int[KEPT];
                for (int round = 0; round < ROUNDS; round++) {
                    int slot = round % KEPT;
                    if (kept[slot] != null) {
                        weak[slot] = new WeakReference<>(kept[slot]);
                        weakRounds[slot] = keptRounds[slot];
                    }
                    kept[slot] = build(index, round);
                    keptRounds[slot] = round;
                    count();
                    handOver(round);
                    for (int k = 0; k < KEPT; k++) {
                        if (kept[k] != null) {
                            note(check(kept[k], index, keptRounds[k]));
                        }
                        Object old = weak[k] == null ? null : weak[k].get();
                        if (old != null) {
                            note(check((Node) old, index, weakRounds[k]));
                        }
                    }
                }
            } catch (Throwable t) {
                note("thread " + index + " threw " + t);
            } finally {
                unregisterTarget();
            }
        }

        void note(String found) {
            if (problem == null) {
                problem = found;
            }
        }

        void count() {
            for (int k = 0; k < INCREMENTS; k++) {
                int which = (index + k) % LOCKS;
                synchronized (locks[which]) {
                    counts[which]++;
                    if (k % 8 == 0) {
                        garbage = new int[64];
                    }
                }
                if (k % 4 == 0) {
                    reentrant.lock();
                    try {
                        reentrantCount++;
                    } finally {
                        reentrant.unlock();
                    }
                }
                atomic.incrementAndGet();
            }
        }

        // Even threads hand their tokens to their odd partner first, then take the partner's.
        void handOver(int round) throws InterruptedException {
            int partner = index ^ 1;
            if (index % 2 == 0) {
                give(round);
                take(partner, round);
            } else {
                take(partner, round);
                give(round);
            }
        }

        void give(int round) throws InterruptedException {
            for (int j = 0; j < HANDOFFS; j++) {
                mailboxes[index].put(value(index, round, j));
            }
        }

        void take(int partner, int round) throws InterruptedException {
            for (int j = 0; j < HANDOFFS; j++) {
                int token = mailboxes[partner].take();
                if (token != value(partner, round, j)) {
                    tokensInPlace = false;
                }
            }
        }
    }

    static int value(int worker, int round, int i) {
        return worker * 1000003 + round * 1009 + i;
    }

    static int size(int i) {
        return i % LARGE_EVERY == 0 ? LARGE : 2 + i % 30;
    }

    static Node build(int worker, int round) {
        Node head = null;
        for (int i = NODES - 1; i >= 0; i--) {
            int value = value(worker, round, i);
            int[] payload = new int[size(i)];
            payload[0] = value;
            payload[payload.length - 1] = ~value;
            head = new Node(value, payload, head);
            if (i % HASH_EVERY == 0) {
                head.hash = System.identityHashCode(head);
            }
        }
        return head;
    }

    // What is wrong with the list `worker` built in `round`, or null.
    static String check(Node head, int worker, int round) {
        int i = 0;
        for (Node node = head; node != null; node = node.next, i++) {
            int value = value(worker, round, i);
            String where = "node " + i + " of thread " + worker + "'s round " + round;
            if (node.value != value) {
                return where + " holds " + node.value;
            }
            int[] payload = node.payload;
            if (payload.length != size(i) || payload[0] != value || payload[payload.length - 1] != ~value) {
                return where + " has another payload";
            }
            if (i % HASH_EVERY == 0 && node.hash != System.identityHashCode(node)) {
                return where + " changed its identity hash";
            }
        }
        return i == NODES ? null : "thread " + worker + "'s round " + round + " has " + i + " nodes";
    }

    static boolean anyAlive(Worker[] workers) {
        for (Worker worker : workers) {
            if (worker.isAlive()) {
                return true;
            }
        }
        return false;
    }

    static void printA(Worker[] workers) {
        String problem = null;
        boolean tokensInPlace = true;
        for (Worker worker : workers) {
            if (problem == null) {
                problem = worker.problem;
            }
            tokensInPlace &= worker.tokensInPlace;
        }
        print("a) " + WORKERS + " threads, " + ROUNDS + " rounds each: the lists they kept across collections, with "
                + "identity hashes and large arrays, and those weak references still held: "
                + (problem == null ? "intact" : problem));
        long each = (long) WORKERS * ROUNDS * INCREMENTS / LOCKS;
        boolean exact = reentrantCount == (long) WORKERS * ROUNDS * INCREMENTS / 4
                && atomic.get() == (long) WORKERS * ROUNDS * INCREMENTS;
        boolean hashes = true;
        for (int i = 0; i < LOCKS; i++) {
            exact &= counts[i] == each;
            hashes &= System.identityHashCode(locks[i]) == lockHashes[i];
        }
        print("a) counts under " + LOCKS + " monitors, a ReentrantLock and an AtomicLong: " + (exact ? "exact" : "off"));
        print("a) the monitors kept their identity hashes: " + yes(hashes));
        print("a) " + WORKERS * ROUNDS * HANDOFFS + " tokens handed over through wait and notify, each in its place: "
                + yes(tokensInPlace));
    }

    static void printBC(int[] results) {
        print("b) SIGUSR2 to part a's threads, and to the main thread while it collects: " + SIGNALS
                + " signals or more, each handled: " + yes(results[RESULT_SENT] >= SIGNALS));
        print("c) Boehm's stop-the-world among " + BOEHM_THREADS + " native threads, " + BOEHM_ATTACHED
                + " of them attached to the VM: " + STOPS + " stops or more, each with every other thread stopped "
                + "and restarted: " + yes(results[RESULT_STOPS] >= STOPS));
        print("c) the world stood still in each stop: " + yes(results[RESULT_MOVED] == 0));
        print("c) SIGXCPU stayed blocked in the stop handler, also after sigsuspend: "
                + yes(results[RESULT_UNBLOCKED] == 0));
        print("c) a restart that came before the handler's sigsuspend waited for it, pending: "
                + yes(results[RESULT_LATE] > 0 && results[RESULT_PENDING] == results[RESULT_LATE]));
        print("c) the stops found threads computing, waiting and in Java: " + yes(results[RESULT_FOUND_COMPUTING] > 0
                && results[RESULT_FOUND_WAITING] > 0 && results[RESULT_FOUND_IN_JAVA] > 0));
        print("c) the calls into Java between the stops returned their sums: "
                + yes(results[RESULT_JAVA_CALLS] > 0 && results[RESULT_JAVA_WRONG] == 0));
    }

    // --- c) What libstress-test's attached threads call -------------------------------------------------------

    // Between their stops: a short list, summed under one of part a's monitors.
    static int boehmWork(int n) {
        Node head = null;
        for (int i = 63; i >= 0; i--) {
            head = new Node(n + i, null, head);
        }
        int sum = 0;
        synchronized (locks[n % LOCKS]) {
            for (Node node = head; node != null; node = node.next) {
                sum += node.value;
            }
        }
        return sum;
    }

    // Where most stops find the thread that waits in Java: in a timed wait on a monitor.
    static void waitAWhile() {
        synchronized (waiting) {
            try {
                waiting.wait(2);
            } catch (InterruptedException e) {
                throw new AssertionError(e);
            }
        }
    }

    // --- Helpers ----------------------------------------------------------------------------------------------

    static void print(String line) {
        System.out.println(line);
    }

    static String yes(boolean value) {
        return value ? "yes" : "no";
    }

    static native void init();

    static native void registerTarget();

    static native void unregisterTarget();

    static native void collecting(boolean on);

    static native void startNative();

    static native void stopNative();

    static native boolean nativeDone();

    static native String failure();

    static native int[] results();

    static native void writeStats(int collections);

    static native void die(int status);
}
