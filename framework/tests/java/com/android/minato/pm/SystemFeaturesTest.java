package com.android.minato.pm;

import java.io.IOException;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Map;
import java.util.TreeMap;

/**
 * Minato's system features (ADR-0045 decision 4, ADR-0039 b3.5b1) against Android 15's reference AVD
 * (fixtures/expected/system-features.txt, tools/android-reference.sh --features), on the host's JVM
 * (tools/framework-host-check.sh, from the repository's root): every feature the AVD reports is Minato's with the same
 * version, or left out with a reason; every other feature or version of Minato's is a departure with a reason; and
 * no reason is stale.
 */
public final class SystemFeaturesTest {
    private static final Path REFERENCE = Path.of("fixtures", "expected", "system-features.txt");

    /** The AVD's features, "feature:<name>[=<version>]" each, without reqGlEsVersion, which is no feature. */
    private static Map<String, Integer> reference() throws IOException {
        Map<String, Integer> features = new TreeMap<>();
        for (String line : Files.readAllLines(REFERENCE, StandardCharsets.UTF_8)) {
            check(line.startsWith("feature:"), "a line of pm list features: " + line);
            String feature = line.substring("feature:".length());
            int equals = feature.indexOf('=');
            String name = equals < 0 ? feature : feature.substring(0, equals);
            if (name.equals("reqGlEsVersion")) {
                continue;
            }
            features.put(name, equals < 0 ? 0 : Integer.decode(feature.substring(equals + 1)));
        }
        return features;
    }

    public void testEveryReferenceFeatureIsMinatosOrLeftOutWithAReason() throws IOException {
        for (Map.Entry<String, Integer> feature : reference().entrySet()) {
            String name = feature.getKey();
            Integer ours = SystemFeatures.FEATURES.get(name);
            if (ours == null) {
                check(!SystemFeatures.LEFT_OUT.getOrDefault(name, "").isEmpty(),
                        name + " is neither kept nor left out");
            } else if (!ours.equals(feature.getValue())) {
                check(!SystemFeatures.DEPARTURES.getOrDefault(name, "").isEmpty(),
                        name + "=" + ours + " departs from " + feature.getValue() + " without a reason");
            }
        }
    }

    public void testEveryAdditionHasAReason() throws IOException {
        Map<String, Integer> reference = reference();
        for (String name : SystemFeatures.FEATURES.keySet()) {
            if (!reference.containsKey(name)) {
                check(!SystemFeatures.DEPARTURES.getOrDefault(name, "").isEmpty(), name + " is added without a reason");
            }
        }
    }

    public void testNoReasonIsStale() throws IOException {
        Map<String, Integer> reference = reference();
        for (String name : SystemFeatures.LEFT_OUT.keySet()) {
            check(reference.containsKey(name) && !SystemFeatures.FEATURES.containsKey(name),
                    name + " is left out but " + (reference.containsKey(name) ? "kept" : "not the reference's"));
        }
        for (String name : SystemFeatures.DEPARTURES.keySet()) {
            check(SystemFeatures.FEATURES.containsKey(name) && !SystemFeatures.FEATURES.get(name)
                    .equals(reference.get(name)), name + " is no departure");
        }
    }

    public void testHasAsThePackageManagerServiceAnswers() {
        check(SystemFeatures.has("android.hardware.touchscreen", 0, false), "the touchscreen");
        check(!SystemFeatures.has("android.hardware.touchscreen", 1, false), "a version above the feature's");
        check(!SystemFeatures.has("android.hardware.camera", 0, true), "a camera");
        check(!SystemFeatures.has("io.github.kisame76.minato.none", 0, true), "a feature nobody declares");
        check(SystemFeatures.has("android.hardware.vulkan.version", 0x403000, true), "Vulkan 1.3 with a driver");
        check(!SystemFeatures.has("android.hardware.vulkan.version", 0x403001, true), "not 1.3.1");
        check(!SystemFeatures.has("android.hardware.vulkan.version", 0, false), "no Vulkan without a driver");
        check(!SystemFeatures.available(false).containsKey("android.hardware.vulkan.compute"), "available without");
        check(SystemFeatures.available(true).containsKey("android.hardware.vulkan.compute"), "available with");
    }

    private static void check(boolean condition, String what) {
        if (!condition) {
            throw new AssertionError(what);
        }
    }

    public static void main(String[] args) throws Exception {
        int failed = 0, run = 0;
        for (Method test : SystemFeaturesTest.class.getDeclaredMethods()) {
            if (!test.getName().startsWith("test")) {
                continue;
            }
            run++;
            try {
                test.invoke(new SystemFeaturesTest());
            } catch (java.lang.reflect.InvocationTargetException thrown) {
                failed++;
                System.out.println("FAIL " + test.getName() + ": " + thrown.getCause());
            }
        }
        if (failed > 0) {
            System.out.println("SystemFeaturesTest: " + failed + " of " + run + " failed");
            System.exit(1);
        }
        System.out.println("SystemFeaturesTest: " + run + " of " + run + " fine");
    }
}
