package com.android.minato.pm;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Base64;
import java.util.Collections;
import java.util.HashSet;
import java.util.Iterator;
import java.util.List;
import java.util.Set;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;
import org.json.JSONTokener;

/**
 * Who signed the app, as the import wrote it into the version's folder (ADR-0039 b3.5b1, ADR-0034 decision 2):
 * signers.json, from which ManifestPackage builds the SigningDetails that PackageManager answers. The import read the
 * signing block once, so the guest parses no part of the APK's signatures:
 * <pre>
 * {"format":1,"lineage":[{"certificate":"&lt;base64 DER&gt;","flags":N},...],"scheme":3,
 *  "signers":["&lt;base64 DER&gt;",...]}
 * </pre>
 * <ul>
 *   <li>scheme: the scheme ADR-0034 decision 2 chose, 1 (JAR), 2, 3 or 31 (v3.1);</li>
 *   <li>signers: that scheme's signers' certificates, in the APK's order;</li>
 *   <li>lineage: a rotated key's proof of rotation, oldest first and the current key last, each with its
 *       capabilities as the int Android reads; only for one signer of v3 or v3.1, and empty without a
 *       rotation.</li>
 * </ul>
 * Only a verified APK has the file; an unsigned or unverified one has none. The file is Minato's own, so anything
 * but the bytes the writer writes, the keys sorted and nothing spaced or escaped, is a bug or a change of the file,
 * not data to guess at: every problem throws IllegalStateException, naming the file, and the app does not start. At
 * most {@link #MAX_BYTES} and {@link #MAX_ENTRIES} entries per list, as far above a real APK's one to three
 * certificates as APKKit's limit, which the writer keeps too. Plain Java with org.json, so
 * tools/framework-host-check.sh runs it on the host's JVM.
 */
public final class SignersFile {
    public static final int FORMAT = 1;
    public static final int MAX_BYTES = 1 << 20;
    public static final int MAX_ENTRIES = 64;
    private static final Set<Integer> SCHEMES = Set.of(1, 2, 3, 31);
    private static final Set<String> KEYS = Set.of("format", "scheme", "signers", "lineage");
    private static final Set<String> NODE_KEYS = Set.of("certificate", "flags");
    /** Every X.509 certificate's DER encoding is an ASN.1 SEQUENCE. */
    private static final byte SEQUENCE = 0x30;
    /** The deepest the writer nests: the object, the lineage, a level. */
    private static final int MAX_DEPTH = 3;

    /** 1, 2, 3 or 31. */
    public final int scheme;
    private final List<byte[]> mSigners;
    private final List<Node> mLineage;

    /** A certificate of the lineage with the capabilities the rotation left it (SigningCertificateLineage). */
    public static final class Node {
        private final byte[] mCertificate;
        public final int flags;

        Node(byte[] certificate, int flags) {
            mCertificate = certificate;
            this.flags = flags;
        }

        /** The certificate's DER encoding, a copy. */
        public byte[] certificate() {
            return mCertificate.clone();
        }
    }

    private SignersFile(int scheme, List<byte[]> signers, List<Node> lineage) {
        this.scheme = scheme;
        mSigners = signers;
        mLineage = lineage;
    }

    /** The signers' certificates, DER encoded, as copies in an unmodifiable list. */
    public List<byte[]> signers() {
        List<byte[]> copies = new ArrayList<>(mSigners.size());
        for (byte[] signer : mSigners) {
            copies.add(signer.clone());
        }
        return Collections.unmodifiableList(copies);
    }

    /** Oldest first, the current key last; empty without a rotation. */
    public List<Node> lineage() {
        return mLineage;
    }

    public static SignersFile read(File file) {
        String where = file.getPath();
        byte[] bytes;
        try (InputStream in = new FileInputStream(file)) {
            // One byte more than allowed shows a bigger file without reading all of it.
            bytes = in.readNBytes(MAX_BYTES + 1);
        } catch (IOException e) {
            throw new IllegalStateException(where + ": cannot read it: " + e, e);
        }
        if (bytes.length > MAX_BYTES) {
            throw problem(where, "bigger than " + MAX_BYTES + " bytes");
        }
        String text;
        try {
            text = StandardCharsets.UTF_8.newDecoder().decode(ByteBuffer.wrap(bytes)).toString();
        } catch (CharacterCodingException e) {
            throw problem(where, "not UTF-8");
        }
        form(text, where);
        SignersFile signers = parse(text, where);
        // The writer's bytes only: org.json also takes duplicate keys, comments, octal and hex numbers, other quotes
        // and separators, and base64 without its padding, none of which APKKit writes.
        int at = Arrays.mismatch(bytes, signers.encode());
        if (at >= 0) {
            throw problem(where, "not the writer's form from byte " + at);
        }
        return signers;
    }

    /**
     * Before org.json reads the text: base64 in the strings, JSON's structure and numbers outside them, and no deeper
     * than the writer nests, so that org.json's recursive descent cannot overflow the stack and everything fails with
     * the file's name. Without quotes, escapes or words outside the strings, the strings here are org.json's.
     */
    private static void form(String text, String where) {
        int depth = 0;
        boolean inString = false;
        for (int i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            if (c == '"') {
                inString = !inString;
            } else if (inString ? !isBase64(c) : "{}[]:,-.0123456789".indexOf(c) < 0) {
                throw problem(where, String.format("U+%04X at %d is not in the writer's form", (int) c, i));
            } else if (c == '{' || c == '[') {
                if (++depth > MAX_DEPTH) {
                    throw problem(where, "nested deeper than the writer's " + MAX_DEPTH + " levels");
                }
            } else if (c == '}' || c == ']') {
                depth--;
            }
        }
    }

    private static boolean isBase64(char c) {
        return c >= 'A' && c <= 'Z' || c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '+' || c == '/' || c == '=';
    }

    private static SignersFile parse(String text, String where) {
        Object value;
        boolean more;
        try {
            JSONTokener tokener = new JSONTokener(text);
            value = tokener.nextValue();
            more = tokener.nextClean() != '\0';
        } catch (JSONException | RuntimeException e) {
            throw problem(where, "not JSON: " + e.getMessage());
        }
        if (!(value instanceof JSONObject)) {
            throw problem(where, "not a JSON object");
        }
        if (more) {
            throw problem(where, "something after the object");
        }
        JSONObject json = (JSONObject) value;
        keys(json, KEYS, where, "");
        Object format = json.opt("format");
        if (!(format instanceof Integer) || (Integer) format != FORMAT) {
            throw problem(where, "format " + format + ", not " + FORMAT);
        }
        Object scheme = json.opt("scheme");
        if (!(scheme instanceof Integer) || !SCHEMES.contains(scheme)) {
            throw problem(where, "scheme " + scheme + " is none of 1, 2, 3 and 31");
        }
        JSONArray signerArray = array(json, "signers", where);
        if (signerArray.length() == 0) {
            throw problem(where, "no signers, which an unsigned APK shows by having no file");
        }
        List<byte[]> signers = new ArrayList<>();
        for (int i = 0; i < signerArray.length(); i++) {
            signers.add(certificate(signerArray.opt(i), where, "signers[" + i + "]"));
        }
        JSONArray nodeArray = array(json, "lineage", where);
        List<Node> lineage = new ArrayList<>();
        for (int i = 0; i < nodeArray.length(); i++) {
            String name = "lineage[" + i + "]";
            if (!(nodeArray.opt(i) instanceof JSONObject)) {
                throw problem(where, name + " is not an object");
            }
            JSONObject node = (JSONObject) nodeArray.opt(i);
            keys(node, NODE_KEYS, where, name + ": ");
            // The proof of rotation's 32 bits, as Android reads them: an int.
            Object flags = node.opt("flags");
            if (!(flags instanceof Integer)) {
                throw problem(where, name + ": flags " + flags + " is not an int");
            }
            lineage.add(new Node(certificate(node.opt("certificate"), where, name + ".certificate"), (Integer) flags));
        }
        if (!lineage.isEmpty()) {
            if ((Integer) scheme != 3 && (Integer) scheme != 31 || signers.size() != 1) {
                throw problem(where, "a lineage belongs to one signer of v3 or v3.1, not to "
                        + signers.size() + " of scheme " + scheme);
            }
            if (!Arrays.equals(lineage.get(lineage.size() - 1).mCertificate, signers.get(0))) {
                throw problem(where, "the lineage does not end with the signer's certificate");
            }
            // As ApkSigningBlockUtils.verifyProofOfRotationStruct: every certificate once.
            Set<ByteBuffer> seen = new HashSet<>();
            for (int i = 0; i < lineage.size(); i++) {
                if (!seen.add(ByteBuffer.wrap(lineage.get(i).mCertificate))) {
                    throw problem(where, "lineage[" + i + "] repeats a certificate, which Android refuses");
                }
            }
        }
        return new SignersFile((Integer) scheme, Collections.unmodifiableList(signers),
                Collections.unmodifiableList(lineage));
    }

    /** The bytes APKKit's SignersFile writes for this content. */
    private byte[] encode() {
        Base64.Encoder base64 = Base64.getEncoder();
        StringBuilder out = new StringBuilder("{\"format\":").append(FORMAT).append(",\"lineage\":[");
        for (int i = 0; i < mLineage.size(); i++) {
            Node node = mLineage.get(i);
            out.append(i == 0 ? "" : ",").append("{\"certificate\":\"").append(base64.encodeToString(node.mCertificate))
                    .append("\",\"flags\":").append(node.flags).append('}');
        }
        out.append("],\"scheme\":").append(scheme).append(",\"signers\":[");
        for (int i = 0; i < mSigners.size(); i++) {
            out.append(i == 0 ? "\"" : ",\"").append(base64.encodeToString(mSigners.get(i))).append('"');
        }
        return out.append("]}").toString().getBytes(StandardCharsets.UTF_8);
    }

    private static void keys(JSONObject json, Set<String> known, String where, String prefix) {
        for (Iterator<String> keys = json.keys(); keys.hasNext(); ) {
            String key = keys.next();
            if (!known.contains(key)) {
                throw problem(where, prefix + "unknown key " + key);
            }
        }
    }

    private static JSONArray array(JSONObject json, String key, String where) {
        Object value = json.opt(key);
        if (!(value instanceof JSONArray)) {
            throw problem(where, key + " is not an array");
        }
        JSONArray array = (JSONArray) value;
        if (array.length() > MAX_ENTRIES) {
            throw problem(where, key + " has more than " + MAX_ENTRIES + " entries");
        }
        return array;
    }

    private static byte[] certificate(Object value, String where, String name) {
        if (!(value instanceof String)) {
            throw problem(where, name + " is not a string");
        }
        byte[] der;
        try {
            der = Base64.getDecoder().decode((String) value);
        } catch (IllegalArgumentException e) {
            throw problem(where, name + " is not base64: " + e.getMessage());
        }
        if (der.length == 0 || der[0] != SEQUENCE) {
            throw problem(where, name + " is not a DER certificate");
        }
        return der;
    }

    private static IllegalStateException problem(String where, String what) {
        return new IllegalStateException(where + ": " + what);
    }
}
