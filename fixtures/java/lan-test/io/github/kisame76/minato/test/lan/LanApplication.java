package io.github.kisame76.minato.test.lan;

import android.app.Application;
import android.util.Log;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.HttpURLConnection;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.net.URL;
import java.nio.charset.StandardCharsets;

/**
 * An app talking to another device on the same network: a TCP echo, an HTTP request and a UDP echo against
 * tools/lan-server.py on the machine next to it, and a connection to an address nobody answers (192.0.2.1, reserved for
 * documentation) as the control, which must end in an exception and not in a hang. The server's address is the
 * environment entry MINATO_LAN_HOST (10.0.2.2, the emulator's name for its host, without it); it is never logged. Each
 * line says "ok" or the exception class. The work runs on a thread of its own, as the main thread may not use the
 * network.
 */
public class LanApplication extends Application {
    private static final String TAG = "lan-test";
    private static final int TCP = 47081;
    private static final int HTTP = 47082;
    private static final int UDP = 47083;

    @Override
    public void onCreate() {
        super.onCreate();
        final Thread worker = new Thread(this::run);
        worker.start();
        try {
            worker.join(60000);
        } catch (InterruptedException e) {
            Log.i(TAG, "interrupted");
        }
        Log.i(TAG, "done");
    }

    private static void failed(String what, Throwable t) {
        Log.i(TAG, what + ": " + t.getClass().getName());
    }

    private void run() {
        String host = System.getenv("MINATO_LAN_HOST");
        if (host == null || host.isEmpty()) host = "10.0.2.2";
        tcp(host);
        http(host);
        udp(host);
        control();
        bye(host);
    }

    private void tcp(String host) {
        try (Socket socket = new Socket()) {
            socket.connect(new InetSocketAddress(host, TCP), 5000);
            socket.setSoTimeout(5000);
            socket.getOutputStream().write("ping\n".getBytes(StandardCharsets.UTF_8));
            socket.getOutputStream().flush();
            final BufferedReader in = new BufferedReader(new InputStreamReader(socket.getInputStream(), "UTF-8"));
            Log.i(TAG, "tcp echo: " + in.readLine());
        } catch (Throwable t) {
            failed("tcp echo", t);
        }
    }

    private void http(String host) {
        try {
            final HttpURLConnection connection = (HttpURLConnection) new URL("http://" + host + ":" + HTTP + "/")
                    .openConnection();
            connection.setConnectTimeout(5000);
            connection.setReadTimeout(5000);
            final BufferedReader in = new BufferedReader(new InputStreamReader(connection.getInputStream(), "UTF-8"));
            Log.i(TAG, "http get: " + connection.getResponseCode() + " " + in.readLine());
            connection.disconnect();
        } catch (Throwable t) {
            failed("http get", t);
        }
    }

    private void udp(String host) {
        try (DatagramSocket socket = new DatagramSocket()) {
            socket.setSoTimeout(3000);
            final byte[] data = "dgram".getBytes(StandardCharsets.UTF_8);
            socket.send(new DatagramPacket(data, data.length, InetAddress.getByName(host), UDP));
            final DatagramPacket reply = new DatagramPacket(new byte[64], 64);
            socket.receive(reply);
            Log.i(TAG, "udp echo: " + new String(reply.getData(), 0, reply.getLength(), StandardCharsets.UTF_8));
        } catch (Throwable t) {
            failed("udp echo", t);
        }
    }

    /** An address nobody answers: the connection ends in an exception after the time-out, it does not hang. */
    private void control() {
        try (Socket socket = new Socket()) {
            socket.connect(new InetSocketAddress("192.0.2.1", 9), 1500);
            Log.i(TAG, "control: connected");
        } catch (Throwable t) {
            Log.i(TAG, "control ended with an exception: " + (t instanceof java.io.IOException));
        }
    }

    private void bye(String host) {
        try (Socket socket = new Socket()) {
            socket.connect(new InetSocketAddress(host, TCP), 3000);
            socket.getOutputStream().write("bye\n".getBytes(StandardCharsets.UTF_8));
            socket.getOutputStream().flush();
        } catch (Throwable t) {
            // The server also ends on its own after its time.
        }
    }
}
