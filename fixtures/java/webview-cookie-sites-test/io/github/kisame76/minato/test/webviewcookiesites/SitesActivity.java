package io.github.kisame76.minato.test.webviewcookiesites;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.webkit.CookieManager;
import android.webkit.JavascriptInterface;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.TreeSet;

/**
 * Third-party cookies, from two sites the app serves itself over http on its loopback: A (http://127.0.0.1:port) and
 * B (http://localhost:port), which are other sites. The app's WebViewClient overrides shouldInterceptRequest and
 * answers nothing, so every request goes to the network. Each round clears the cookies, visits B as the main frame
 * (B sets a cookie, and one with SameSite=None; Secure, which localhost may carry over http), loads a page of A with
 * an image of B (whose answer sets two more, likewise), then visits B again; the server notes the cookies each
 * request carried, by name. Round 1 with the WebView's default (third-party cookies
 * off from target SDK 21), round 2 after CookieManager.setAcceptThirdPartyCookies(view, true). It ends with "done".
 */
public class SitesActivity extends Activity {
    private static final String TAG = "webview-cookie-sites-test";
    private static final String PNG_BASE64 =
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==";

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private final Set<String> mServed = new TreeSet<>();
    private ServerSocket mA, mB;
    private WebView mView;
    private volatile int mRound;
    private int mStep;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private String a() {
        return "http://127.0.0.1:" + mA.getLocalPort();
    }

    private String b() {
        return "http://localhost:" + mB.getLocalPort();
    }

    public final class Bridge {
        @JavascriptInterface
        public void postMessage(String text) {
            mHandler.post(() -> {
                log("round " + mRound + " page: " + text);
                step();
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
        round();
    }

    /** A round: a WebView of its own, the cookies cleared, then the steps. */
    private void round() {
        mRound++;
        if (mRound == 3) {
            synchronized (mServed) {
                log("served " + mServed);
            }
            log("done");
            return;
        }
        mView = new WebView(this);
        setContentView(mView);
        mView.getSettings().setJavaScriptEnabled(true);
        mView.setWebViewClient(new WebViewClient() {
            @Override
            public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest request) {
                return null;
            }

            @Override
            public void onPageFinished(WebView view, String url) {
                if (url.contains("/visit")) {
                    step();
                }
            }
        });
        mView.addJavascriptInterface(new Bridge(), "nativeBridge");
        CookieManager cookies = CookieManager.getInstance();
        if (mRound == 2) {
            cookies.setAcceptThirdPartyCookies(mView, true);
        }
        log("round " + mRound + " third-party cookies " + cookies.acceptThirdPartyCookies(mView));
        cookies.removeAllCookies(removed -> {
            mStep = 0;
            step();
        });
    }

    private void step() {
        mStep++;
        switch (mStep) {
            case 1:
                mView.loadUrl(b() + "/visit?first");
                break;
            case 2:
                mView.loadUrl(a() + "/page");
                break;
            case 3:
                mView.loadUrl(b() + "/visit?again");
                break;
            default:
                String cookie = CookieManager.getInstance().getCookie(b());
                log("round " + mRound + " cookies of B: " + (cookie == null ? "-" : names(cookie)));
                mView.destroy();
                round();
        }
    }

    /** The names of a Cookie header's cookies, sorted. */
    private static String names(String header) {
        List<String> names = new ArrayList<>();
        for (String part : header.split(";")) {
            String name = part.trim();
            if (!name.isEmpty()) {
                names.add(name.contains("=") ? name.substring(0, name.indexOf('=')) : name);
            }
        }
        Collections.sort(names);
        return String.join(",", names);
    }

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
            BufferedReader reader = new BufferedReader(new InputStreamReader(socket.getInputStream(),
                    StandardCharsets.ISO_8859_1));
            String first = reader.readLine();
            if (first == null) {
                return;
            }
            String path = first.split(" ").length > 1 ? first.split(" ")[1] : "/";
            String cookie = null;
            for (String line; (line = reader.readLine()) != null && !line.isEmpty();) {
                if (line.toLowerCase(Locale.ROOT).startsWith("cookie:")) {
                    cookie = line.substring("cookie:".length()).trim();
                }
            }
            if (path.startsWith("/favicon.ico")) {
                send(socket, 404, "text/plain", new byte[0], "");
                return;
            }
            synchronized (mServed) {
                mServed.add("round " + mRound + " " + name + path + " cookies "
                        + (cookie == null ? "-" : names(cookie)));
            }
            switch (path.contains("?") ? path.substring(0, path.indexOf('?')) : path) {
                case "/visit":
                    send(socket, 200, "text/html", "<html><head><title>visit</title></head></html>"
                            .getBytes(StandardCharsets.UTF_8), "Set-Cookie: first=1; Path=/\r\n"
                            + "Set-Cookie: none=1; Path=/; SameSite=None; Secure\r\n");
                    break;
                case "/page":
                    send(socket, 200, "text/html", ("<html><body><script>const image = new Image();"
                            + "image.onload = () => nativeBridge.postMessage('image loaded');"
                            + "image.onerror = () => nativeBridge.postMessage('image error');"
                            + "image.src = '" + b() + "/img.png';</script></body></html>")
                            .getBytes(StandardCharsets.UTF_8), "");
                    break;
                case "/img.png":
                    send(socket, 200, "image/png", android.util.Base64.decode(PNG_BASE64, android.util.Base64.DEFAULT),
                            "Set-Cookie: third=1; Path=/\r\n"
                            + "Set-Cookie: thirdnone=1; Path=/; SameSite=None; Secure\r\n");
                    break;
                default:
                    send(socket, 404, "text/plain", new byte[0], "");
            }
        } catch (IOException | RuntimeException e) {
            // the client went away
        }
    }

    private static void send(Socket socket, int status, String type, byte[] body, String extra) throws IOException {
        String head = "HTTP/1.1 " + status + (status == 200 ? " OK" : " Not Found") + "\r\nContent-Type: " + type
                + "\r\nContent-Length: " + body.length + "\r\nConnection: close\r\n" + extra + "\r\n";
        OutputStream out = socket.getOutputStream();
        out.write(head.getBytes(StandardCharsets.ISO_8859_1));
        out.write(body);
        out.flush();
    }
}
