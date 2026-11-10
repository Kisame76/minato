package io.github.kisame76.minato.test.netoptionlate;

import android.app.Application;
import android.util.Log;

import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;

/**
 * Socket options set after both ends have shut down their sending side. The server accepts and closes at once; the
 * client reads the end of the stream (-1), shuts its own output down, and only then sets TCP_NODELAY, SO_KEEPALIVE and
 * SO_LINGER and reads them back. On Android the options stay as set (true, true, 3); a host that refuses a setsockopt
 * on such a socket shows false, false and -1 here. The work runs on a thread of its own, as the main thread may not use
 * the network.
 */
public class NetOptionLateApplication extends Application {
    private static final String TAG = "net-option-late-test";

    @Override
    public void onCreate() {
        super.onCreate();
        final Thread worker = new Thread(this::run);
        worker.start();
        try {
            worker.join(30000);
        } catch (InterruptedException e) {
            Log.i(TAG, "interrupted");
        }
        Log.i(TAG, "done");
    }

    private void run() {
        try (ServerSocket server = new ServerSocket(0, 1, InetAddress.getByName("127.0.0.1"))) {
            final Thread peer = new Thread(() -> {
                try {
                    server.accept().close();
                } catch (Throwable t) {
                    Log.i(TAG, "peer: " + t.getClass().getName());
                }
            });
            peer.start();
            try (Socket socket = new Socket()) {
                socket.connect(new InetSocketAddress("127.0.0.1", server.getLocalPort()), 5000);
                Log.i(TAG, "end of the stream: " + socket.getInputStream().read());
                socket.shutdownOutput();
                socket.setTcpNoDelay(true);
                socket.setKeepAlive(true);
                socket.setSoLinger(true, 3);
                Log.i(TAG, "options after both ends shut their output: no delay "
                        + socket.getTcpNoDelay() + ", keep alive " + socket.getKeepAlive() + ", linger "
                        + socket.getSoLinger());
            }
            peer.join(5000);
        } catch (Throwable t) {
            Log.i(TAG, "failed: " + t.getClass().getName());
        }
    }
}
