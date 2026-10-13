package io.github.kisame76.minato.test.networksecurity;

import android.app.Application;
import android.security.NetworkSecurityPolicy;
import android.util.Log;

import java.net.HttpURLConnection;
import java.net.URL;
import java.security.KeyStore;
import java.security.Provider;
import java.security.Security;
import javax.net.ssl.SSLContext;
import javax.net.ssl.TrustManager;
import javax.net.ssl.TrustManagerFactory;
import javax.net.ssl.X509TrustManager;

/**
 * The test of the app's network security policy, without a window (framework breadth, step (d)). It logs under the tag
 * network-security-test the policy's answers for an app without a network security config, the security providers in
 * order, the default trust manager's provider, class and whether it trusts any issuers, the default SSL context's
 * provider and what an http:// connection to a closed local port throws first. Public API only.
 * tools/android-reference.sh takes the log from Android 15, and tools/network-security-check.sh compares Minato's with it.
 */
public class NetworkSecurityApplication extends Application {
    private static final String TAG = "network-security-test";

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
            Log.i(TAG, "cleartext " + policy.isCleartextTrafficPermitted() + " example.com="
                    + policy.isCleartextTrafficPermitted("example.com") + " localhost="
                    + policy.isCleartextTrafficPermitted("localhost"));
            Log.i(TAG, "same instance " + (policy == NetworkSecurityPolicy.getInstance()));
        });
        probe("providers", () -> {
            StringBuilder names = new StringBuilder();
            for (Provider provider : Security.getProviders()) {
                // The key store providers belong to the keystore step, not to the network security config.
                if (provider.getName().startsWith("AndroidKeyStore")) {
                    continue;
                }
                names.append(provider.getName()).append(' ');
            }
            Log.i(TAG, "providers " + names.toString().trim());
        });
        probe("trust", () -> {
            TrustManagerFactory factory = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
            Log.i(TAG, "factory " + TrustManagerFactory.getDefaultAlgorithm() + " " + factory.getProvider().getName());
            factory.init((KeyStore) null);
            TrustManager[] managers = factory.getTrustManagers();
            Log.i(TAG, "managers " + managers.length + " " + managers[0].getClass().getName());
            X509TrustManager manager = (X509TrustManager) managers[0];
            Log.i(TAG, "issuers " + (manager.getAcceptedIssuers().length > 100));
        });
        probe("context", () -> {
            SSLContext context = SSLContext.getDefault();
            Log.i(TAG, "context " + context.getProtocol() + " " + context.getProvider().getName());
        });
        // Off the main thread, which Android's StrictMode would stop first.
        Thread thread = new Thread(() -> probe("http", () -> {
            HttpURLConnection connection = (HttpURLConnection) new URL("http://localhost:1/").openConnection();
            connection.setConnectTimeout(2000);
            try {
                connection.connect();
                Log.i(TAG, "http connected");
            } catch (java.io.IOException e) {
                Log.i(TAG, "http " + e.getClass().getName() + ": " + e.getMessage());
            }
        }));
        thread.start();
        try {
            thread.join();
        } catch (InterruptedException e) {
            Log.i(TAG, "interrupted");
        }
        Log.i(TAG, "done");
    }
}
