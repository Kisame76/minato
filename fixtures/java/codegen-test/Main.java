/**
 * What the optimizing compiler's code does with references (M6 c2, ADR-0053): instanceof and check-cast against a
 * class, an interface and an array type, invoke-interface through a full IMT, every array element type with constant
 * and variable indexes, fields (plain, volatile, long), null receivers of each kind, a static call of a class that
 * is not initialized yet, loops the compiler vectorizes, exceptions thrown through several frames, allocation, and
 * (under -Xgc:CMS) the card marks: old objects that point to young ones across many collections. Every line is
 * what the interpreter prints, which is what Android 15 prints.
 */
public class Main {
    interface Shape { int sides(); }
    interface Named { String name(); }
    static class Base implements Shape { public int sides() { return 0; } }
    static class Square extends Base implements Named {
        public int sides() { return 4; }
        public String name() { return "square"; }
    }
    static final class Tri extends Base {
        public int sides() { return 3; }
    }
    interface Big {
        int m0();
        int m1();
        int m2();
        int m3();
        int m4();
        int m5();
        int m6();
        int m7();
        int m8();
        int m9();
        int m10();
        int m11();
        int m12();
        int m13();
        int m14();
        int m15();
        int m16();
        int m17();
        int m18();
        int m19();
        int m20();
        int m21();
        int m22();
        int m23();
        int m24();
        int m25();
        int m26();
        int m27();
        int m28();
        int m29();
        int m30();
        int m31();
        int m32();
        int m33();
        int m34();
        int m35();
        int m36();
        int m37();
        int m38();
        int m39();
        int m40();
        int m41();
        int m42();
        int m43();
        int m44();
        int m45();
        int m46();
        int m47();
        int m48();
        int m49();
        int m50();
        int m51();
        int m52();
        int m53();
        int m54();
        int m55();
        int m56();
        int m57();
        int m58();
        int m59();
    }
    static class BigImpl implements Big {
        final int base;
        BigImpl(int base) { this.base = base; }
        public int m0() { return 0 * 3 + base; }
        public int m1() { return 1 * 3 + base; }
        public int m2() { return 2 * 3 + base; }
        public int m3() { return 3 * 3 + base; }
        public int m4() { return 4 * 3 + base; }
        public int m5() { return 5 * 3 + base; }
        public int m6() { return 6 * 3 + base; }
        public int m7() { return 7 * 3 + base; }
        public int m8() { return 8 * 3 + base; }
        public int m9() { return 9 * 3 + base; }
        public int m10() { return 10 * 3 + base; }
        public int m11() { return 11 * 3 + base; }
        public int m12() { return 12 * 3 + base; }
        public int m13() { return 13 * 3 + base; }
        public int m14() { return 14 * 3 + base; }
        public int m15() { return 15 * 3 + base; }
        public int m16() { return 16 * 3 + base; }
        public int m17() { return 17 * 3 + base; }
        public int m18() { return 18 * 3 + base; }
        public int m19() { return 19 * 3 + base; }
        public int m20() { return 20 * 3 + base; }
        public int m21() { return 21 * 3 + base; }
        public int m22() { return 22 * 3 + base; }
        public int m23() { return 23 * 3 + base; }
        public int m24() { return 24 * 3 + base; }
        public int m25() { return 25 * 3 + base; }
        public int m26() { return 26 * 3 + base; }
        public int m27() { return 27 * 3 + base; }
        public int m28() { return 28 * 3 + base; }
        public int m29() { return 29 * 3 + base; }
        public int m30() { return 30 * 3 + base; }
        public int m31() { return 31 * 3 + base; }
        public int m32() { return 32 * 3 + base; }
        public int m33() { return 33 * 3 + base; }
        public int m34() { return 34 * 3 + base; }
        public int m35() { return 35 * 3 + base; }
        public int m36() { return 36 * 3 + base; }
        public int m37() { return 37 * 3 + base; }
        public int m38() { return 38 * 3 + base; }
        public int m39() { return 39 * 3 + base; }
        public int m40() { return 40 * 3 + base; }
        public int m41() { return 41 * 3 + base; }
        public int m42() { return 42 * 3 + base; }
        public int m43() { return 43 * 3 + base; }
        public int m44() { return 44 * 3 + base; }
        public int m45() { return 45 * 3 + base; }
        public int m46() { return 46 * 3 + base; }
        public int m47() { return 47 * 3 + base; }
        public int m48() { return 48 * 3 + base; }
        public int m49() { return 49 * 3 + base; }
        public int m50() { return 50 * 3 + base; }
        public int m51() { return 51 * 3 + base; }
        public int m52() { return 52 * 3 + base; }
        public int m53() { return 53 * 3 + base; }
        public int m54() { return 54 * 3 + base; }
        public int m55() { return 55 * 3 + base; }
        public int m56() { return 56 * 3 + base; }
        public int m57() { return 57 * 3 + base; }
        public int m58() { return 58 * 3 + base; }
        public int m59() { return 59 * 3 + base; }
    }

    static class Late {
        static int value = init();
        static int init() { System.out.println("Late.<clinit>"); return 41; }
        static int twice() { return value * 2; }
    }

    static class Holder {
        int i; long l; Object o; volatile int vi; volatile long vl; volatile Object vo;
        static int si; static Object so;
    }

    static class Node {
        final int id; Node next;
        Node(int id) { this.id = id; }
    }

    static Object nul() { return counter < 0 ? "never" : null; }

    static void out(String what, Object value) { System.out.println(what + ": " + value); }

    static void instanceOf() {
        Object[] things = { new Square(), new Tri(), new Base(), "text", new int[2], new String[1], new Object[1], null,
                            Integer.valueOf(7) };
        StringBuilder b = new StringBuilder();
        for (Object o : things) {
            b.append(o instanceof Shape ? 'S' : '-').append(o instanceof Named ? 'N' : '-')
             .append(o instanceof Base ? 'B' : '-').append(o instanceof Tri ? 'T' : '-')
             .append(o instanceof int[] ? 'I' : '-').append(o instanceof Object[] ? 'O' : '-')
             .append(o instanceof String[] ? 'X' : '-').append(o instanceof CharSequence ? 'C' : '-').append(' ');
        }
        out("instanceof", b);
        for (Object o : things) {
            String r;
            try { Shape s = (Shape) o; r = "shape " + (s == null ? "null" : s.sides()); }
            catch (ClassCastException e) { r = e.getMessage(); }
            System.out.println("cast " + r);
        }
        try { Object o = new Object[] { "a" }; String[] s = (String[]) o; out("array cast", s.length); }
        catch (ClassCastException e) { out("array cast", e.getMessage()); }
    }

    static void interfaces() {
        Shape[] shapes = { new Square(), new Tri(), new Base() };
        int sum = 0;
        for (Shape s : shapes) sum += s.sides();
        out("sides", sum);
        Named n = new Square();
        out("name", n.name());
        Big big = new BigImpl(5);
        sum = 0;
        sum += big.m0();
        sum += big.m7();
        sum += big.m14();
        sum += big.m21();
        sum += big.m28();
        sum += big.m35();
        sum += big.m42();
        sum += big.m49();
        sum += big.m56();
        out("big", sum);
        out("big m59", big.m59());
        Shape none = (Shape) nul();
        try { none.sides(); } catch (NullPointerException e) { out("null interface", e.getMessage()); }
    }

    static void arrays() {
        byte[] by = new byte[8]; short[] sh = new short[8]; char[] ch = new char[8]; int[] in = new int[8];
        long[] lo = new long[8]; float[] fl = new float[8]; double[] db = new double[8]; boolean[] bo = new boolean[8];
        String[] st = new String[8]; Object[] ob = new Object[8];
        for (int i = 0; i < 8; i++) {
            by[i] = (byte) (i * 40); sh[i] = (short) (i * 9000); ch[i] = (char) ('a' + i); in[i] = i * i - 3;
            lo[i] = (long) i << 33; fl[i] = i / 3f; db[i] = i / 7.0; bo[i] = (i & 1) == 0; st[i] = "s" + i;
                    ob[i] = st[i];
        }
        by[2] = 7; sh[3] = 9; ch[4] = 'z'; in[5] = 11; lo[6] = 13L; fl[7] = 1.5f; db[0] = 2.5; bo[1] = true;
        st[1] = "one"; ob[2] = "two";
        long total = 0;
        for (int i = 0; i < 8; i++) total += by[i] + sh[i] + ch[i] + in[i] + lo[i] + (long) (fl[i] * 10)
                + (long) (db[i] * 10)
                                     + (bo[i] ? 1 : 0) + st[i].length() + ob[i].hashCode() % 7;
        out("arrays", total);
        out("last", by[7] + " " + sh[7] + " " + ch[7] + " " + in[7] + " " + lo[7] + " " + fl[7] + " " + db[7] + " "
                + bo[7]
            + " " + st[7] + " " + ob[7]);
        int[][] grid = new int[3][4];
        grid[1][2] = 5; grid[2][3] = grid[1][2] + 1;
        out("grid", grid[2][3] + " " + grid.length + " " + grid[0].length);
        String s = "compressed", u = "unicod\u00e9\u4e2d";
        out("charAt", s.charAt(3) + " " + u.charAt(6) + " " + u.charAt(7) + " " + s.length() + " " + u.length());
        out("indexOf", s.indexOf('s') + " " + u.indexOf('\u4e2d') + " " + s.indexOf('s', 6) + " " + s.indexOf("ss"));
        out("string compare", s.compareTo("compressor") + " " + u.equals("unicod\u00e9\u4e2d") + " " + s.equals(u));
        try { in[8] = 1; } catch (ArrayIndexOutOfBoundsException e) { out("aioobe set", e.getMessage()); }
        try { out("aioobe get", ob[-1]); }
                catch (ArrayIndexOutOfBoundsException e) { out("aioobe get", e.getMessage()); }
        Object[] strings = new String[2];
        try { strings[0] = Integer.valueOf(1); } catch (ArrayStoreException e) { out("array store", e.getMessage()); }
        int[] none = (int[]) nul();
        try { none[0] = 1; } catch (NullPointerException e) { out("null array set", e.getMessage()); }
        try { out("null array length", none.length); }
                catch (NullPointerException e) { out("null array length", e.getMessage()); }
        try { out("neg size", new int[Integer.parseInt("-1")]); }
                catch (NegativeArraySizeException e) { out("neg size", e.getMessage()); }
    }

    static void fields() {
        Holder h = new Holder();
        h.i = 3; h.l = 1L << 40; h.o = "obj"; h.vi = 4; h.vl = 2L << 40; h.vo = h; Holder.si = 9; Holder.so = h.o;
        h.i += h.vi; h.l += h.vl;
        out("fields", h.i + " " + h.l + " " + h.o + " " + (h.vo == h) + " " + Holder.si + " " + Holder.so);
        Holder none = (Holder) nul();
        try { none.i = 1; } catch (NullPointerException e) { out("null field set", e.getMessage()); }
        try { out("null field get", none.o); } catch (NullPointerException e) { out("null field get", e.getMessage()); }
        try { out("null volatile", none.vl); } catch (NullPointerException e) { out("null volatile", e.getMessage()); }
        Object lock = new Object();
        synchronized (lock) { synchronized (lock) { out("monitor", "held twice"); } }
        out("late", Late.twice());
    }

    static void loops() {
        int n = 103;
        int[] a = new int[n], b = new int[n], c = new int[n];
        char[] ch = new char[n]; byte[] by = new byte[n];
        for (int i = 0; i < n; i++) { b[i] = i; c[i] = 3 * i - 7; }
        for (int i = 0; i < n; i++) a[i] = b[i] + c[i];
        for (int i = 0; i < n; i++) ch[i] = (char) (a[i] & 0x7f);
        for (int i = 0; i < n; i++) by[i] = (byte) (ch[i] + 1);
        long s = 0;
        for (int i = 0; i < n; i++) s += a[i] + ch[i] + by[i];
        out("loops", s);
    }

    static int thrower(int depth) {
        if (depth == 0) throw new IllegalStateException("bottom");
        try { return thrower(depth - 1) + 1; } finally { counter++; }
    }
    static int counter;

    static void exceptions() {
        try { thrower(5); } catch (IllegalStateException e) { out("thrown", e.getMessage() + " " + counter); }
        try { Object o = "x"; Integer i = (Integer) o; out("not reached", i); }
        catch (ClassCastException e) { out("cast", e.getMessage()); }
        try { out("div", 5 / Integer.parseInt("0")); } catch (ArithmeticException e) { out("div", e.getMessage()); }
        try { throw (RuntimeException) nul(); } catch (NullPointerException e) { out("throw null", e.getMessage()); }
    }

    static void allocation() {
        Object[] keep = new Object[50];
        for (int i = 0; i < keep.length; i++) keep[i] = new Node[i + 1];
        int[] big = new int[300000];
        big[299999] = 5;
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < 30; i++) b.append(i).append(',');
        long sum = 0;
        for (Object o : keep) sum += ((Node[]) o).length;
        out("allocation", sum + " " + big[299999] + " " + b.length());
    }

    static void cards() {
        Node[] old = new Node[4096];
        for (int i = 0; i < old.length; i++) old[i] = new Node(-1);
        System.gc();
        long garbage = 0;
        for (int round = 0; round < 40; round++) {
            for (int i = 0; i < old.length; i++) {
                old[i].next = new Node(round * 10000 + i);
                byte[] junk = new byte[2000];
                garbage += junk.length;
            }
        }
        long bad = 0;
        for (int i = 0; i < old.length; i++) if (old[i].next.id != 39 * 10000 + i) bad++;
        out("cards", bad + " " + (garbage > 0));
    }

    public static void main(String[] args) {
        instanceOf();
        interfaces();
        arrays();
        fields();
        loops();
        exceptions();
        allocation();
        cards();
    }
}
