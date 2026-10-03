package android.app;

import android.content.pm.Signature;
import android.content.pm.SigningDetails;
import android.content.pm.SigningInfo;

import com.android.minato.pm.SignersFile;

import java.io.File;
import java.io.IOException;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.Base64;

/**
 * What PackageManager gives of the app's signers (ADR-0039 b3.5b1), on the host's JVM with framework.jar's classes
 * (tools/package-manager-check.sh): PackageSigning with the taken SigningDetails and SigningInfo. No signer on the Mac
 * makes a rotated APK, so no test app compares a lineage with Android 15; these are the rules of PackageInfoCommonUtils
 * (198-221) and of SigningDetails as the tag has them.
 */
public final class PackageSigningTest {
    private static final byte[] OLDEST = {0x30, 0x01, 0x01};
    private static final byte[] OLDER = {0x30, 0x01, 0x02};
    private static final byte[] SIGNER = {0x30, 0x01, 0x03};
    private static final byte[] OTHER = {0x30, 0x01, 0x04};

    private static String b64(byte[] bytes) {
        return Base64.getEncoder().encodeToString(bytes);
    }

    /** signers.json in the writer's form. */
    private static SigningDetails details(int scheme, String lineage, byte[]... signers) throws IOException {
        StringBuilder list = new StringBuilder();
        for (byte[] signer : signers) {
            list.append(list.length() == 0 ? "\"" : ",\"").append(b64(signer)).append('"');
        }
        File file = File.createTempFile("signers", ".json");
        file.deleteOnExit();
        Files.write(file.toPath(), ("{\"format\":1,\"lineage\":[" + lineage + "],\"scheme\":" + scheme + ",\"signers\":["
                + list + "]}").getBytes(StandardCharsets.UTF_8));
        return PackageSigning.details(SignersFile.read(file));
    }

    /** A key rotated twice: the oldest may still roll back (8), the older has apksigner's defaults (23). */
    private static SigningDetails rotated(int scheme) throws IOException {
        return details(scheme, node(OLDEST, 8) + "," + node(OLDER, 23) + "," + node(SIGNER, 0), SIGNER);
    }

    private static String node(byte[] certificate, int flags) {
        return "{\"certificate\":\"" + b64(certificate) + "\",\"flags\":" + flags + "}";
    }

    private static boolean same(Signature[] signatures, byte[]... certificates) {
        if (signatures == null || signatures.length != certificates.length) {
            return false;
        }
        for (int i = 0; i < certificates.length; i++) {
            if (!Arrays.equals(signatures[i].toByteArray(), certificates[i])) {
                return false;
            }
        }
        return true;
    }

    public void testGetSignaturesGivesTheOldestCertificateOfALineage() throws IOException {
        check(same(PackageSigning.signatures(rotated(3)), OLDEST), "GET_SIGNATURES: the oldest certificate alone");
    }

    public void testTheHistoryIsTheLineageOldestFirst() throws IOException {
        SigningDetails details = rotated(3);
        SigningInfo info = PackageSigning.signingInfo(details);
        check(info.hasPastSigningCertificates() && !info.hasMultipleSigners(), "one signer with a past");
        check(same(info.getSigningCertificateHistory(), OLDEST, OLDER, SIGNER), "the history, the signer last");
        check(same(info.getApkContentsSigners(), SIGNER), "the APK's contents signer");
        Signature[] past = details.getPastSigningCertificates();
        check(past[0].getFlags() == 8 && past[1].getFlags() == 23, "the capabilities kept");
    }

    public void testAPastKeyIsOneOfTheAppsCertificates() throws Exception {
        // hasSigningCertificate: CERT_INPUT_RAW_X509 asks hasCertificate, CERT_INPUT_SHA256 hasSha256Certificate,
        // which needs a JCA provider in the guest (ADR-0046) and has the host's here.
        SigningDetails details = rotated(3);
        for (byte[] certificate : new byte[][] {OLDEST, OLDER, SIGNER}) {
            check(details.hasCertificate(certificate), "raw: " + Arrays.toString(certificate));
            check(details.hasSha256Certificate(MessageDigest.getInstance("SHA-256").digest(certificate)),
                    "SHA-256: " + Arrays.toString(certificate));
        }
        check(!details.hasCertificate(OTHER), "another certificate");
    }

    public void testV31IsV3() throws IOException {
        check(rotated(31).getSignatureSchemeVersion() == SigningDetails.SignatureSchemeVersion.SIGNING_BLOCK_V3,
                "v3.1's scheme");
        check(same(PackageSigning.signatures(rotated(31)), OLDEST), "v3.1's GET_SIGNATURES");
    }

    public void testWithoutALineageTheSignersInTheirOrder() throws IOException {
        SigningDetails details = details(1, "", SIGNER, OLDER);
        check(same(PackageSigning.signatures(details), SIGNER, OLDER), "GET_SIGNATURES: every signer");
        SigningInfo info = PackageSigning.signingInfo(details);
        check(info.hasMultipleSigners() && !info.hasPastSigningCertificates(), "two signers, no past");
        check(same(info.getApkContentsSigners(), SIGNER, OLDER), "the APK's contents signers");
        check(info.getSigningCertificateHistory() == null, "no history for several signers");
        check(!details.hasCertificate(OTHER), "another certificate");
    }

    public void testAnUnsignedAppHasNone() {
        check(PackageSigning.signatures(SigningDetails.UNKNOWN) == null, "no signatures");
        check(PackageSigning.signingInfo(SigningDetails.UNKNOWN) == null, "no signing info");
    }

    private static void check(boolean condition, String what) {
        if (!condition) {
            throw new AssertionError(what);
        }
    }

    public static void main(String[] args) throws Exception {
        int failed = 0, run = 0;
        for (Method test : PackageSigningTest.class.getDeclaredMethods()) {
            if (!test.getName().startsWith("test")) {
                continue;
            }
            run++;
            try {
                test.invoke(new PackageSigningTest());
            } catch (java.lang.reflect.InvocationTargetException thrown) {
                failed++;
                System.out.println("FAIL " + test.getName() + ": " + thrown.getCause());
            }
        }
        if (failed > 0) {
            System.out.println("PackageSigningTest: " + failed + " of " + run + " failed");
            System.exit(1);
        }
        System.out.println("PackageSigningTest: " + run + " of " + run + " fine");
    }
}
