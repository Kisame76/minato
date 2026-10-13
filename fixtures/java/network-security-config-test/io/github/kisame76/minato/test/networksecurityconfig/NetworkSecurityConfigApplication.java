package io.github.kisame76.minato.test.networksecurityconfig;

import android.app.Application;
import android.net.http.X509TrustManagerExtensions;
import android.security.NetworkSecurityPolicy;
import android.util.Log;

import java.io.ByteArrayInputStream;
import java.security.KeyStore;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import javax.net.ssl.TrustManagerFactory;
import javax.net.ssl.X509TrustManager;

/**
 * The test of an app's network_security_config.xml, without a window (framework breadth, step (d)). The config has a
 * base config without clear text, a domain config that permits it for cleartext.example.com and its subdomains and for
 * exact.example.org alone, a pin-set on pinned.example.com that no chain fits and debug-overrides, which a
 * non-debuggable app ignores. It logs under the tag network-security-config-test the policy's answer per host, and the
 * trust manager's verdict on a chain of one public root (ISRG Root X1, which both Android 15's and Minato's system
 * store hold) for hosts with and without the pin-set. Public API only. tools/android-reference.sh takes the log from
 * Android 15, and tools/network-security-config-check.sh compares Minato's with it.
 */
public class NetworkSecurityConfigApplication extends Application {
    private static final String TAG = "network-security-config-test";

    private static final String ROOT =
            "-----BEGIN CERTIFICATE-----\n" +
            "MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw\n" +
            "TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh\n" +
            "cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4\n" +
            "WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu\n" +
            "ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY\n" +
            "MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc\n" +
            "h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+\n" +
            "0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U\n" +
            "A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW\n" +
            "T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH\n" +
            "B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC\n" +
            "B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv\n" +
            "KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn\n" +
            "OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn\n" +
            "jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw\n" +
            "qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI\n" +
            "rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV\n" +
            "HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq\n" +
            "hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL\n" +
            "ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ\n" +
            "3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK\n" +
            "NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5\n" +
            "ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur\n" +
            "TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC\n" +
            "jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc\n" +
            "oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq\n" +
            "4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA\n" +
            "mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d\n" +
            "emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=\n" +
            "-----END CERTIFICATE-----\n";

    private interface Body {
        void run() throws Exception;
    }

    private static void probe(String name, Body body) {
        try {
            body.run();
        } catch (Throwable t) {
            Log.i(TAG, name + " failed: " + t.getClass().getName() + ": " + t.getMessage());
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        probe("policy", () -> {
            NetworkSecurityPolicy policy = NetworkSecurityPolicy.getInstance();
            String[] hosts = {"cleartext.example.com", "sub.cleartext.example.com", "a.b.cleartext.example.com",
                "exact.example.org", "sub.exact.example.org", "example.com", "pinned.example.com", "localhost",
                "CLEARTEXT.EXAMPLE.COM"};
            StringBuilder out = new StringBuilder();
            for (String host : hosts) {
                out.append(host).append('=').append(policy.isCleartextTrafficPermitted(host)).append(' ');
            }
            Log.i(TAG, "base " + policy.isCleartextTrafficPermitted());
            Log.i(TAG, "hosts " + out.toString().trim());
        });
        probe("trust", () -> {
            CertificateFactory factory = CertificateFactory.getInstance("X.509");
            X509Certificate root = (X509Certificate) factory.generateCertificate(
                    new ByteArrayInputStream(ROOT.getBytes("US-ASCII")));
            Log.i(TAG, "root " + root.getSubjectX500Principal().getName());
            TrustManagerFactory tmf = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
            tmf.init((KeyStore) null);
            X509TrustManager manager = (X509TrustManager) tmf.getTrustManagers()[0];
            X509TrustManagerExtensions extensions = new X509TrustManagerExtensions(manager);
            X509Certificate[] chain = {root};
            for (String host : new String[] {"example.com", "pinned.example.com", "cleartext.example.com"}) {
                try {
                    Log.i(TAG, "chain " + host + " trusted " + extensions.checkServerTrusted(chain, "RSA", host).size());
                } catch (java.security.cert.CertificateException e) {
                    Log.i(TAG, "chain " + host + " " + e.getClass().getSimpleName() + ": " + e.getMessage());
                }
            }
            Log.i(TAG, "isUserAdded " + extensions.isUserAddedCertificate(root));
        });
        Log.i(TAG, "done");
    }
}
