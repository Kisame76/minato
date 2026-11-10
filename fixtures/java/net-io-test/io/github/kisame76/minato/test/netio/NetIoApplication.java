package io.github.kisame76.minato.test.netio;

import android.app.Application;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.HttpURLConnection;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.URL;
import java.nio.charset.StandardCharsets;

/**
 * What an app does with sockets and plain HTTP on the device itself: TCP over the loopback (an echo, the end of a
 * stream, a read time-out, a refused connection, the socket options an app sets), UDP over the loopback, name lookups
 * that need no server, and HttpURLConnection against a server of the app's own (a GET, a POST, a 404 with its error
 * stream). The sections run on a thread of their own, as the main thread may not use the network (the first line).
 * Nothing leaves the device, so the log does not depend on the network. Each section logs its lines on its own, so
 * that one that fails does not hide the others; a Throwable shows as its class name.
 */
public class NetIoApplication extends Application {
    private static final String TAG = "net-io-test";

    @Override
    public void onCreate() {
        super.onCreate();
        mainThread();
        final Thread worker = new Thread(() -> {
            tcp();
            udp();
            dns();
            http();
        });
        worker.start();
        try {
            worker.join(60000);
        } catch (InterruptedException e) {
            Log.i(TAG, "interrupted");
        }
        Log.i(TAG, "done");
    }

    /** The main thread may not use the network (StrictMode's default for a target SDK of 11 or more). */
    private void mainThread() {
        try (Socket socket = new Socket()) {
            socket.connect(new InetSocketAddress("127.0.0.1", 9), 200);
            Log.i(TAG, "main thread connect: went through");
        } catch (Throwable t) {
            failed("main thread connect", t);
        }
    }

    private static void failed(String section, Throwable t) {
        Log.i(TAG, section + ": " + t.getClass().getName());
    }

    private static String readLine(InputStream in) throws Exception {
        final ByteArrayOutputStream line = new ByteArrayOutputStream();
        for (int c = in.read(); c >= 0 && c != '\n'; c = in.read()) {
            if (c != '\r') line.write(c);
        }
        return line.toString("UTF-8");
    }

    private void tcp() {
        try (ServerSocket server = new ServerSocket(0, 1, InetAddress.getByName("127.0.0.1"))) {
            final String[] heard = new String[2];
            final Thread peer = new Thread(() -> {
                try (Socket client = server.accept()) {
                    heard[0] = readLine(client.getInputStream());
                    client.getOutputStream().write("pong\n".getBytes(StandardCharsets.UTF_8));
                    client.getOutputStream().flush();
                    heard[1] = String.valueOf(client.getInputStream().read());
                } catch (Throwable t) {
                    heard[0] = t.getClass().getName();
                }
            });
            peer.start();
            try (Socket socket = new Socket()) {
                socket.connect(new InetSocketAddress("127.0.0.1", server.getLocalPort()), 5000);
                Log.i(TAG, "tcp connected: " + socket.isConnected() + " loopback "
                        + socket.getInetAddress().isLoopbackAddress() + " local loopback "
                        + socket.getLocalAddress().isLoopbackAddress());
                socket.setTcpNoDelay(true);
                socket.setKeepAlive(true);
                socket.setSoLinger(true, 3);
                Log.i(TAG, "tcp options: no delay " + socket.getTcpNoDelay() + ", keep alive " + socket.getKeepAlive()
                        + ", linger " + socket.getSoLinger() + ", buffers positive "
                        + (socket.getReceiveBufferSize() > 0 && socket.getSendBufferSize() > 0));
                socket.getOutputStream().write("ping\n".getBytes(StandardCharsets.UTF_8));
                socket.getOutputStream().flush();
                Log.i(TAG, "tcp reply: " + readLine(socket.getInputStream()));
                socket.shutdownOutput();
                peer.join(5000);
                Log.i(TAG, "tcp server heard: " + heard[0] + ", then " + heard[1]);
                Log.i(TAG, "tcp output shut: " + socket.isOutputShutdown());
                socket.setSoTimeout(200);
                try {
                    Log.i(TAG, "tcp read after the server closed: " + socket.getInputStream().read());
                } catch (Throwable t) {
                    failed("tcp read after the server closed", t);
                }
            }
            final int port = server.getLocalPort();
            server.close();
            try (Socket refused = new Socket()) {
                refused.connect(new InetSocketAddress("127.0.0.1", port), 2000);
                Log.i(TAG, "tcp refused: connected");
            } catch (Throwable t) {
                failed("tcp refused", t);
            }
        } catch (Throwable t) {
            failed("tcp", t);
        }
    }

    private void udp() {
        try (DatagramSocket server = new DatagramSocket(0, InetAddress.getByName("127.0.0.1"));
                DatagramSocket client = new DatagramSocket()) {
            final byte[] data = "dgram".getBytes(StandardCharsets.UTF_8);
            client.send(new DatagramPacket(data, data.length, InetAddress.getByName("127.0.0.1"),
                    server.getLocalPort()));
            server.setSoTimeout(5000);
            final DatagramPacket in = new DatagramPacket(new byte[64], 64);
            server.receive(in);
            Log.i(TAG, "udp received: " + new String(in.getData(), 0, in.getLength(), StandardCharsets.UTF_8)
                    + " from loopback " + in.getAddress().isLoopbackAddress());
            server.setSoTimeout(200);
            try {
                server.receive(in);
                Log.i(TAG, "udp second receive: a datagram");
            } catch (Throwable t) {
                failed("udp second receive", t);
            }
        } catch (Throwable t) {
            failed("udp", t);
        }
    }

    private void dns() {
        try {
            final InetAddress local = InetAddress.getByName("localhost");
            Log.i(TAG, "dns localhost: loopback " + local.isLoopbackAddress() + ", all "
                    + (InetAddress.getAllByName("localhost").length > 0));
            Log.i(TAG, "dns literal v4: " + InetAddress.getByName("192.0.2.7").getHostAddress());
            Log.i(TAG, "dns literal v6: " + InetAddress.getByName("2001:db8::1").getHostAddress());
            Log.i(TAG, "dns loopback v4: " + InetAddress.getByName("127.0.0.1").isLoopbackAddress());
            Log.i(TAG, "dns local host name not empty: " + !InetAddress.getLocalHost().getHostName().isEmpty());
        } catch (Throwable t) {
            failed("dns", t);
        }
        try {
            InetAddress.getByName("no-such-host.invalid");
            Log.i(TAG, "dns invalid: resolved");
        } catch (Throwable t) {
            failed("dns invalid", t);
        }
    }

    private void http() {
        try (ServerSocket server = new ServerSocket(0, 4, InetAddress.getByName("127.0.0.1"))) {
            final Thread serving = new Thread(() -> {
                for (int i = 0; i < 3; i++) {
                    try (Socket client = server.accept()) {
                        final InputStream in = client.getInputStream();
                        final String request = readLine(in);
                        int length = 0;
                        for (String header = readLine(in); !header.isEmpty(); header = readLine(in)) {
                            if (header.toLowerCase().startsWith("content-length:")) {
                                length = Integer.parseInt(header.substring(15).trim());
                            }
                        }
                        final byte[] body = new byte[length];
                        for (int got = 0; got < length; ) {
                            final int n = in.read(body, got, length - got);
                            if (n < 0) break;
                            got += n;
                        }
                        final boolean post = request.startsWith("POST ");
                        final boolean missing = request.contains("/missing");
                        final String text = post ? "echo:" + new String(body, StandardCharsets.UTF_8)
                                : missing ? "gone" : "hello";
                        final byte[] out = text.getBytes(StandardCharsets.UTF_8);
                        final String head = "HTTP/1.1 " + (missing ? "404 Not Found" : "200 OK")
                                + "\r\nX-Test: yes\r\nContent-Type: text/plain\r\nContent-Length: " + out.length
                                + "\r\nConnection: close\r\n\r\n";
                        final OutputStream stream = client.getOutputStream();
                        stream.write(head.getBytes(StandardCharsets.UTF_8));
                        stream.write(out);
                        stream.flush();
                    } catch (Throwable ignored) {
                        break;
                    }
                }
            });
            serving.start();
            final String base = "http://127.0.0.1:" + server.getLocalPort();
            HttpURLConnection get = (HttpURLConnection) new URL(base + "/hello").openConnection();
            get.setConnectTimeout(5000);
            get.setReadTimeout(5000);
            Log.i(TAG, "http get: " + get.getResponseCode() + " " + get.getResponseMessage() + ", X-Test "
                    + get.getHeaderField("X-Test") + ", type " + get.getContentType() + ", length "
                    + get.getContentLength() + ", body " + slurp(get.getInputStream()));
            get.disconnect();
            HttpURLConnection post = (HttpURLConnection) new URL(base + "/post").openConnection();
            post.setConnectTimeout(5000);
            post.setReadTimeout(5000);
            post.setDoOutput(true);
            post.setRequestMethod("POST");
            post.getOutputStream().write("name=x".getBytes(StandardCharsets.UTF_8));
            Log.i(TAG, "http post: " + post.getResponseCode() + ", body " + slurp(post.getInputStream()));
            post.disconnect();
            HttpURLConnection missing = (HttpURLConnection) new URL(base + "/missing").openConnection();
            missing.setConnectTimeout(5000);
            missing.setReadTimeout(5000);
            Log.i(TAG, "http missing: " + missing.getResponseCode() + " " + missing.getResponseMessage());
            try {
                missing.getInputStream();
                Log.i(TAG, "http missing input stream: opened");
            } catch (Throwable t) {
                failed("http missing input stream", t);
            }
            Log.i(TAG, "http missing error stream: " + slurp(missing.getErrorStream()));
            missing.disconnect();
            serving.join(5000);
        } catch (Throwable t) {
            failed("http", t);
        }
    }

    private static String slurp(InputStream in) throws Exception {
        final ByteArrayOutputStream out = new ByteArrayOutputStream();
        final byte[] buffer = new byte[256];
        for (int n = in.read(buffer); n >= 0; n = in.read(buffer)) out.write(buffer, 0, n);
        in.close();
        return out.toString("UTF-8");
    }
}
