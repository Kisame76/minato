/**
 * What nterp does with invoke-interface (M6 a, ADR-0053): an interface method, a default method and the methods of
 * java.lang.Object on a receiver of interface type, a receiver that is null, and a synchronized block on an object.
 * The output is what Android 15 prints.
 */
public class Main {
    interface Named {
        String name();

        default String hello() {
            return "hello " + name();
        }
    }

    interface Counted extends Named {
        int count();

        default String describe() {
            return hello() + " x" + count();
        }
    }

    static class Thing implements Counted {
        private final String name;
        private final int count;

        Thing(String name, int count) {
            this.name = name;
            this.count = count;
        }

        public String name() {
            return name;
        }

        public int count() {
            return count;
        }

        @Override
        public String toString() {
            return "Thing(" + name + ")";
        }
    }

    static int calls = 0;

    static void check(boolean ok, String what) {
        System.out.println(what + ": " + (ok ? "ok" : "FAILED"));
    }

    public static void main(String[] args) {
        Named named = new Thing("a", 2);
        Counted counted = new Thing("b", 3);
        System.out.println(named.name());
        System.out.println(named.hello());
        System.out.println(counted.describe());
        // The methods of java.lang.Object on receivers of interface type.
        System.out.println(named.toString());
        check(counted.hashCode() == System.identityHashCode(counted), "hashCode");
        check(named.equals(named) && !named.equals(counted), "equals");
        check(named.getClass() == Thing.class, "getClass");
        // A receiver that is null: the NullPointerException comes from the fault on the guard page.
        Named none = args.length > 100 ? named : null;
        try {
            none.hello();
            check(false, "null default method");
        } catch (NullPointerException e) {
            check(true, "null default method");
        }
        try {
            none.name();
            check(false, "null interface method");
        } catch (NullPointerException e) {
            check(true, "null interface method");
        }
        try {
            none.toString();
            check(false, "null Object method");
        } catch (NullPointerException e) {
            check(true, "null Object method");
        }
        // monitor-enter and monitor-exit, with the same object twice (a recursive lock).
        Object lock = new Object();
        synchronized (lock) {
            synchronized (lock) {
                calls++;
            }
        }
        check(calls == 1 && !Thread.holdsLock(lock), "synchronized");
    }
}
