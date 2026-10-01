package com.android.minato.sample;

import com.android.minato.services.Unwritten;

import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;

/**
 * The managers' generator's output for framework/tests/managergen (ADR-0045 decision 3), compiled and run on the
 * host's JVM by tools/framework-host-check.sh: an unwritten method logs once and returns its type's default, the
 * hand file's bodies answer, constants keep their values, and nested types, enums, varargs and abstract classes
 * work as the API declares them.
 */
public final class GeneratedSampleTest {
    private final List<String> lines = new ArrayList<>();

    private SampleManager manager() {
        Unwritten.setLogger(lines::add);
        return new SampleManager("x");
    }

    public void testUnwrittenLogsOnce() {
        SampleManager manager = manager();
        manager.adjustVolume(1, 2);
        manager.adjustVolume(3, 4);
        check(manager.getProperty("p") == null, "getProperty");
        equal(lines, List.of(
                "Unwritten: com.android.minato.sample.SampleManager.adjustVolume(int,int) is not implemented",
                "Unwritten: com.android.minato.sample.SampleManager.getProperty(String) is not implemented,"
                        + " returns null"),
                "log lines");
    }

    @SuppressWarnings("deprecation") // isMusicActive, deprecated in the sample as in AudioManager's API
    public void testDefaults() {
        SampleManager manager = manager();
        check(!manager.isMusicActive(), "isMusicActive");
        check(manager.since() == 0L, "since");
        check(manager.ratio() == 0f, "ratio");
        check(manager.precise() == 0d, "precise");
        check(manager.letter() == '\0', "letter");
        check(manager.level() == 0, "level");
        check(manager.small() == 0, "small");
        check(manager.ids() == null, "ids");
        check(manager.getDevices(1) == null, "getDevices");
        check(manager.<Integer>first(List.of(1)) == null, "first");
        check(!SampleManager.isSupported(SampleFormat.PCM_16BIT), "isSupported");
        manager.register(change -> { });
        check(lines.contains("Unwritten: com.android.minato.sample.SampleManager.letter() is not implemented,"
                + " returns '\\0'"), "char's default in the log: " + lines);
    }

    public void testHandBodies() {
        SampleManager manager = manager();
        check(manager.getMode() == 2, "getMode answered");
        int[] heard = {0};
        check(manager.requestFocus(change -> heard[0] = change, 3, 5) == 1, "requestFocus with a listener");
        check(heard[0] == 5, "listener called");
        check(manager.requestFocus(SampleFormat.PCM_FLOAT) == 0, "the other overload stays generated");
        equal(manager.calls(), List.of("new x", "getMode"), "hidden members");
        equal(lines, List.of("Unwritten: com.android.minato.sample.SampleManager.requestFocus("
                + "com.android.minato.sample.SampleFormat)"
                + " is not implemented, returns 0"), "only the generated overload logs");
    }

    public void testConstants() {
        check(SampleManager.MODE_NORMAL == 0 && SampleManager.MODE_RINGTONE == -1, "int");
        check(SampleManager.BIG == Long.MAX_VALUE, "long");
        check(SampleManager.HALF == 0.5f, "float");
        check(SampleManager.LETTER == 'A', "char");
        check(SampleManager.SMALL == -8, "byte");
        check(SampleManager.URL.equals("http://example.org/a;b // c \"q\""), "String " + SampleManager.URL);
        check(SampleManager.ACTION_NOISY.equals("android.media.AUDIO_BECOMING_NOISY"), "ACTION_NOISY");
        check(SampleManager.RUNTIME == 0 && SampleManager.NONE == null, "fields without a value");
    }

    public void testObjectMethods() {
        SampleManager one = manager(), two = manager();
        check(one.equals(one) && !one.equals(two), "equals");
        check(one.hashCode() == System.identityHashCode(one), "hashCode");
        check(one.toString().startsWith("com.android.minato.sample.SampleManager@"), "toString " + one);
        check(lines.isEmpty(), "Object's methods log nothing: " + lines);
    }

    public void testNestedEnumAbstract() throws Exception {
        manager();
        SampleManager.Settings settings = new SampleManager.Settings(3);
        check(settings.value() == 0 && settings.initial == 0, "Settings");
        SampleManager.OnChangeListener listener = change -> { };
        check(!listener.wants(1), "default method");
        check(SampleFormat.values().length == 2 && SampleFormat.valueOf("PCM_FLOAT") == SampleFormat.PCM_FLOAT,
                "enum");
        check(SampleFormat.PCM_16BIT.label() == null, "enum method");
        check(new SampleDevice().getId() == 0, "package-private ctor");
        check(java.lang.reflect.Modifier.isAbstract(SampleCallback.class.getModifiers()), "abstract");
        new SampleCallback<String>() {
            @Override
            public void onResult(String result) { }
        }.run();
        try {
            manager().load("f");
        } catch (java.io.IOException never) {
            fail("load threw");
        }
        check(lines.contains("Unwritten: com.android.minato.sample.SampleCallback.run() is not implemented"),
                "abstract class's body logs: " + lines);
    }

    // The runner, as ServiceStubTest's.

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
        for (Method test : GeneratedSampleTest.class.getDeclaredMethods()) {
            if (!test.getName().startsWith("test")) {
                continue;
            }
            run++;
            Unwritten.forgetLogged();
            try {
                test.invoke(new GeneratedSampleTest());
            } catch (java.lang.reflect.InvocationTargetException thrown) {
                failed++;
                System.out.println("FAIL " + test.getName() + ": " + thrown.getCause());
            }
        }
        if (failed > 0) {
            System.out.println("GeneratedSampleTest: " + failed + " of " + run + " failed");
            System.exit(1);
        }
        System.out.println("GeneratedSampleTest: " + run + " of " + run + " fine");
    }
}
