package com.android.minato.services;

import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;

/**
 * ServiceStub on the host's JVM (tools/framework-host-check.sh), without JUnit: each test is a method whose name
 * starts with "test", and the runner prints FAIL with the reason for each that throws. ADR-0045 decision 3's rules:
 * the permission gate first, then a written answer, else one log line per method and its type's default.
 * <p>
 * The check script generates {@code Wide}, an interface of {@value #WIDE_METHODS} methods named m0 to m299, as big
 * as IActivityManager.
 */
public final class ServiceStubTest {
    static final int WIDE_METHODS = 300;

    /** Stands for an AIDL interface: methods of every return kind, two of them overloaded. */
    public interface IFake {
        void ping();
        boolean isOn();
        byte b();
        char c();
        short s();
        int count();
        long since();
        float f();
        double d();
        String name();
        int[] ids();
        List<String> list();
        int add(int a, int b);
        String add(String a, String b);
        int fail(int code) throws Exception;
        Object asBinder();
    }

    /** Answers two of IFake's methods, one overload of add and fail. */
    public static final class Answers {
        int calls;
        public int count() { calls++; return 7; }
        public String name() { return "answered"; }
        public String add(String a, String b) { return a + b; }
        public int fail(int code) throws Exception {
            throw new java.io.IOException("code " + code);
        }
    }

    /** Package-private, as an answers class inside a service's file may be. */
    static final class HiddenAnswers {
        public boolean isOn() { return true; }
    }

    /** Answers isOn with the wrong type. */
    public static final class WrongAnswers {
        public int isOn() { return 1; }
    }

    private final List<String> lines = new ArrayList<>();
    private final ServiceStub.Logger logger = lines::add;
    private static final ServiceStub.PermissionGate OPEN = (method, args) -> { };

    private IFake fake(Object answers, ServiceStub.PermissionGate gate) {
        return ServiceStub.create(IFake.class, null, answers, gate, logger);
    }

    // The defaults, as AIDL's Default class returns them.

    public void testDefaultsPerReturnType() {
        IFake stub = fake(null, OPEN);
        stub.ping();
        check(!stub.isOn(), "isOn");
        check(stub.b() == 0, "b");
        check(stub.c() == '\0', "c");
        check(stub.s() == 0, "s");
        check(stub.count() == 0, "count");
        check(stub.since() == 0L, "since");
        check(stub.f() == 0f, "f");
        check(stub.d() == 0d, "d");
        check(stub.name() == null, "name");
        check(stub.ids() == null, "ids");
        check(stub.list() == null, "list");
    }

    public void testLogsEachMethodOnce() {
        IFake stub = fake(null, OPEN);
        stub.count();
        stub.count();
        stub.name();
        stub.add(1, 2);
        stub.add(3, 4);
        stub.add("a", "b");
        equal(lines, List.of(
                "ServiceStub: com.android.minato.services.ServiceStubTest$IFake.count() is not implemented, returns 0",
                "ServiceStub: com.android.minato.services.ServiceStubTest$IFake.name() is not implemented, returns null",
                "ServiceStub: com.android.minato.services.ServiceStubTest$IFake.add(int,int) is not implemented, returns 0",
                "ServiceStub: com.android.minato.services.ServiceStubTest$IFake.add(java.lang.String,java.lang.String)"
                        + " is not implemented, returns null"),
                "log lines");
    }

    public void testLogsOncePerInterfaceAcrossStubs() {
        fake(null, OPEN).ping();
        fake(null, OPEN).ping();
        check(lines.size() == 1, "two stubs of one interface log a method once, got " + lines);
    }

    // Written answers.

    public void testAnswersReplaceTheDefault() {
        Answers answers = new Answers();
        IFake stub = fake(answers, OPEN);
        check(stub.count() == 7, "count answered");
        check(stub.count() == 7 && answers.calls == 2, "answer called each time");
        check("answered".equals(stub.name()), "name answered");
        check(lines.isEmpty(), "an answered method logs nothing, got " + lines);
    }

    public void testOverloadsMatchByParameterTypes() {
        IFake stub = fake(new Answers(), OPEN);
        check("ab".equals(stub.add("a", "b")), "add(String,String) answered");
        check(stub.add(1, 2) == 0, "add(int,int) not answered");
        check(lines.size() == 1 && lines.get(0).contains(".add(int,int) "), "only add(int,int) logs, got " + lines);
    }

    public void testAnswerExceptionReachesTheCaller() {
        IFake stub = fake(new Answers(), OPEN);
        try {
            stub.fail(3);
            fail("no exception");
        } catch (java.io.IOException expected) {
            check("code 3".equals(expected.getMessage()), "message " + expected.getMessage());
        } catch (Exception other) {
            fail("got " + other);
        }
    }

    public void testPackagePrivateAnswers() {
        IFake stub = fake(new HiddenAnswers(), OPEN);
        check(stub.isOn(), "a package-private answers class answers");
    }

    public void testAnswerWithWrongReturnTypeIsRefused() {
        try {
            fake(new WrongAnswers(), OPEN);
            fail("created");
        } catch (IllegalArgumentException expected) {
            check(expected.getMessage().contains("isOn"), "message names the method: " + expected.getMessage());
        }
    }

    // The permission gate.

    public void testGateRunsFirstAndThrows() {
        List<String> asked = new ArrayList<>();
        ServiceStub.PermissionGate gate = (method, args) -> {
            asked.add(method.getName() + args.length);
            if (method.getName().equals("count")) {
                throw new SecurityException("needs android.permission.FAKE");
            }
        };
        Answers answers = new Answers();
        IFake stub = fake(answers, gate);
        try {
            stub.count();
            fail("no SecurityException");
        } catch (SecurityException expected) {
            check(expected.getMessage().equals("needs android.permission.FAKE"), "message");
        }
        check(answers.calls == 0, "the answer does not run after a refusal");
        check("answered".equals(stub.name()), "an allowed method answers");
        stub.ping();
        equal(asked, List.of("count0", "name0", "ping0"), "gate calls");
        check(lines.size() == 1 && lines.get(0).contains(".ping()"), "a refusal logs nothing, got " + lines);
    }

    // Object's methods and asBinder.

    public void testObjectMethods() {
        IFake one = fake(null, OPEN);
        IFake two = fake(null, OPEN);
        check(one.equals(one) && !one.equals(two) && !one.equals(null), "equals by identity");
        check(one.hashCode() == System.identityHashCode(one), "hashCode by identity");
        check(one.toString().equals("ServiceStub[com.android.minato.services.ServiceStubTest$IFake]"),
                "toString " + one);
        check(lines.isEmpty(), "Object's methods log nothing, got " + lines);
    }

    public void testAsBinderReturnsTheBinder() {
        Object binder = new Object();
        IFake stub = ServiceStub.create(IFake.class, binder, null, OPEN, logger);
        check(stub.asBinder() == binder, "asBinder");
        check(lines.isEmpty(), "asBinder logs nothing, got " + lines);
    }

    public void testRefusesAClass() {
        try {
            ServiceStub.create(Answers.class, null, null, OPEN, logger);
            fail("created");
        } catch (IllegalArgumentException expected) {
            // Proxy takes interfaces only.
        }
    }

    // As big as IActivityManager.

    public void testWideInterface() throws Exception {
        Class<?> wide = Class.forName("com.android.minato.services.Wide");
        check(wide.getMethods().length == WIDE_METHODS, "Wide has " + wide.getMethods().length + " methods");
        Object stub = ServiceStub.create(wide, null, null, OPEN, logger);
        for (Method method : wide.getMethods()) {
            method.invoke(stub);
        }
        check(lines.size() == WIDE_METHODS, "one line per method, got " + lines.size());
    }

    /** An indication, not a limit: a proxy call against a direct one. */
    public void testCostIndication() {
        Answers answers = new Answers();
        IFake stub = fake(answers, OPEN);
        int rounds = 2_000_000;
        long sum = 0;
        for (int i = 0; i < rounds; i++) sum += stub.count();
        long start = System.nanoTime();
        for (int i = 0; i < rounds; i++) sum += stub.count();
        long proxy = System.nanoTime() - start;
        start = System.nanoTime();
        for (int i = 0; i < rounds; i++) sum += answers.count();
        long direct = System.nanoTime() - start;
        System.out.printf(java.util.Locale.ROOT, "cost: %.1f ns per proxy call, %.1f ns direct (sum %d)%n",
                (double) proxy / rounds, (double) direct / rounds, sum);
    }

    // The runner.

    private static void check(boolean condition, String what) {
        if (!condition) {
            throw new AssertionError(what);
        }
    }

    private static void equal(Object got, Object wanted, String what) {
        if (!got.equals(wanted)) {
            throw new AssertionError(what + ": got " + got + ", wanted " + wanted);
        }
    }

    private static void fail(String what) {
        throw new AssertionError(what);
    }

    public static void main(String[] args) throws Exception {
        int failed = 0, run = 0;
        for (Method test : ServiceStubTest.class.getDeclaredMethods()) {
            if (!test.getName().startsWith("test")) {
                continue;
            }
            run++;
            ServiceStub.forgetLogged();
            try {
                test.invoke(new ServiceStubTest());
            } catch (java.lang.reflect.InvocationTargetException thrown) {
                failed++;
                System.out.println("FAIL " + test.getName() + ": " + thrown.getCause());
            }
        }
        if (failed > 0) {
            System.out.println("ServiceStubTest: " + failed + " of " + run + " failed");
            System.exit(1);
        }
        System.out.println("ServiceStubTest: " + run + " of " + run + " fine");
    }
}
