// crypto-test: Java cryptography through the JCA, and an https URL (ADR-0046 test 1, step d). dalvikvm runs it as
// dalvikvm64 -Xint does on Android 15, and both print the same lines. It needs every provider of security.properties:
// Conscrypt's two, CertPathProvider and BouncyCastle's, since Android's JCA classes make all of them first; and
// okhttp's URL handlers, since javax.crypto's JceSecurity makes an http URL first. It prints the provider list;
// digests, a MAC and two ciphers with known answers and the provider that serves each; the default SecureRandom and a
// random UUID's form; RSA and EC signatures that verify; the default key store type with empty BKS and PKCS12 stores;
// the default SSLContext's protocols and cipher suites; an https URL's parts and the class of its connection, which
// openConnection() makes without connecting; and the system's CA certificates (ADR-0046, decision 4) as the default
// trust manager and the AndroidCAStore key store have them, with ISRG Root X1 among them.
import java.net.URL;
import java.net.URLConnection;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.KeyStore;
import java.security.MessageDigest;
import java.security.Provider;
import java.security.SecureRandom;
import java.security.Security;
import java.security.Signature;
import java.security.cert.X509Certificate;
import java.util.Arrays;
import java.util.Collections;
import java.util.UUID;
import javax.crypto.Cipher;
import javax.crypto.Mac;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.spec.IvParameterSpec;
import javax.crypto.spec.SecretKeySpec;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLParameters;
import javax.net.ssl.TrustManagerFactory;
import javax.net.ssl.X509TrustManager;

public class Main {
    public static void main(String[] args) throws Exception {
        for (Provider provider : Security.getProviders()) {
            System.out.println("provider: " + provider.getName() + ", " + provider.getInfo());
        }

        for (String algorithm : new String[] {"MD5", "SHA-1", "SHA-256", "SHA-512"}) {
            MessageDigest digest = MessageDigest.getInstance(algorithm);
            System.out.println(algorithm + "(\"abc\") from " + digest.getProvider().getName() + ": "
                    + hex(digest.digest(ascii("abc"))));
        }

        Mac hmac = Mac.getInstance("HmacSHA256");
        hmac.init(new SecretKeySpec(ascii("Jefe"), "HmacSHA256"));
        System.out.println("HmacSHA256, RFC 4231 case 2, from " + hmac.getProvider().getName() + ": "
                + hex(hmac.doFinal(ascii("what do ya want for nothing?"))));

        Cipher gcm = Cipher.getInstance("AES/GCM/NoPadding");
        gcm.init(Cipher.ENCRYPT_MODE, new SecretKeySpec(bytes("feffe9928665731c6d6a8f9467308308"), "AES"),
                new GCMParameterSpec(128, bytes("cafebabefacedbaddecaf888")));
        gcm.updateAAD(bytes("feedfacedeadbeeffeedfacedeadbeefabaddad2"));
        System.out.println("AES/GCM/NoPadding, GCM test case 4, from " + gcm.getProvider().getName() + ": "
                + hex(gcm.doFinal(bytes("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
                + "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39"))));

        Cipher cbc = Cipher.getInstance("AES/CBC/PKCS5Padding");
        cbc.init(Cipher.ENCRYPT_MODE, new SecretKeySpec(bytes("2b7e151628aed2a6abf7158809cf4f3c"), "AES"),
                new IvParameterSpec(bytes("000102030405060708090a0b0c0d0e0f")));
        System.out.println("AES/CBC/PKCS5Padding, SP 800-38A F.2.1, from " + cbc.getProvider().getName() + ": "
                + hex(cbc.doFinal(bytes("6bc1bee22e409f96e93d7e117393172a"))));

        SecureRandom random = new SecureRandom();
        byte[] noise = new byte[32];
        random.nextBytes(noise);
        System.out.println("SecureRandom: " + random.getAlgorithm() + " from " + random.getProvider().getName()
                + ", 32 bytes, not all zero: " + yes(!Arrays.equals(noise, new byte[32])));
        UUID uuid = UUID.randomUUID();
        System.out.println("UUID.randomUUID(): version " + uuid.version() + ", variant " + uuid.variant());

        signature("RSA", 2048, "SHA256withRSA");
        signature("EC", 256, "SHA256withECDSA");

        System.out.println("KeyStore.getDefaultType(): " + KeyStore.getDefaultType());
        for (String type : new String[] {"BKS", "PKCS12"}) {
            KeyStore store = KeyStore.getInstance(type);
            store.load(null, null);
            System.out.println("empty " + type + " store from " + store.getProvider().getName() + ": "
                    + store.size() + " entries");
        }

        SSLContext context = SSLContext.getDefault();
        SSLParameters parameters = context.getDefaultSSLParameters();
        System.out.println("SSLContext.getDefault(): " + context.getProtocol() + " from "
                + context.getProvider().getName() + ", protocols " + Arrays.toString(parameters.getProtocols()));
        System.out.println("cipher suites: " + Arrays.toString(parameters.getCipherSuites()));

        URL url = new URL("https://example.com/a?b");
        System.out.println("URL " + url + ": protocol " + url.getProtocol() + ", host " + url.getHost() + ", port "
                + url.getPort() + ", default port " + url.getDefaultPort() + ", path " + url.getPath() + ", query "
                + url.getQuery());
        URLConnection connection = url.openConnection();
        System.out.println("openConnection(): " + connection.getClass().getName());
        TrustManagerFactory factory = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
        factory.init((KeyStore) null);
        X509TrustManager trust = (X509TrustManager) factory.getTrustManagers()[0];
        boolean isrg = false;
        for (X509Certificate issuer : trust.getAcceptedIssuers()) {
            isrg |= issuer.getSubjectX500Principal().getName().contains("CN=ISRG Root X1");
        }
        System.out.println("default trust manager (" + factory.getAlgorithm() + " from "
                + factory.getProvider().getName() + "): " + trust.getAcceptedIssuers().length
                + " issuers, ISRG Root X1 among them: " + yes(isrg));
        KeyStore system = KeyStore.getInstance("AndroidCAStore");
        system.load(null);
        int roots = 0;
        for (String alias : Collections.list(system.aliases())) {
            if (alias.startsWith("system:")) roots++;
        }
        System.out.println("AndroidCAStore from " + system.getProvider().getName() + ": " + roots
                + " system certificates, system:6187b673.0 there: " + yes(system.containsAlias("system:6187b673.0")));
    }

    static void signature(String keyAlgorithm, int size, String signatureAlgorithm) throws Exception {
        KeyPairGenerator generator = KeyPairGenerator.getInstance(keyAlgorithm);
        generator.initialize(size);
        KeyPair pair = generator.generateKeyPair();
        byte[] message = ascii("a message to sign");
        Signature signer = Signature.getInstance(signatureAlgorithm);
        signer.initSign(pair.getPrivate());
        signer.update(message);
        byte[] signed = signer.sign();
        Signature verifier = Signature.getInstance(signatureAlgorithm);
        verifier.initVerify(pair.getPublic());
        verifier.update(message);
        boolean verifies = verifier.verify(signed);
        message[0] ^= 1;
        verifier.initVerify(pair.getPublic());
        verifier.update(message);
        boolean changed = verifier.verify(signed);
        System.out.println(signatureAlgorithm + " with a " + size + "-bit " + keyAlgorithm + " key from "
                + generator.getProvider().getName() + ", signed by " + signer.getProvider().getName() + ": verifies "
                + yes(verifies) + ", a changed message: " + yes(changed));
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
