/*
 * Minato: a compile-time stand-in (ADR-0033 a) for external/okhttp's
 * com.android.okhttp.internalandroidapi.HttpURLConnectionFactory, which libcore's libcore.net.http's factory of the
 * same name wraps; see Dns.java. Only the signatures matter: the boot jars refer to okhttp's methods by them.
 */
package com.android.okhttp.internalandroidapi;

import java.io.IOException;
import java.net.Proxy;
import java.net.URL;
import java.net.URLConnection;
import java.util.concurrent.TimeUnit;
import javax.net.SocketFactory;

public final class HttpURLConnectionFactory {
    public void setNewConnectionPool(int maxIdleConnections, long keepAliveDuration, TimeUnit timeUnit) {
        throw new UnsupportedOperationException();
    }

    public void setDns(Dns dns) {
        throw new UnsupportedOperationException();
    }

    public URLConnection openConnection(URL url, SocketFactory socketFactory, Proxy proxy) throws IOException {
        throw new UnsupportedOperationException();
    }
}
