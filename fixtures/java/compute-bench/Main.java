import java.util.ArrayList;
import java.util.HashMap;

/**
 * A CPU-bound program for the compiled-against-interpreted measure (M6 c3, tools/compiled-apps-check.sh): integer
 * loops, array and matrix work in exact arithmetic, virtual and interface calls, strings, collections and
 * allocation, each part about a second in nterp. It prints one checksum per part and no times, so the
 * output is the same on every engine.
 */
public class Main {
    interface Op {
        int apply(int value);
    }

    static final class Add implements Op {
        final int amount;

        Add(int amount) {
            this.amount = amount;
        }

        public int apply(int value) {
            return value + amount;
        }
    }

    static final class Mix implements Op {
        public int apply(int value) {
            return (value ^ (value >>> 7)) * 31;
        }
    }

    static abstract class Shape {
        abstract long area();
    }

    static final class Rect extends Shape {
        final int w, h;

        Rect(int w, int h) {
            this.w = w;
            this.h = h;
        }

        long area() {
            return (long) w * h;
        }
    }

    static final class Square extends Shape {
        final int side;

        Square(int side) {
            this.side = side;
        }

        long area() {
            return (long) side * side;
        }
    }

    static int sieve(int limit) {
        boolean[] composite = new boolean[limit + 1];
        int count = 0;
        for (int i = 2; i <= limit; i++) {
            if (!composite[i]) {
                count++;
                for (long j = (long) i * i; j <= limit; j += i) {
                    composite[(int) j] = true;
                }
            }
        }
        return count;
    }

    static long matrix(int n, int rounds) {
        double[][] a = new double[n][n], b = new double[n][n], c = new double[n][n];
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < n; j++) {
                a[i][j] = (i * 3 + j) % 17;
                b[i][j] = (i + j * 5) % 13;
            }
        }
        long sum = 0;
        for (int r = 0; r < rounds; r++) {
            for (int i = 0; i < n; i++) {
                for (int j = 0; j < n; j++) {
                    double s = 0;
                    for (int k = 0; k < n; k++) {
                        s += a[i][k] * b[k][j];
                    }
                    c[i][j] = s;
                }
            }
            sum += (long) c[r % n][(r * 7) % n];
            a[r % n][r % n] += 1;
        }
        return sum;
    }

    static int calls(int rounds) {
        Op[] ops = {new Add(3), new Mix(), new Add(-11), new Mix()};
        Shape[] shapes = {new Rect(3, 4), new Square(5), new Rect(7, 2), new Square(9)};
        int value = 1;
        long area = 0;
        for (int i = 0; i < rounds; i++) {
            value = ops[i & 3].apply(value);
            area += shapes[(i >> 2) & 3].area();
        }
        return value ^ (int) area ^ (int) (area >>> 32);
    }

    static int strings(int rounds) {
        StringBuilder text = new StringBuilder();
        int hash = 0;
        for (int i = 0; i < rounds; i++) {
            text.setLength(0);
            text.append("item-").append(i).append(':').append(i * 31 % 97);
            String s = text.toString();
            hash = hash * 31 + s.hashCode() + s.indexOf(':') + s.charAt(s.length() - 1);
        }
        return hash;
    }

    static long collections(int rounds) {
        HashMap<Integer, Integer> map = new HashMap<>();
        ArrayList<Integer> list = new ArrayList<>();
        for (int i = 0; i < rounds; i++) {
            map.merge(i % 1021, i, Integer::sum);
            list.add(i ^ (i >> 3));
        }
        long sum = 0;
        for (int v : list) {
            sum += v;
        }
        for (int v : map.values()) {
            sum ^= v;
        }
        return sum + map.size();
    }

    static final class Node {
        final Node next;
        final int value;

        Node(Node next, int value) {
            this.next = next;
            this.value = value;
        }
    }

    static long allocation(int rounds) {
        long sum = 0;
        for (int r = 0; r < rounds; r++) {
            Node head = null;
            for (int i = 0; i < 100; i++) {
                head = new Node(head, i + r);
            }
            for (Node n = head; n != null; n = n.next) {
                sum += n.value;
            }
        }
        return sum;
    }

    public static void main(String[] args) {
        System.out.println("sieve: " + sieve(8000000));
        System.out.println("matrix: " + matrix(64, 40));
        System.out.println("calls: " + calls(20000000));
        System.out.println("strings: " + strings(400000));
        System.out.println("collections: " + collections(1200000));
        System.out.println("allocation: " + allocation(100000));
    }
}
