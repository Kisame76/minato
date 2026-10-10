package io.github.kisame76.minato.test.keystore;

import android.app.Application;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Log;

import java.math.BigInteger;
import java.security.KeyStore;
import java.security.Security;
import java.security.spec.AlgorithmParameterSpec;
import java.security.spec.ECGenParameterSpec;
import java.util.Arrays;
import java.util.Date;
import java.util.TreeSet;

import javax.security.auth.x500.X500Principal;

/**
 * The keystore's parameter classes (batch D 2: GetSocial and AndroidX Security build a KeyGenParameterSpec before
 * they ask the AndroidKeyStore provider for a key): the specs of AndroidX's MasterKey and of EC, RSA and HMAC keys with
 * every getter, the Builder's defaults, its checks and their messages, the copies the spec hands out, and
 * KeyProperties' constants. Whether the provider is there differs (ADR-0059), so it goes to the tag
 * keystore-test-device, which is only shown.
 */
public class KeystoreApplication extends Application {
    static final String TAG = "keystore-test";

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private interface Action {
        Object run() throws Exception;
    }

    private static String outcome(Action action) {
        try {
            Object result = action.run();
            return result instanceof String[] ? Arrays.toString((String[]) result) : String.valueOf(result);
        } catch (Throwable e) {
            return e.getClass().getName() + ": " + e.getMessage();
        }
    }

    private static String time(Date date) {
        return date == null ? "null" : String.valueOf(date.getTime());
    }

    private static void describe(String name, KeyGenParameterSpec spec) {
        log(name + ": alias " + spec.getKeystoreAlias() + ", purposes " + spec.getPurposes() + ", key size "
                + spec.getKeySize() + ", parameters " + (spec.getAlgorithmParameterSpec() == null ? "null"
                : spec.getAlgorithmParameterSpec().getClass().getName()));
        log(name + ": block modes " + Arrays.toString(spec.getBlockModes()) + ", encryption paddings "
                + Arrays.toString(spec.getEncryptionPaddings()) + ", signature paddings "
                + Arrays.toString(spec.getSignaturePaddings()));
        log(name + ": digests specified " + spec.isDigestsSpecified() + ", digests " + outcome(spec::getDigests)
                + ", MGF1 digests specified " + spec.isMgf1DigestsSpecified() + ", MGF1 digests "
                + outcome(() -> new TreeSet<>(spec.getMgf1Digests())));
        log(name + ": certificate " + spec.getCertificateSubject() + ", serial " + spec.getCertificateSerialNumber()
                + ", from " + time(spec.getCertificateNotBefore()) + " to " + time(spec.getCertificateNotAfter()));
        log(name + ": valid from " + time(spec.getKeyValidityStart()) + ", for origination to "
                + time(spec.getKeyValidityForOriginationEnd()) + ", for consumption to "
                + time(spec.getKeyValidityForConsumptionEnd()));
        log(name + ": user authentication " + spec.isUserAuthenticationRequired() + ", for "
                + spec.getUserAuthenticationValidityDurationSeconds() + " s, type " + spec.getUserAuthenticationType()
                + ", while on body " + spec.isUserAuthenticationValidWhileOnBody() + ", invalidated by enrollment "
                + spec.isInvalidatedByBiometricEnrollment() + ", presence " + spec.isUserPresenceRequired()
                + ", confirmation " + spec.isUserConfirmationRequired());
        log(name + ": randomized encryption " + spec.isRandomizedEncryptionRequired() + ", unlocked device "
                + spec.isUnlockedDeviceRequired() + ", StrongBox " + spec.isStrongBoxBacked() + ", max usage count "
                + spec.getMaxUsageCount() + ", attestation challenge "
                + Arrays.toString(spec.getAttestationChallenge()) + ", attest key " + spec.getAttestKeyAlias()
                + ", device properties " + spec.isDevicePropertiesAttestationIncluded());
    }

    @Override
    public void onCreate() {
        super.onCreate();
        // AndroidX Security's MasterKey.KeyScheme.AES256_GCM.
        KeyGenParameterSpec master = new KeyGenParameterSpec.Builder("_androidx_security_master_key_",
                KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build();
        describe("MasterKey", master);
        log("MasterKey is an AlgorithmParameterSpec: " + (((Object) master) instanceof AlgorithmParameterSpec));

        KeyGenParameterSpec ec = new KeyGenParameterSpec.Builder("ec", KeyProperties.PURPOSE_SIGN
                | KeyProperties.PURPOSE_VERIFY)
                .setDigests(KeyProperties.DIGEST_SHA256, KeyProperties.DIGEST_SHA512)
                .setAlgorithmParameterSpec(new ECGenParameterSpec("secp256r1"))
                .setCertificateSubject(new X500Principal("CN=test, O=Minato"))
                .setCertificateSerialNumber(BigInteger.TEN)
                .setCertificateNotBefore(new Date(1000L))
                .setCertificateNotAfter(new Date(2000L))
                .setUserAuthenticationRequired(true)
                .setUserAuthenticationParameters(30, KeyProperties.AUTH_BIOMETRIC_STRONG
                        | KeyProperties.AUTH_DEVICE_CREDENTIAL)
                .setInvalidatedByBiometricEnrollment(false)
                .setUserAuthenticationValidWhileOnBody(true)
                .build();
        describe("EC", ec);
        log("EC parameters' name: " + ((ECGenParameterSpec) ec.getAlgorithmParameterSpec()).getName());

        KeyGenParameterSpec rsa = new KeyGenParameterSpec.Builder("rsa", KeyProperties.PURPOSE_ENCRYPT
                | KeyProperties.PURPOSE_DECRYPT | KeyProperties.PURPOSE_SIGN)
                .setKeySize(2048)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_RSA_OAEP,
                        KeyProperties.ENCRYPTION_PADDING_RSA_PKCS1)
                .setSignaturePaddings(KeyProperties.SIGNATURE_PADDING_RSA_PSS)
                .setDigests(KeyProperties.DIGEST_SHA256)
                .setMgf1Digests(KeyProperties.DIGEST_SHA1, KeyProperties.DIGEST_SHA256)
                .setRandomizedEncryptionRequired(false)
                .setMaxUsageCount(3)
                .setUnlockedDeviceRequired(true)
                .setIsStrongBoxBacked(true)
                .setAttestationChallenge(new byte[] {1, 2, 3})
                .setDevicePropertiesAttestationIncluded(true)
                .setUserPresenceRequired(true)
                .setUserConfirmationRequired(true)
                .build();
        describe("RSA", rsa);

        KeyGenParameterSpec hmac = new KeyGenParameterSpec.Builder("hmac", KeyProperties.PURPOSE_SIGN)
                .setDigests(KeyProperties.DIGEST_SHA256)
                .setKeyValidityStart(new Date(5000L))
                .setKeyValidityEnd(new Date(9000L))
                .setUserAuthenticationValidityDurationSeconds(-1)
                .build();
        describe("HMAC", hmac);

        // What the spec hands out are copies.
        hmac.getDigests()[0] = "changed";
        hmac.getKeyValidityStart().setTime(1L);
        rsa.getAttestationChallenge()[0] = 9;
        log("copies: digests " + Arrays.toString(hmac.getDigests()) + ", valid from "
                + time(hmac.getKeyValidityStart()) + ", challenge " + Arrays.toString(rsa.getAttestationChallenge()));

        // The Builder's checks.
        int encrypt = KeyProperties.PURPOSE_ENCRYPT;
        log("null alias: " + outcome(() -> new KeyGenParameterSpec.Builder(null, encrypt)));
        log("empty alias: " + outcome(() -> new KeyGenParameterSpec.Builder("", encrypt)));
        log("key size -1: " + outcome(() -> new KeyGenParameterSpec.Builder("a", encrypt).setKeySize(-1)));
        log("null parameters: " + outcome(() -> new KeyGenParameterSpec.Builder("a", encrypt)
                .setAlgorithmParameterSpec(null)));
        log("null subject: " + outcome(() -> new KeyGenParameterSpec.Builder("a", encrypt)
                .setCertificateSubject(null)));
        log("null serial: " + outcome(() -> new KeyGenParameterSpec.Builder("a", encrypt)
                .setCertificateSerialNumber(null)));
        log("null not before: " + outcome(() -> new KeyGenParameterSpec.Builder("a", encrypt)
                .setCertificateNotBefore(null)));
        log("null not after: " + outcome(() -> new KeyGenParameterSpec.Builder("a", encrypt)
                .setCertificateNotAfter(null)));
        log("not after before not before: " + outcome(() -> new KeyGenParameterSpec.Builder("a", encrypt)
                .setCertificateNotBefore(new Date(2000L)).setCertificateNotAfter(new Date(1000L)).build()));
        log("validity seconds -2: " + outcome(() -> new KeyGenParameterSpec.Builder("a", encrypt)
                .setUserAuthenticationValidityDurationSeconds(-2)));
        log("authentication timeout -1: " + outcome(() -> new KeyGenParameterSpec.Builder("a", encrypt)
                .setUserAuthenticationParameters(-1, KeyProperties.AUTH_BIOMETRIC_STRONG)));
        log("max usage count 0: " + outcome(() -> new KeyGenParameterSpec.Builder("a", encrypt)
                .setMaxUsageCount(0)));
        log("max usage count -1: " + outcome(() -> new KeyGenParameterSpec.Builder("a", encrypt)
                .setMaxUsageCount(-1).build().getMaxUsageCount()));
        log("purposes 0: " + outcome(() -> new KeyGenParameterSpec.Builder("a", 0).build().getPurposes()));
        log("null block modes: " + outcome(() -> Arrays.toString(new KeyGenParameterSpec.Builder("a", encrypt)
                .setBlockModes((String[]) null).build().getBlockModes())));
        log("null digests: " + outcome(() -> new KeyGenParameterSpec.Builder("a", encrypt)
                .setDigests((String[]) null).build().isDigestsSpecified()));

        log("algorithms: " + KeyProperties.KEY_ALGORITHM_RSA + " " + KeyProperties.KEY_ALGORITHM_EC + " "
                + KeyProperties.KEY_ALGORITHM_AES + " " + KeyProperties.KEY_ALGORITHM_HMAC_SHA256 + " "
                + KeyProperties.KEY_ALGORITHM_3DES);
        log("block modes and paddings: " + KeyProperties.BLOCK_MODE_ECB + " " + KeyProperties.BLOCK_MODE_CBC + " "
                + KeyProperties.BLOCK_MODE_CTR + " " + KeyProperties.BLOCK_MODE_GCM + " "
                + KeyProperties.ENCRYPTION_PADDING_PKCS7 + " " + KeyProperties.ENCRYPTION_PADDING_RSA_OAEP + " "
                + KeyProperties.SIGNATURE_PADDING_RSA_PKCS1);
        log("digests: " + KeyProperties.DIGEST_NONE + " " + KeyProperties.DIGEST_MD5 + " "
                + KeyProperties.DIGEST_SHA224 + " " + KeyProperties.DIGEST_SHA384);
        log("purposes: " + KeyProperties.PURPOSE_ENCRYPT + " " + KeyProperties.PURPOSE_DECRYPT + " "
                + KeyProperties.PURPOSE_SIGN + " " + KeyProperties.PURPOSE_VERIFY + " "
                + KeyProperties.PURPOSE_WRAP_KEY + " " + KeyProperties.PURPOSE_AGREE_KEY + " "
                + KeyProperties.PURPOSE_ATTEST_KEY);
        log("authenticators, origins, levels: " + KeyProperties.AUTH_BIOMETRIC_STRONG + " "
                + KeyProperties.AUTH_DEVICE_CREDENTIAL + " " + KeyProperties.ORIGIN_GENERATED + " "
                + KeyProperties.ORIGIN_IMPORTED + " " + KeyProperties.ORIGIN_UNKNOWN + " "
                + KeyProperties.ORIGIN_SECURELY_IMPORTED + " " + KeyProperties.SECURITY_LEVEL_UNKNOWN_SECURE + " "
                + KeyProperties.SECURITY_LEVEL_UNKNOWN + " " + KeyProperties.SECURITY_LEVEL_SOFTWARE + " "
                + KeyProperties.SECURITY_LEVEL_TRUSTED_ENVIRONMENT + " " + KeyProperties.SECURITY_LEVEL_STRONGBOX);

        Log.i(TAG + "-device", "provider AndroidKeyStore: " + (Security.getProvider("AndroidKeyStore") != null)
                + ", KeyStore: " + outcome(() -> KeyStore.getInstance("AndroidKeyStore").getType()));
        log("done");
    }
}
