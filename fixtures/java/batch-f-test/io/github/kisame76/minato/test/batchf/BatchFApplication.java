package io.github.kisame76.minato.test.batchf;

import android.app.Application;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.net.Proxy;
import android.net.Uri;
import android.os.Process;
import android.telephony.PhoneNumberUtils;
import android.text.format.Formatter;
import android.util.Log;
import android.widget.SearchView;
import android.widget.VideoView;

import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

/**
 * The answers an app reads from the framework breadth's batch F: android.text.format.Formatter,
 * android.widget.SearchView, android.net.Proxy, android.telephony.PhoneNumberUtils, android.widget.VideoView, and
 * android.net.LocalSocket with LocalServerSocket. Each section logs its lines on its own, so that one that fails does not hide the others; a
 * Throwable shows as its class name. A string shows its non-ASCII characters as \\uXXXX.
 */
public class BatchFApplication extends Application {
    private static final String TAG = "batch-f-test";

    @Override
    public void onCreate() {
        super.onCreate();
        formatter();
        searchView();
        proxy();
        phoneNumberUtils();
        videoView();
        localSocket();
        Log.i(TAG, "done");
    }

    private static String show(CharSequence text) {
        if (text == null) return "null";
        final StringBuilder out = new StringBuilder();
        for (int i = 0; i < text.length(); i++) {
            final char c = text.charAt(i);
            if (c < 32 || c > 126) out.append(String.format("\\u%04x", (int) c)); else out.append(c);
        }
        return out.toString();
    }

    private static void failed(String section, Throwable t) {
        Log.i(TAG, section + ": " + t.getClass().getName());
    }

    private void formatter() {
        try {
            final long[] sizes = {0, 999, 1000, 1500, 1234567, 5368709120L};
            for (final long size : sizes) {
                Log.i(TAG, "formatter size " + size + ": " + show(Formatter.formatFileSize(this, size)) + " | "
                        + show(Formatter.formatShortFileSize(this, size)));
            }
            Log.i(TAG, "formatter ip: " + Formatter.formatIpAddress(0x0100007f) + " "
                    + Formatter.formatIpAddress(0x0100a8c0));
        } catch (Throwable t) {
            failed("formatter", t);
        }
    }

    private void searchView() {
        try {
            final SearchView view = new SearchView(this);
            final StringBuilder events = new StringBuilder();
            view.setOnQueryTextListener(new SearchView.OnQueryTextListener() {
                @Override
                public boolean onQueryTextSubmit(String query) {
                    events.append("submit ").append(query).append(' ');
                    return false;
                }

                @Override
                public boolean onQueryTextChange(String newText) {
                    events.append("change ").append(newText).append(' ');
                    return false;
                }
            });
            Log.i(TAG, "searchview start: iconified " + view.isIconified() + " query " + show(view.getQuery())
                    + " hint " + show(view.getQueryHint()) + " submit button " + view.isSubmitButtonEnabled()
                    + " refinement " + view.isQueryRefinementEnabled() + " max width " + view.getMaxWidth());
            view.setIconified(false);
            view.setQueryHint("Find");
            view.setSubmitButtonEnabled(true);
            view.setQueryRefinementEnabled(true);
            view.setMaxWidth(500);
            view.setQuery("abc", false);
            view.setQuery("xyz", true);
            Log.i(TAG, "searchview after: iconified " + view.isIconified() + " query " + show(view.getQuery())
                    + " hint " + show(view.getQueryHint()) + " submit button " + view.isSubmitButtonEnabled()
                    + " refinement " + view.isQueryRefinementEnabled() + " max width " + view.getMaxWidth());
            Log.i(TAG, "searchview events: " + events.toString().trim());
            view.setIconified(true);
            Log.i(TAG, "searchview iconified again: " + view.isIconified() + " query " + show(view.getQuery()));
        } catch (Throwable t) {
            failed("searchview", t);
        }
    }

    private void proxy() {
        try {
            Log.i(TAG, "proxy constants: " + Proxy.PROXY_CHANGE_ACTION);
            Log.i(TAG, "proxy none: " + Proxy.getDefaultHost() + " " + Proxy.getDefaultPort());
            System.setProperty("http.proxyHost", "proxy.example.com");
            System.setProperty("http.proxyPort", "8080");
            Log.i(TAG, "proxy set: " + Proxy.getDefaultHost() + " " + Proxy.getDefaultPort() + " "
                    + Proxy.getHost(this) + " " + Proxy.getPort(this));
            final java.net.Proxy viaProxy = Proxy.getProxy(this, "http://example.com/x");
            Log.i(TAG, "proxy get: " + viaProxy.type() + " "
                    + ((java.net.InetSocketAddress) viaProxy.address()).getPort() + " local "
                    + Proxy.getProxy(this, "http://localhost/x").type() + " "
                    + Proxy.getProxy(this, "http://127.0.0.1/x").type());
            System.setProperty("http.proxyPort", "x");
            Log.i(TAG, "proxy bad port: " + Proxy.getDefaultHost() + " " + Proxy.getDefaultPort());
            System.clearProperty("http.proxyHost");
            System.clearProperty("http.proxyPort");
            Log.i(TAG, "proxy cleared: " + Proxy.getDefaultHost() + " " + Proxy.getDefaultPort());
        } catch (Throwable t) {
            failed("proxy", t);
        }
    }

    private void phoneNumberUtils() {
        try {
            Log.i(TAG, "phone strip: " + PhoneNumberUtils.stripSeparators("(415) 555-2671 ext.3") + " "
                    + PhoneNumberUtils.stripSeparators("+1 415-555-2671"));
            Log.i(TAG, "phone keypad: " + PhoneNumberUtils.convertKeypadLettersToDigits("1-800-FLOWERS"));
            Log.i(TAG, "phone global: " + PhoneNumberUtils.isGlobalPhoneNumber("+14155552671") + " "
                    + PhoneNumberUtils.isGlobalPhoneNumber("abc") + " "
                    + PhoneNumberUtils.isGlobalPhoneNumber("*123#"));
            Log.i(TAG, "phone network portion: " + PhoneNumberUtils.extractNetworkPortion("+1 (415) 555-2671,123")
                    + " postdial " + PhoneNumberUtils.extractPostDialPortion("+1 (415) 555-2671,123;45"));
            Log.i(TAG, "phone compare: " + PhoneNumberUtils.compare("5552671", "+1 415 555 2671") + " "
                    + PhoneNumberUtils.compare("5552671", "5552672"));
            Log.i(TAG, "phone tel: " + PhoneNumberUtils.isNonSeparator('5') + " " + PhoneNumberUtils.isNonSeparator('-')
                    + " " + PhoneNumberUtils.is12Key('#') + " " + PhoneNumberUtils.isReallyDialable('+')
                    + " " + PhoneNumberUtils.isStartsPostDial(','));
            Log.i(TAG, "phone toa: " + PhoneNumberUtils.toaFromString("+14155552671") + " "
                    + PhoneNumberUtils.toaFromString("4155552671"));
            Log.i(TAG, "phone minmatch: " + PhoneNumberUtils.toCallerIDMinMatch("+1 (415) 555-2671"));
        } catch (Throwable t) {
            failed("phone", t);
        }
    }

    private void videoView() {
        try {
            final VideoView view = new VideoView(this);
            Log.i(TAG, "videoview start: playing " + view.isPlaying() + " position " + view.getCurrentPosition()
                    + " duration " + view.getDuration() + " buffer " + view.getBufferPercentage()
                    + " pause " + view.canPause() + " back " + view.canSeekBackward() + " forward "
                    + view.canSeekForward());
            view.setVideoURI(Uri.parse("file:///no/such/video.mp4"));
            view.setVideoPath("/no/such/video.mp4");
            Log.i(TAG, "videoview after uri: playing " + view.isPlaying() + " position " + view.getCurrentPosition()
                    + " duration " + view.getDuration());
            view.seekTo(1000);
            view.start();
            view.pause();
            view.stopPlayback();
            Log.i(TAG, "videoview after stop: playing " + view.isPlaying() + " position " + view.getCurrentPosition());
        } catch (Throwable t) {
            failed("videoview", t);
        }
    }

    /** LocalSocket.isClosed is AOSP's: it throws UnsupportedOperationException on every state. */
    private static String closed(LocalSocket socket) {
        try {
            return String.valueOf(socket.isClosed());
        } catch (UnsupportedOperationException e) {
            return "UnsupportedOperationException";
        }
    }

    private void localSocket() {
        try {
            final String name = "batch-f-test-" + Process.myPid();
            final LocalSocketAddress address = new LocalSocketAddress(name);
            Log.i(TAG, "localsocket address: " + address.getName().startsWith("batch-f-test-") + " "
                    + address.getNamespace() + " "
                    + new LocalSocketAddress("x", LocalSocketAddress.Namespace.FILESYSTEM).getNamespace());
            final LocalServerSocket server = new LocalServerSocket(name);
            Log.i(TAG, "localsocket server: " + server.getLocalSocketAddress().getName().equals(name) + " "
                    + (server.getFileDescriptor() != null));
            final String[] heard = new String[1];
            final Thread accepter = new Thread(() -> {
                try {
                    final LocalSocket peer = server.accept();
                    final byte[] buffer = new byte[16];
                    final int count = peer.getInputStream().read(buffer);
                    heard[0] = new String(buffer, 0, count, StandardCharsets.UTF_8);
                    final OutputStream out = peer.getOutputStream();
                    out.write("pong".getBytes(StandardCharsets.UTF_8));
                    out.flush();
                    heard[0] += " uid " + (peer.getPeerCredentials().getUid() == Process.myUid());
                    peer.close();
                } catch (Throwable t) {
                    heard[0] = t.getClass().getName();
                }
            });
            accepter.start();
            final LocalSocket client = new LocalSocket();
            Log.i(TAG, "localsocket before: connected " + client.isConnected() + " bound " + client.isBound()
                    + " closed " + closed(client));
            client.connect(address);
            final OutputStream out = client.getOutputStream();
            out.write("ping".getBytes(StandardCharsets.UTF_8));
            out.flush();
            final InputStream in = client.getInputStream();
            final byte[] reply = new byte[16];
            final int count = in.read(reply);
            Log.i(TAG, "localsocket reply: " + new String(reply, 0, count, StandardCharsets.UTF_8) + " connected "
                    + client.isConnected() + " peer uid " + (client.getPeerCredentials().getUid() == Process.myUid()));
            Log.i(TAG, "localsocket end: " + in.read(reply));
            accepter.join(5000);
            Log.i(TAG, "localsocket server heard: " + heard[0]);
            client.close();
            server.close();
            Log.i(TAG, "localsocket closed: " + closed(client));
            try {
                new LocalSocket().connect(new LocalSocketAddress("batch-f-test-nobody-" + Process.myPid()));
                Log.i(TAG, "localsocket missing: connected");
            } catch (java.io.IOException e) {
                Log.i(TAG, "localsocket missing: IOException");
            }
        } catch (Throwable t) {
            failed("localsocket", t);
        }
    }
}
