package com.android.minato.pm;

import java.io.File;
import java.io.IOException;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.Base64;
import java.util.List;

/**
 * The guest's reader of signers.json (ADR-0039 b3.5b1) on the host's JVM (tools/framework-host-check.sh): the
 * signers and the lineage the import wrote into the version's folder, read strictly, so that a broken file fails at
 * the app's start instead of giving the app made-up signatures. Only the writer's bytes read.
 */
public final class SignersFileTest {
    // DER encodings start with an ASN.1 SEQUENCE (0x30); the rest is not read.
    private static final byte[] A = {0x30, 0x03, 'a', 'a', 'a'};
    private static final byte[] B = {0x30, 0x03, 'b', 'b', 'b'};
    private static final byte[] C = {0x30, 0x03, 'c', 'c', 'c'};

    private static String b64(byte[] bytes) {
        return Base64.getEncoder().encodeToString(bytes);
    }

    private static File file(String json) throws IOException {
        File file = File.createTempFile("signers", ".json");
        file.deleteOnExit();
        Files.write(file.toPath(), json.getBytes(StandardCharsets.UTF_8));
        return file;
    }

    private static SignersFile read(String json) throws IOException {
        return SignersFile.read(file(json));
    }

    /** The file is refused, and the message names the file and the problem. */
    private static void refused(String json, String problem) throws IOException {
        File file = file(json);
        try {
            SignersFile.read(file);
        } catch (IllegalStateException expected) {
            check(expected.getMessage().startsWith(file.getPath() + ": "), "names the file: " + expected.getMessage());
            check(expected.getMessage().contains(problem), "'" + problem + "' in: " + expected.getMessage());
            return;
        }
        throw new AssertionError("accepted: " + json.substring(0, Math.min(json.length(), 200)));
    }

    /** In the writer's form: the keys sorted, nothing spaced. */
    private static String signers(String scheme, String signers, String lineage) {
        return "{\"format\":1,\"lineage\":[" + lineage + "],\"scheme\":" + scheme + ",\"signers\":[" + signers + "]}";
    }

    private static String node(byte[] certificate, String flags) {
        return "{\"certificate\":\"" + b64(certificate) + "\",\"flags\":" + flags + "}";
    }

    // What a valid file gives.

    public void testOneSignerOfV2() throws IOException {
        SignersFile file = read(signers("2", "\"" + b64(A) + "\"", ""));
        check(file.scheme == 2, "scheme " + file.scheme);
        List<byte[]> signers = file.signers();
        check(signers.size() == 1 && Arrays.equals(signers.get(0), A), "the signer's DER");
        check(file.lineage().isEmpty(), "no lineage");
    }

    public void testSeveralSignersOfV1KeepTheirOrder() throws IOException {
        SignersFile file = read(signers("1", "\"" + b64(B) + "\",\"" + b64(A) + "\"", ""));
        check(file.scheme == 1, "scheme " + file.scheme);
        check(file.signers().size() == 2 && Arrays.equals(file.signers().get(0), B)
                && Arrays.equals(file.signers().get(1), A), "the APK's order");
    }

    public void testV3LineageOldestFirstWithItsFlags() throws IOException {
        SignersFile file = read(signers("3", "\"" + b64(B) + "\"", node(A, "23") + "," + node(B, "0")));
        List<SignersFile.Node> lineage = file.lineage();
        check(lineage.size() == 2, "two nodes");
        check(Arrays.equals(lineage.get(0).certificate(), A) && lineage.get(0).flags == 23, "the older key first");
        check(Arrays.equals(lineage.get(1).certificate(), B) && lineage.get(1).flags == 0, "the current key last");
    }

    public void testV31IsItsOwnScheme() throws IOException {
        check(read(signers("31", "\"" + b64(A) + "\"", node(C, "8") + "," + node(A, "1"))).scheme == 31, "v3.1");
    }

    public void testCopiesCannotChangeTheFile() throws IOException {
        SignersFile file = read(signers("2", "\"" + b64(A) + "\"", ""));
        file.signers().get(0)[0] = 0;
        check(Arrays.equals(file.signers().get(0), A), "a caller's change stays its own");
        try {
            file.signers().clear();
            throw new AssertionError("a modifiable list");
        } catch (UnsupportedOperationException expected) {
            // Unmodifiable.
        }
    }

    public void testTheWritersBytes() throws IOException {
        // What APKKit's SignersFileTests.theOneFormTheGuestReads writes.
        SignersFile file = read("{\"format\":1,\"lineage\":[{\"certificate\":\"MAH7\",\"flags\":-2147483648},"
                + "{\"certificate\":\"MAH/\",\"flags\":23}],\"scheme\":3,\"signers\":[\"MAH/\"]}");
        byte[] signer = {0x30, 0x01, (byte) 0xff};
        check(file.scheme == 3 && Arrays.equals(file.signers().get(0), signer), "the signer");
        check(Arrays.equals(file.lineage().get(0).certificate(), new byte[] {0x30, 0x01, (byte) 0xfb})
                && file.lineage().get(0).flags == Integer.MIN_VALUE, "the older key");
        check(Arrays.equals(file.lineage().get(1).certificate(), signer) && file.lineage().get(1).flags == 23, "the key");
    }

    // What is refused.

    public void testMissingFile() throws IOException {
        File missing = new File(file("{}").getPath() + ".missing");
        try {
            SignersFile.read(missing);
            throw new AssertionError("a missing file read");
        } catch (IllegalStateException expected) {
            check(expected.getMessage().startsWith(missing.getPath() + ": "), expected.getMessage());
        }
    }

    public void testNotJson() throws IOException {
        refused("", "not JSON");
        refused("{\"format\":", "not JSON");
        refused("\"signers\"", "not a JSON object");
        refused("[" + signers("2", "\"" + b64(A) + "\"", "") + "]", "not a JSON object");
    }

    public void testOnlyTheWritersForm() throws IOException {
        // Whatever else org.json would take: a file APKKit did not write.
        String valid = signers("2", "\"" + b64(A) + "\"", "");
        String form = "writer's form";
        refused("\n " + valid + " \n", form);
        refused(valid.replace(",", ", "), form);
        refused(valid.replace("{\"format\":1,", "{\"format\":1,\"format\":1,"), form);
        refused(valid.replace("\"scheme\":2,", "").replace("{\"format\"", "{\"scheme\":2,\"format\""), form);
        refused(valid + "\u0000", form);
        refused(valid + "//", form);
        refused(valid + "#", form);
        refused(valid.replace("\"scheme\":2", "\"scheme\":0x2"), form);
        refused(valid.replace("\"scheme\":2", "\"scheme\":02"), form);
        refused(valid.replace("\"scheme\":2", "\"scheme\":2.0"), "scheme");
        refused(valid.replace("\"format\":1", "format:1"), form);
        refused(valid.replace("\"format\":1", "'format':1"), form);
        refused(valid.replace("\"format\":1", "\"format\"=1"), form);
        refused(valid.replace("\"format\":1,", "\"format\":1;"), form);
        refused(valid.replace("\"format\"", "\"\\u0066ormat\""), form);
        refused(valid.replace("\"format\"", "\"format\u001f\""), form);
        refused(signers("2", "\"" + b64(A).replace("=", "") + "\"", ""), form);
    }

    public void testNestingIsBounded() throws IOException {
        // Deeper than org.json's recursion can go: the named problem, not a StackOverflowError.
        char[] open = new char[200_000];
        Arrays.fill(open, '[');
        refused(new String(open), "nested deeper");
        // Brackets inside strings, which org.json reads as strings and a count of brackets would not.
        StringBuilder strings = new StringBuilder();
        for (int i = 0; i < 100_000; i++) {
            strings.append("[\"]\",");
        }
        refused(strings.toString(), "writer's form");
    }

    public void testEveryLineageCertificateOnce() throws IOException {
        refused(signers("3", "\"" + b64(B) + "\"", node(A, "1") + "," + node(A, "1") + "," + node(B, "0")),
                "lineage[1] repeats");
        refused(signers("3", "\"" + b64(B) + "\"", node(B, "1") + "," + node(B, "0")), "lineage[1] repeats");
    }

    public void testSomethingAfterTheObject() throws IOException {
        refused(signers("2", "\"" + b64(A) + "\"", "") + "{}", "after the object");
    }

    public void testFormat() throws IOException {
        refused(signers("2", "\"" + b64(A) + "\"", "").replace("\"format\":1", "\"format\":2"), "format 2");
        refused(signers("2", "\"" + b64(A) + "\"", "").replace("\"format\":1", "\"format\":\"1\""), "format");
        refused(signers("2", "\"" + b64(A) + "\"", "").replace("\"format\":1,", ""), "format");
    }

    public void testScheme() throws IOException {
        for (String scheme : new String[] {"0", "4", "30", "-1", "2.0", "\"2\""}) {
            refused(signers(scheme, "\"" + b64(A) + "\"", ""), "scheme");
        }
        refused(signers("null", "\"" + b64(A) + "\"", ""), "writer's form");
    }

    public void testUnknownKey() throws IOException {
        refused(signers("2", "\"" + b64(A) + "\"", "").replace("{\"format\"", "{\"extra\":0,\"format\""),
                "unknown key extra");
        refused(signers("3", "\"" + b64(B) + "\"", "{\"certificate\":\"" + b64(B) + "\",\"flags\":0,\"x\":1}"),
                "unknown key x");
    }

    public void testSigners() throws IOException {
        refused(signers("2", "", ""), "no signers");
        refused(signers("2", "\"" + b64(A) + "\"", "").replace(",\"signers\":[\"" + b64(A) + "\"]", ""), "signers");
        refused(signers("2", "1", ""), "signers[0]");
        refused(signers("2", "\"\"", ""), "signers[0]");
        refused(signers("2", "\"@@@@\"", ""), "writer's form");
        refused(signers("2", "\"MA==MA==\"", ""), "signers[0]");
        refused(signers("2", "\"" + b64(A) + "\",\"" + b64(new byte[] {'n', 'o'}) + "\"", ""), "signers[1]");
    }

    public void testTooManyEntries() throws IOException {
        StringBuilder many = new StringBuilder("\"" + b64(A) + "\"");
        for (int i = 1; i <= SignersFile.MAX_ENTRIES; i++) {
            many.append(",\"").append(b64(A)).append('"');
        }
        refused(signers("1", many.toString(), ""), "more than " + SignersFile.MAX_ENTRIES);
        StringBuilder nodes = new StringBuilder(node(A, "0"));
        for (int i = 1; i <= SignersFile.MAX_ENTRIES; i++) {
            nodes.append(',').append(node(A, "0"));
        }
        refused(signers("3", "\"" + b64(A) + "\"", nodes.toString()), "more than " + SignersFile.MAX_ENTRIES);
    }

    public void testTooBig() throws IOException {
        char[] padding = new char[SignersFile.MAX_BYTES];
        Arrays.fill(padding, ' ');
        refused(signers("2", "\"" + b64(A) + "\"", "") + new String(padding), "bigger than");
    }

    public void testLineageEndsWithTheSigner() throws IOException {
        refused(signers("3", "\"" + b64(B) + "\"", node(A, "23") + "," + node(C, "0")), "does not end with");
    }

    public void testLineageOnlyForOneSignerOfV3() throws IOException {
        refused(signers("2", "\"" + b64(B) + "\"", node(A, "23") + "," + node(B, "0")), "lineage");
        refused(signers("3", "\"" + b64(B) + "\",\"" + b64(A) + "\"", node(A, "23") + "," + node(B, "0")),
                "lineage");
    }

    public void testFlagsAreAnInt() throws IOException {
        // The proof of rotation's flags are 32 bits that Android reads as an int; the writer keeps the bits.
        SignersFile file = read(signers("3", "\"" + b64(B) + "\"", node(A, "-2147483648") + "," + node(B, "-1")));
        check(file.lineage().get(0).flags == Integer.MIN_VALUE && file.lineage().get(1).flags == -1, "the bits");
        refused(signers("3", "\"" + b64(B) + "\"", node(A, "2147483648") + "," + node(B, "0")), "lineage[0]");
    }

    public void testLineageNodes() throws IOException {
        refused(signers("3", "\"" + b64(B) + "\"", node(A, "\"1\"") + "," + node(B, "0")), "lineage[0]");
        refused(signers("3", "\"" + b64(B) + "\"", "{\"flags\":1}," + node(B, "0")), "lineage[0]");
        refused(signers("3", "\"" + b64(B) + "\"", "\"" + b64(A) + "\"," + node(B, "0")), "lineage[0]");
        refused(signers("3", "\"" + b64(B) + "\"", "{\"certificate\":\"%%\",\"flags\":1}," + node(B, "0")),
                "writer's form");
        refused(signers("3", "\"" + b64(B) + "\"", "{\"certificate\":\"MA=\",\"flags\":1}," + node(B, "0")),
                "lineage[0]");
    }

    public void testNotUtf8() throws IOException {
        File file = File.createTempFile("signers", ".json");
        file.deleteOnExit();
        byte[] json = signers("2", "\"" + b64(A) + "\"", "").getBytes(StandardCharsets.UTF_8);
        byte[] broken = Arrays.copyOf(json, json.length + 1);
        broken[json.length] = (byte) 0xff;
        Files.write(file.toPath(), broken);
        try {
            SignersFile.read(file);
            throw new AssertionError("accepted a byte that is not UTF-8");
        } catch (IllegalStateException expected) {
            check(expected.getMessage().contains("UTF-8"), expected.getMessage());
        }
    }

    private static void check(boolean condition, String what) {
        if (!condition) {
            throw new AssertionError(what);
        }
    }

    public static void main(String[] args) throws Exception {
        int failed = 0, run = 0;
        for (Method test : SignersFileTest.class.getDeclaredMethods()) {
            if (!test.getName().startsWith("test")) {
                continue;
            }
            run++;
            try {
                test.invoke(new SignersFileTest());
            } catch (java.lang.reflect.InvocationTargetException thrown) {
                failed++;
                System.out.println("FAIL " + test.getName() + ": " + thrown.getCause());
            }
        }
        if (failed > 0) {
            System.out.println("SignersFileTest: " + failed + " of " + run + " failed");
            System.exit(1);
        }
        System.out.println("SignersFileTest: " + run + " of " + run + " fine");
    }
}
