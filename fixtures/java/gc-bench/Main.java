// gc-bench: a Java allocation benchmark for ADR-0033's choice of collector, SS against CMS (decision 5). dalvikvm
// runs it as dalvikvm64 -Xint does on Android 15, and both print the same lines. Two phases, each a loop of frames,
// as a game allocates: every frame makes short-lived lists, byte arrays and two large int arrays, and replaces 1 % of
// a kept set of lists with payloads, about 17 MB in phase A and 68 MB in phase B. A generator with a fixed seed picks
// every size and place, so every run, collector and device makes the same objects and prints the same checksums.
// What the frames took, which differs from run to run, goes to stderr as "gc-bench stats: ..." lines: the frame
// times' median, 99th percentile and maximum, the frames over 8.3 ms and over 16.7 ms (120 and 60 Hz), and what the
// phase allocated per second. ART's own pauses come from -XX:DumpGCPerformanceOnShutdown, which a run may add.
import java.util.Arrays;

public class Main {
    static final int FRAMES = 5000;          // per phase
    static final int SLOTS_A = 2000;         // kept lists in phase A
    static final int SLOTS_B = 8000;         // kept lists in phase B
    static final int SLOT_NODES = 8;         // nodes per kept list
    static final int PAYLOAD = 1024;         // a kept node's payload: 512 to 1535 bytes
    static final int REPLACED_PERCENT = 1;   // of the kept lists, per frame
    static final int LISTS = 40;             // short-lived lists per frame
    static final int LIST_NODES = 50;        // nodes per short-lived list
    static final int ARRAYS = 100;           // short-lived byte arrays per frame, 1 to 8 KiB
    static final int LARGE = 2;              // short-lived int arrays per frame
    static final int LARGE_INTS = 16 * 1024; // 64 KiB, in the large-object space

    static final class Node {
        final Node next;
        final int value;
        final byte[] payload;

        Node(Node next, int value, byte[] payload) {
            this.next = next;
            this.value = value;
            this.payload = payload;
        }
    }

    // A linear congruential generator (Knuth's MMIX constants), the same on every VM.
    static long state = 0x2545F4914F6CDD1DL;

    static int next(int bound) {
        state = state * 6364136223846793005L + 1442695040888963407L;
        return (int) ((state >>> 33) % bound);
    }

    // What a phase made: objects and array bytes, both the same in every run.
    static long objects;
    static long arrayBytes;

    static Node keptList(int value) {
        Node list = null;
        for (int i = 0; i < SLOT_NODES; i++) {
            byte[] payload = new byte[PAYLOAD / 2 + next(PAYLOAD)];
            payload[0] = (byte) i;
            payload[payload.length - 1] = (byte) value;
            list = new Node(list, value + i, payload);
            objects += 2;
            arrayBytes += payload.length;
        }
        return list;
    }

    // One frame's work; its result keeps the short-lived objects from being dead code.
    static long frame(Node[] kept, int index) {
        long result = 0;
        for (int l = 0; l < LISTS; l++) {
            Node list = null;
            for (int i = 0; i < LIST_NODES; i++) {
                list = new Node(list, i, null);
            }
            result += list.value + list.next.value;
        }
        objects += LISTS * LIST_NODES;
        for (int a = 0; a < ARRAYS; a++) {
            byte[] array = new byte[1024 + next(7 * 1024)];
            array[array.length - 1] = (byte) a;
            result += array.length + array[array.length - 1];
            objects++;
            arrayBytes += array.length;
        }
        for (int a = 0; a < LARGE; a++) {
            int[] large = new int[LARGE_INTS];
            large[index % LARGE_INTS] = index;
            result += large[index % LARGE_INTS];
            objects++;
            arrayBytes += LARGE_INTS * 4L;
        }
        for (int r = kept.length * REPLACED_PERCENT / 100; r > 0; r--) {
            kept[next(kept.length)] = keptList(index * 131 + r);
        }
        return result;
    }

    static long checksum(Node[] kept) {
        long sum = 0;
        for (Node list : kept) {
            for (Node node = list; node != null; node = node.next) {
                sum = sum * 31 + node.value + node.payload.length + node.payload[0]
                        + node.payload[node.payload.length - 1];
            }
        }
        return sum;
    }

    static long payloadBytes(Node[] kept) {
        long bytes = 0;
        for (Node list : kept) {
            for (Node node = list; node != null; node = node.next) {
                bytes += node.payload.length;
            }
        }
        return bytes;
    }

    static String milliseconds(long nanoseconds) {
        return String.format("%.3f ms", nanoseconds / 1e6);
    }

    static void phase(String name, int slots) {
        objects = 0;
        arrayBytes = 0;
        long start = System.nanoTime();
        Node[] kept = new Node[slots];
        for (int s = 0; s < slots; s++) {
            kept[s] = keptList(s * SLOT_NODES);
        }
        long built = System.nanoTime();
        long[] times = new long[FRAMES];
        long result = 0;
        for (int f = 0; f < FRAMES; f++) {
            long before = System.nanoTime();
            result += frame(kept, f);
            times[f] = System.nanoTime() - before;
        }
        long end = System.nanoTime();
        System.out.println(name + ") " + slots + " kept lists of " + SLOT_NODES + " nodes, " + payloadBytes(kept)
                + " payload bytes; " + FRAMES + " frames made " + objects + " objects and " + arrayBytes
                + " array bytes; checksums " + Long.toHexString(checksum(kept)) + " " + Long.toHexString(result));
        long[] sorted = times.clone();
        Arrays.sort(sorted);
        int over120 = 0;
        int over60 = 0;
        for (long time : times) {
            if (time > 8_333_333L) over120++;
            if (time > 16_666_667L) over60++;
        }
        double seconds = (end - built) / 1e9;
        System.err.println("gc-bench stats: " + name + ") kept set built in " + milliseconds(built - start)
                + "; frames: median " + milliseconds(sorted[FRAMES / 2]) + ", p99 "
                + milliseconds(sorted[FRAMES * 99 / 100]) + ", max " + milliseconds(sorted[FRAMES - 1])
                + "; over 8.3 ms " + over120 + ", over 16.7 ms " + over60 + "; "
                + String.format("%.1f MB of arrays in %.2f s, %.1f MB/s", arrayBytes / 1e6, seconds,
                        arrayBytes / 1e6 / seconds));
    }

    public static void main(String[] args) {
        phase("A", SLOTS_A);
        phase("B", SLOTS_B);
        System.out.println("done");
    }
}
