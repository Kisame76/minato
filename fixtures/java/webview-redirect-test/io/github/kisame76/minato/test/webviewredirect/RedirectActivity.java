package io.github.kisame76.minato.test.webviewredirect;

import android.app.Activity;
import android.graphics.Bitmap;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.webkit.JavascriptInterface;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.TreeSet;

/**
 * Redirects a page meets on the network, with two origins the app serves itself over http on 127.0.0.1 (A and B, two
 * ports), so that no network is needed. The app's WebViewClient overrides shouldInterceptRequest and answers nothing,
 * so every request goes to the network; it notes each URL it is asked for (a redirect's target is asked for again).
 * Round 1, a page of A fetches: a same-origin redirect (its hop sets a cookie), a cross-origin redirect without and
 * with CORS, an image through a cross-origin redirect drawn to a canvas (which then may not be read), a POST of
 * 100000 bytes and the same POST through a 307 redirect, and tells the app over addJavascriptInterface what it saw.
 * Rounds 2 and 3 load a main-frame redirect of A to A and of A to B: the page callbacks with their URLs, and
 * location.href. It ends with the URLs the client was asked for, the requests the server took (and whether each
 * carried a cookie), then "done". Origins and ports are written A and B; Chromium's favicon requests are left out.
 */
public class RedirectActivity extends Activity {
    private static final String TAG = "webview-redirect-test";
    private static final String PNG_BASE64 =
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==";

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private final Set<String> mAsked = new TreeSet<>();
    private final Set<String> mServed = new TreeSet<>();
    private ServerSocket mA, mB;
    private WebView mView;
    private int mRound;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private String origin(ServerSocket socket) {
        return "http://127.0.0.1:" + socket.getLocalPort();
    }

    /** A URL with the two origins written A and B. */
    private String named(String url) {
        if (url == null) {
            return "null";
        }
        return url.replace(origin(mA), "A").replace(origin(mB), "B");
    }

    public final class Bridge {
        @JavascriptInterface
        public void postMessage(String text) {
            mHandler.post(() -> {
                log("round 1 page: " + named(text));
                next();
            });
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        try {
            mA = new ServerSocket(0, 50, InetAddress.getByName("127.0.0.1"));
            mB = new ServerSocket(0, 50, InetAddress.getByName("127.0.0.1"));
        } catch (IOException e) {
            log("no server: " + e.getClass().getSimpleName());
            return;
        }
        serve(mA, "A");
        serve(mB, "B");
        mView = new WebView(this);
        setContentView(mView);
        mView.getSettings().setJavaScriptEnabled(true);
        mView.addJavascriptInterface(new Bridge(), "nativeBridge");
        mView.setWebViewClient(new WebViewClient() {
            @Override
            public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest request) {
                String url = request.getUrl().toString();
                url = url.contains("?") ? url.substring(0, url.indexOf('?')) : url;  // the page's query names a port
                if (!url.endsWith("/favicon.ico")) {
                    synchronized (mAsked) {
                        mAsked.add(named(url));
                    }
                }
                return null;
            }

            @Override
            public void onPageStarted(WebView view, String url, Bitmap favicon) {
                if (mRound > 1) {
                    log("round " + mRound + " started " + named(url));
                }
            }

            @Override
            public void onPageFinished(WebView view, String url) {
                if (mRound > 1 && url.endsWith("/page2")) {
                    log("round " + mRound + " finished " + named(url));
                    view.evaluateJavascript("location.href + ' ' + document.title", value -> {
                        log("round " + mRound + " location " + named(value));
                        next();
                    });
                }
            }
        });
        next();
    }

    private void next() {
        mRound++;
        switch (mRound) {
            case 1:
                mView.loadUrl(origin(mA) + "/page?b=" + mB.getLocalPort());
                break;
            case 2:
                mView.loadUrl(origin(mA) + "/nav-same");
                break;
            case 3:
                mView.loadUrl(origin(mA) + "/nav-cross");
                break;
            default:
                synchronized (mAsked) {
                    log("asked " + mAsked);
                }
                synchronized (mServed) {
                    log("served " + mServed);
                }
                log("done");
        }
    }

    /** One origin's server: each connection on a thread of its own, one request each. */
    private void serve(ServerSocket server, String name) {
        Thread thread = new Thread(() -> {
            while (true) {
                try {
                    Socket socket = server.accept();
                    new Thread(() -> answer(socket, name)).start();
                } catch (IOException e) {
                    return;
                }
            }
        });
        thread.setDaemon(true);
        thread.start();
    }

    private void answer(Socket socket, String name) {
        try (Socket closing = socket) {
            InputStream in = socket.getInputStream();
            BufferedReader reader = new BufferedReader(new InputStreamReader(in, StandardCharsets.ISO_8859_1));
            String first = reader.readLine();
            if (first == null) {
                return;
            }
            String[] parts = first.split(" ");
            String method = parts[0];
            String path = parts.length > 1 ? parts[1] : "/";
            Map<String, String> headers = new HashMap<>();
            for (String line; (line = reader.readLine()) != null && !line.isEmpty();) {
                int colon = line.indexOf(':');
                if (colon > 0) {
                    headers.put(line.substring(0, colon).trim().toLowerCase(Locale.ROOT),
                            line.substring(colon + 1).trim());
                }
            }
            int length = headers.containsKey("content-length") ? Integer.parseInt(headers.get("content-length")) : 0;
            int got = 0;
            for (; got < length && reader.read() >= 0; got++) {
                // the body's bytes, counted
            }
            String plain = path.contains("?") ? path.substring(0, path.indexOf('?')) : path;
            if (!plain.equals("/favicon.ico")) {
                synchronized (mServed) {
                    mServed.add(method + " " + name + plain + (headers.containsKey("cookie") ? " cookie" : ""));
                }
            }
            String a = origin(mA), b = origin(mB);
            switch (plain) {
                case "/page":
                    send(socket, 200, "text/html", PAGE.replace("ORIGIN_B", b), null);
                    break;
                case "/page2":
                    send(socket, 200, "text/html", "<html><head><title>page2 " + name + "</title></head></html>", null);
                    break;
                case "/r-same":
                    send(socket, 302, "text/plain", "", "Location: " + a + "/data\r\nSet-Cookie: hop=1; Path=/\r\n");
                    break;
                case "/r-cross":
                    send(socket, 302, "text/plain", "", "Location: " + b + "/data\r\n");
                    break;
                case "/r-cross-cors":
                    send(socket, 302, "text/plain", "", "Location: " + b + "/cors\r\n");
                    break;
                case "/r-img":
                    send(socket, 302, "text/plain", "", "Location: " + b + "/img.png\r\n");
                    break;
                case "/r-post":
                    send(socket, 307, "text/plain", "", "Location: " + a + "/echo\r\n");
                    break;
                case "/nav-same":
                    send(socket, 302, "text/plain", "", "Location: " + a + "/page2\r\n");
                    break;
                case "/nav-cross":
                    send(socket, 302, "text/plain", "", "Location: " + b + "/page2\r\n");
                    break;
                case "/data":
                    send(socket, 200, "text/plain", "data " + name, null);
                    break;
                case "/cors":
                    send(socket, 200, "text/plain", "cors " + name, "Access-Control-Allow-Origin: *\r\n");
                    break;
                case "/echo":
                    send(socket, 200, "text/plain", method + " " + got + " bytes", null);
                    break;
                case "/img.png":
                    byte[] png = android.util.Base64.decode(PNG_BASE64, android.util.Base64.DEFAULT);
                    sendBytes(socket, 200, "image/png", png, null);
                    break;
                default:
                    send(socket, 404, "text/plain", "", null);
            }
        } catch (IOException | RuntimeException e) {
            // the client went away
        }
    }

    private static void send(Socket socket, int status, String type, String body, String extra) throws IOException {
        sendBytes(socket, status, type, body.getBytes(StandardCharsets.UTF_8), extra);
    }

    private static void sendBytes(Socket socket, int status, String type, byte[] body, String extra)
            throws IOException {
        String reason = status == 200 ? "OK" : status == 302 ? "Found" : status == 307 ? "Temporary Redirect"
                : "Not Found";
        String head = "HTTP/1.1 " + status + " " + reason + "\r\nContent-Type: " + type + "\r\nContent-Length: "
                + body.length + "\r\nConnection: close\r\n" + (extra != null ? extra : "") + "\r\n";
        OutputStream out = socket.getOutputStream();
        out.write(head.getBytes(StandardCharsets.ISO_8859_1));
        out.write(body);
        out.flush();
    }

    private static final String PAGE = "<!DOCTYPE html><html><head><title>page A</title></head><body><script>\n"
            + "const named = (u) => u.split(location.origin).join('A').split('ORIGIN_B').join('B');\n"
            + "const out = [];\n"
            + "async function t(name, url, options) {\n"
            + "  try { const r = await fetch(url, options); out.push(name + ' ' + r.status + ' ' + (await r.text())"
            + " + ' at ' + named(r.url)); }\n"
            + "  catch (e) { out.push(name + ' ' + e.name); }\n"
            + "}\n"
            + "(async () => {\n"
            + "  await t('same', '/r-same');\n"
            + "  await t('cross', '/r-cross');\n"
            + "  await t('cross-cors', '/r-cross-cors');\n"
            + "  await new Promise((done) => {\n"
            + "    const image = new Image();\n"
            + "    image.onload = () => {\n"
            + "      const canvas = document.createElement('canvas'); canvas.width = 1; canvas.height = 1;\n"
            + "      canvas.getContext('2d').drawImage(image, 0, 0);\n"
            + "      try { canvas.toDataURL(); out.push('canvas readable'); }\n"
            + "      catch (e) { out.push('canvas ' + e.name); }\n"
            + "      done();\n"
            + "    };\n"
            + "    image.onerror = () => { out.push('image error'); done(); };\n"
            + "    image.src = '/r-img';\n"
            + "  });\n"
            + "  const body = 'x'.repeat(100000);\n"
            + "  await t('post', '/echo', {method: 'POST', body});\n"
            + "  await t('post-307', '/r-post', {method: 'POST', body});\n"
            + "  out.push('cookie ' + (document.cookie || '-'));\n"
            + "  nativeBridge.postMessage(out.join(', '));\n"
            + "})();\n"
            + "</script></body></html>";
}
