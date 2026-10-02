package com.android.minato.sample;

import com.android.minato.sample.app.SampleAppManager;
import com.android.minato.sample.pm.SampleBaseManager;
import com.android.minato.sample.pm.SampleInfo;
import com.android.minato.services.Unwritten;

import java.io.File;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;

/**
 * managergen --implement's output for framework/tests/managergen (ADR-0039 b3.5a), compiled and run on the host's
 * JVM by tools/framework-host-check.sh: SampleAppManager is a concrete SampleBaseManager, whose unwritten abstract
 * methods log once and return their type's default, and whose hand file's methods answer.
 */
public final class ImplementedSampleTest {
    private final List<String> lines = new ArrayList<>();

    private SampleBaseManager manager() {
        Unwritten.setLogger(lines::add);
        return new SampleAppManager("a");
    }

    public void testUnwrittenLogsOnce() {
        SampleBaseManager manager = manager();
        manager.clear("x");
        manager.clear("y");
        check(!manager.hasFeature("f", 1), "hasFeature");
        equal(lines, List.of(
                "Unwritten: com.android.minato.sample.app.SampleAppManager.clear(String) is not implemented",
                "Unwritten: com.android.minato.sample.app.SampleAppManager.hasFeature(String,int) is not"
                        + " implemented, returns false"),
                "log lines");
    }

    @SuppressWarnings("deprecation") // getOld, deprecated in the sample as some of PackageManager's methods are
    public void testDefaults() throws Exception {
        SampleBaseManager manager = manager();
        check(manager.getInfos(0) == null, "getInfos");
        check(manager.getInfoAsUser("a", 0, 0) == null, "getInfoAsUser");
        check(manager.getOld("a") == 0, "getOld");
        check(manager.first(SampleInfo.class, "a", "b") == null, "first");
        check(manager.sizes(new File[0]) == null, "sizes");
        manager.register(name -> { }, null);
        check(lines.size() == 6, "each logs: " + lines);
    }

    public void testHandBodies() throws Exception {
        SampleBaseManager manager = manager();
        check(manager.getInfo("a", 0).name.equals("a"), "the abstract method answered");
        check(manager.getInfo("a", 0L).name.equals("a"), "the concrete method overridden");
        try {
            manager.getInfo("b", 0);
            fail("getInfo(b) answered");
        } catch (SampleBaseManager.NotFoundException expected) {
            equal(expected.getMessage(), "b", "the exception's name");
        }
        check(manager.count() == SampleBaseManager.FLAG_A, "the concrete method kept");
        check(lines.isEmpty(), "hand methods log nothing: " + lines);
    }

    // The runner, as GeneratedSampleTest's.

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
        for (Method test : ImplementedSampleTest.class.getDeclaredMethods()) {
            if (!test.getName().startsWith("test")) {
                continue;
            }
            run++;
            Unwritten.forgetLogged();
            try {
                test.invoke(new ImplementedSampleTest());
            } catch (java.lang.reflect.InvocationTargetException thrown) {
                failed++;
                System.out.println("FAIL " + test.getName() + ": " + thrown.getCause());
            }
        }
        if (failed > 0) {
            System.out.println("ImplementedSampleTest: " + failed + " of " + run + " failed");
            System.exit(1);
        }
        System.out.println("ImplementedSampleTest: " + run + " of " + run + " fine");
    }
}
