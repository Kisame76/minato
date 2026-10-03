// conscrypt-test: Conscrypt's provider on ART as guest code (ADR-0046 c). dalvikvm runs it as dalvikvm64 -Xint does
// on Android 15, and both print the same lines. It makes Conscrypt's OpenSSLProvider itself and takes each service's
// own object from it, as the JCA does inside: SHA-256, HmacSHA256 (RFC 4231 case 2), AES-128-GCM (GCM test case 4:
// sealed, opened, and refused with a changed tag), an EC key pair that signs and verifies, and a fixed self-signed
// X.509 certificate. Not through MessageDigest, Cipher and the other JCA classes: on Android they first make every
// provider that security.properties lists, BouncyCastle's too, which comes in step d). Making the provider loads
// libjavacrypto.so from Conscrypt's folder, through the namespace com_android_conscrypt (ADR-0046 a).
import java.io.ByteArrayInputStream;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.security.Key;
import java.security.KeyPair;
import java.security.KeyPairGeneratorSpi;
import java.security.MessageDigestSpi;
import java.security.PrivateKey;
import java.security.Provider;
import java.security.PublicKey;
import java.security.SecureRandom;
import java.security.SignatureSpi;
import java.security.cert.CertificateFactorySpi;
import java.security.cert.X509Certificate;
import java.security.spec.AlgorithmParameterSpec;
import java.security.spec.ECGenParameterSpec;
import java.util.Arrays;
import java.util.Base64;
import javax.crypto.AEADBadTagException;
import javax.crypto.Cipher;
import javax.crypto.CipherSpi;
import javax.crypto.MacSpi;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.spec.SecretKeySpec;

public class Main {
    // Made for this test with OpenSSL: P-256, SHA256withECDSA, O=Minato, CN=conscrypt-test, valid for 100 years.
    static final String CERTIFICATE =
            "MIIBnTCCAUOgAwIBAgIGTWluYXRvMAoGCCqGSM49BAMCMCoxDzANBgNVBAoMBk1pbmF0bzEXMBUGA1UEAwwOY29uc2NyeXB0LXRl"
            + "c3QwIBcNMjYxMDAzMjE1NTI0WhgPMjEyNjA5MDkyMTU1MjRaMCoxDzANBgNVBAoMBk1pbmF0bzEXMBUGA1UEAwwOY29uc2NyeXB0"
            + "LXRlc3QwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAASAbhPaaUgXBeE4XERg97/7ETl8/pCkEL1j8FPW8EQIdRWFwH509MY7u6AV"
            + "i1BdqLhnrVmlU7oXivCQ4vqULs2po1MwUTAdBgNVHQ4EFgQUtSgbjH5j81Td0kzRAu0KiCINMfowHwYDVR0jBBgwFoAUtSgbjH5j"
            + "81Td0kzRAu0KiCINMfowDwYDVR0TAQH/BAUwAwEB/zAKBggqhkjOPQQDAgNIADBFAiBf5oSMDZnLtw3NcBnM3+G5MJiegWTnMqB5"
            + "Lvjn3mhcnQIhAL6qucRah+KSia3mWCB/AX4GtFta1bdUZVETcwr1jE0T";

    static Provider provider;

    public static void main(String[] args) throws Exception {
        provider = (Provider) Class.forName("com.android.org.conscrypt.OpenSSLProvider")
                .getDeclaredConstructor().newInstance();
        System.out.println("provider: " + provider.getName() + ", " + provider.getInfo() + ", "
                + provider.getServices().size() + " services");
        System.out.println("SHA-256(\"abc\"): " + hex(sha256(ascii("abc"))));

        Object hmac = service("Mac", "HmacSHA256");
        call(hmac, MacSpi.class, "engineInit", new Class<?>[] {Key.class, AlgorithmParameterSpec.class},
                new SecretKeySpec(ascii("Jefe"), "HmacSHA256"), null);
        byte[] message = ascii("what do ya want for nothing?");
        call(hmac, MacSpi.class, "engineUpdate", new Class<?>[] {byte[].class, int.class, int.class}, message, 0,
                message.length);
        System.out.println("HmacSHA256, RFC 4231 case 2: "
                + hex((byte[]) call(hmac, MacSpi.class, "engineDoFinal", new Class<?>[0])));

        aesGcm();
        ecdsa();
        certificate();
    }

    static byte[] sha256(byte[] data) throws Exception {
        Object digest = service("MessageDigest", "SHA-256");
        call(digest, MessageDigestSpi.class, "engineUpdate", new Class<?>[] {byte[].class, int.class, int.class}, data,
                0, data.length);
        return (byte[]) call(digest, MessageDigestSpi.class, "engineDigest", new Class<?>[0]);
    }

    static void aesGcm() throws Exception {
        SecretKeySpec key = new SecretKeySpec(bytes("feffe9928665731c6d6a8f9467308308"), "AES");
        GCMParameterSpec parameters = new GCMParameterSpec(128, bytes("cafebabefacedbaddecaf888"));
        byte[] plain = bytes("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
                + "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39");
        byte[] aad = bytes("feedfacedeadbeeffeedfacedeadbeefabaddad2");
        byte[] sealed = gcm(Cipher.ENCRYPT_MODE, key, parameters, aad, plain);
        boolean opens = Arrays.equals(gcm(Cipher.DECRYPT_MODE, key, parameters, aad, sealed), plain);
        byte[] changed = sealed.clone();
        changed[changed.length - 1] ^= 1;
        boolean refused = false;
        try {
            gcm(Cipher.DECRYPT_MODE, key, parameters, aad, changed);
        } catch (AEADBadTagException expected) {
            refused = true;
        }
        System.out.println("AES-128-GCM, GCM test case 4: " + hex(sealed) + ", opens: " + yes(opens)
                + ", with a changed tag: " + (refused ? "refused" : "OPENED"));
    }

    static byte[] gcm(int mode, Key key, GCMParameterSpec parameters, byte[] aad, byte[] input) throws Exception {
        Object cipher = service("Cipher", "AES/GCM/NoPadding");
        call(cipher, CipherSpi.class, "engineInit",
                new Class<?>[] {int.class, Key.class, AlgorithmParameterSpec.class, SecureRandom.class}, mode, key,
                parameters, null);
        call(cipher, CipherSpi.class, "engineUpdateAAD", new Class<?>[] {byte[].class, int.class, int.class}, aad, 0,
                aad.length);
        return (byte[]) call(cipher, CipherSpi.class, "engineDoFinal",
                new Class<?>[] {byte[].class, int.class, int.class}, input, 0, input.length);
    }

    static void ecdsa() throws Exception {
        KeyPairGeneratorSpi generator = (KeyPairGeneratorSpi) service("KeyPairGenerator", "EC");
        generator.initialize(new ECGenParameterSpec("secp256r1"), null);
        KeyPair pair = generator.generateKeyPair();
        byte[] message = ascii("a message to sign");
        Object signer = service("Signature", "SHA256withECDSA");
        call(signer, SignatureSpi.class, "engineInitSign", new Class<?>[] {PrivateKey.class}, pair.getPrivate());
        call(signer, SignatureSpi.class, "engineUpdate", new Class<?>[] {byte[].class, int.class, int.class}, message,
                0, message.length);
        byte[] signature = (byte[]) call(signer, SignatureSpi.class, "engineSign", new Class<?>[0]);
        boolean verifies = verify(pair.getPublic(), message, signature);
        message[0] ^= 1;
        boolean changed = verify(pair.getPublic(), message, signature);
        System.out.println("EC key pair: " + pair.getPublic().getAlgorithm() + ", " + pair.getPublic().getFormat() + " "
                + pair.getPublic().getEncoded().length + " bytes, " + pair.getPrivate().getFormat()
                + "; SHA256withECDSA verifies: " + yes(verifies) + ", a changed message: " + yes(changed));
    }

    static boolean verify(PublicKey key, byte[] message, byte[] signature) throws Exception {
        Object verifier = service("Signature", "SHA256withECDSA");
        call(verifier, SignatureSpi.class, "engineInitVerify", new Class<?>[] {PublicKey.class}, key);
        call(verifier, SignatureSpi.class, "engineUpdate", new Class<?>[] {byte[].class, int.class, int.class},
                message, 0, message.length);
        return (Boolean) call(verifier, SignatureSpi.class, "engineVerify", new Class<?>[] {byte[].class}, signature);
    }

    static void certificate() throws Exception {
        CertificateFactorySpi factory = (CertificateFactorySpi) service("CertificateFactory", "X.509");
        X509Certificate certificate = (X509Certificate) factory.engineGenerateCertificate(
                new ByteArrayInputStream(Base64.getDecoder().decode(CERTIFICATE)));
        System.out.println("certificate: " + certificate.getType() + " version " + certificate.getVersion()
                + ", serial " + certificate.getSerialNumber().toString(16));
        System.out.println("subject: " + certificate.getSubjectX500Principal().getName()
                + "; issuer: " + certificate.getIssuerX500Principal().getName());
        System.out.println("valid from " + certificate.getNotBefore().getTime() + " to "
                + certificate.getNotAfter().getTime() + " (ms)");
        System.out.println("signature: " + certificate.getSigAlgName() + " (" + certificate.getSigAlgOID()
                + "), key: " + certificate.getPublicKey().getAlgorithm() + ", basic constraints: "
                + certificate.getBasicConstraints());
        boolean verifies = true;
        try {
            certificate.verify(certificate.getPublicKey(), provider);
        } catch (Exception failure) {
            verifies = false;
        }
        System.out.println("self-signature verifies: " + yes(verifies) + "; SHA-256 of the encoding: "
                + hex(sha256(certificate.getEncoded())));
    }

    // The provider's own object for a service, as the JCA makes it.
    static Object service(String type, String algorithm) throws Exception {
        return provider.getService(type, algorithm).newInstance(null);
    }

    // Calls the protected SPI method `name` that the JCA class of the service would call.
    static Object call(Object target, Class<?> type, String name, Class<?>[] parameters, Object... arguments)
            throws Exception {
        Method method = type.getDeclaredMethod(name, parameters);
        method.setAccessible(true);
        try {
            return method.invoke(target, arguments);
        } catch (InvocationTargetException failure) {
            throw (Exception) failure.getCause();
        }
    }

    static byte[] ascii(String text) throws Exception {
        return text.getBytes("US-ASCII");
    }

    static byte[] bytes(String hex) {
        byte[] result = new byte[hex.length() / 2];
        for (int index = 0; index < result.length; index++) {
            result[index] = (byte) Integer.parseInt(hex.substring(2 * index, 2 * index + 2), 16);
        }
        return result;
    }

    static String hex(byte[] bytes) {
        StringBuilder result = new StringBuilder();
        for (byte value : bytes) {
            result.append(String.format("%02x", value & 0xff));
        }
        return result.toString();
    }

    static String yes(boolean value) {
        return value ? "yes" : "no";
    }
}
