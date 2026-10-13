// net-test: networking through Java on loopback (ADR-0054's tests, step c2). dalvikvm runs it as dalvikvm64 -Xint does
// on Android 15, and both print the same lines. Each part prints what does not differ between devices or runs (no
// ports, no timings, no device names): InetAddress through the hosts file; NetworkInterface's lo; TCP over 127.0.0.1
// and ::1 with a 1 MiB transfer, the options read back, a refused connect and a read that times out; UDP datagrams
// up to Linux's longest, 65,507 bytes over IPv4 and 65,527 over IPv6, beyond Darwin's 9,216, also after SO_SNDBUF and
// SO_RCVBUF shrink; NIO with a Selector; asynchronous channels; HttpURLConnection, okhttp's, to a loopback server;
// the default key store (BKS) taking a fixed self-signed key; and TLS on loopback with that key (ADR-0046 test 2).
// A part that throws prints the exception and the next part runs. It needs Conscrypt, okhttp and BouncyCastle
// (art_run.py's --conscrypt --okhttp --bouncycastle).
import java.io.BufferedReader;
import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.ConnectException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.HttpURLConnection;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.URL;
import java.nio.ByteBuffer;
import java.nio.channels.AsynchronousServerSocketChannel;
import java.nio.channels.AsynchronousSocketChannel;
import java.nio.channels.CompletionHandler;
import java.nio.channels.SelectionKey;
import java.nio.channels.Selector;
import java.nio.channels.ServerSocketChannel;
import java.nio.channels.SocketChannel;
import java.nio.charset.StandardCharsets;
import java.security.KeyFactory;
import java.security.KeyStore;
import java.security.PrivateKey;
import java.security.cert.Certificate;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.security.spec.PKCS8EncodedKeySpec;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Base64;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import javax.net.ssl.KeyManagerFactory;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLServerSocket;
import javax.net.ssl.SSLSession;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.TrustManagerFactory;

public class Main {
    // Made for this test with OpenSSL: a P-256 key (PKCS #8) and its self-signed certificate, SHA256withECDSA,
    // O=Minato, CN=net-test, valid for 100 years.
    static final String KEY =
            "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgjh3efZ0yaLcwGzL/9oewzU93jSrmos1m8QMVnZeco5mhRANC"
            + "AARIJjNbEWVHx9RIxzY37DzBoNZr4tSuuEtmGZjZbWmsLpDkmxICp6aJXeShJ2LiMoR3kHA7OBaImV3NzcRghHPd";
    static final String CERTIFICATE =
            "MIIBnjCCAUWgAwIBAgIULDlyOQdOb6Gy7H7Hxal8z+WBRj0wCgYIKoZIzj0EAwIwJDEPMA0GA1UECgwGTWluYXRvMREwDwYDVQQD"
            + "DAhuZXQtdGVzdDAgFw0yNjEwMTMxNDE3NTVaGA8yMTI2MDkxOTE0MTc1NVowJDEPMA0GA1UECgwGTWluYXRvMREwDwYDVQQDDAhu"
            + "ZXQtdGVzdDBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABEgmM1sRZUfH1EjHNjfsPMGg1mvi1K64S2YZmNltaawukOSbEgKnpold"
            + "5KEnYuIyhHeQcDs4FoiZXc3NxGCEc92jUzBRMB0GA1UdDgQWBBQgpJu0jYmdq+tkWflN4cADK10PWDAfBgNVHSMEGDAWgBQgpJu0"
            + "jYmdq+tkWflN4cADK10PWDAPBgNVHRMBAf8EBTADAQH/MAoGCCqGSM49BAMCA0cAMEQCIE4aJlRdqo9saNo2lZWf0M4Kv5krvN0E"
            + "ZeldKpyq+6hhAiBlsrLSCXl6NxeIyzmnGN/LCySJ3zZFAzJdt+7TZgrWOg==";
    static final char[] PASSWORD = "net-test".toCharArray();

    interface Part {
        void run() throws Exception;
    }

    public static void main(String[] args) throws Exception {
        part("addresses", Main::addresses);
        part("interfaces", Main::interfaces);
        part("tcp ipv4", () -> tcp(InetAddress.getByName("127.0.0.1")));
        part("tcp ipv6", () -> tcp(InetAddress.getByName("::1")));
        part("tcp options", Main::tcpOptions);
        part("refused", Main::refused);
        part("timeout", Main::timeout);
        part("udp ipv4", () -> udp(InetAddress.getByName("127.0.0.1"), 65_507, 65_507));
        part("udp ipv6", () -> udp(InetAddress.getByName("::1"), 65_527, 20_000));
        part("selector", Main::selector);
        part("asynchronous", Main::asynchronous);
        part("http", Main::http);
        part("key store", Main::keyStore);
        part("tls", Main::tls);
        System.out.println("done");
    }

    static void part(String name, Part part) {
        System.out.println("-- " + name);
        try {
            part.run();
        } catch (Throwable thrown) {
            System.out.println("failed: " + thrown);
            for (Throwable cause = thrown.getCause(); cause != null; cause = cause.getCause()) {
                System.out.println("  caused by: " + cause);
            }
        }
    }

    static InetAddress loopback4() throws IOException {
        return InetAddress.getByName("127.0.0.1");
    }

    static void addresses() throws Exception {
        System.out.println("localhost: " + Arrays.toString(InetAddress.getAllByName("localhost")));
        InetAddress loopback = InetAddress.getLoopbackAddress();
        System.out.println("loopback: " + loopback + ", " + loopback.isLoopbackAddress());
        InetAddress six = InetAddress.getByName("::1");
        System.out.println("::1: " + six.getClass().getName() + " " + six.getHostAddress() + ", "
                + six.isLoopbackAddress());
        System.out.println("127.1.2.3 is loopback: " + InetAddress.getByName("127.1.2.3").isLoopbackAddress());
        InetAddress named = InetAddress.getByAddress("named", new byte[] {127, 0, 0, 1});
        System.out.println("by address: " + named + ", " + (named instanceof Inet4Address));
    }

    static void interfaces() throws Exception {
        NetworkInterface lo = NetworkInterface.getByName("lo");
        System.out.println("lo: index " + lo.getIndex() + ", up " + lo.isUp() + ", loopback " + lo.isLoopback()
                + ", point to point " + lo.isPointToPoint() + ", virtual " + lo.isVirtual() + ", mtu " + lo.getMTU());
        List<String> addresses = new ArrayList<>();
        for (InterfaceAddress address : lo.getInterfaceAddresses()) {
            addresses.add(address.getAddress().getHostAddress() + "/" + address.getNetworkPrefixLength());
        }
        Collections.sort(addresses);
        System.out.println("lo's addresses: " + addresses);
        System.out.println("index 1: " + NetworkInterface.getByIndex(1).getName());
        System.out.println("127.0.0.1's: " + NetworkInterface.getByInetAddress(loopback4()).getName());
    }

    static String readLine(Socket socket) throws IOException {
        return new BufferedReader(new InputStreamReader(socket.getInputStream(), StandardCharsets.UTF_8)).readLine();
    }

    static void write(Socket socket, String text) throws IOException {
        socket.getOutputStream().write(text.getBytes(StandardCharsets.UTF_8));
    }

    static void tcp(InetAddress address) throws Exception {
        try (ServerSocket server = new ServerSocket(0, 50, address);
                Socket client = new Socket(address, server.getLocalPort());
                Socket accepted = server.accept()) {
            write(client, "ping\n");
            InputStream in = accepted.getInputStream();
            byte[] line = new byte[5];
            readFully(in, line);
            write(accepted, new String(line, StandardCharsets.UTF_8).trim() + " pong\n");
            System.out.println("echo: " + readLine(client));
            System.out.println("client's local address: " + client.getLocalAddress().getHostAddress()
                    + ", accepted's peer: " + accepted.getInetAddress().getHostAddress() + ", ports match: "
                    + (accepted.getPort() == client.getLocalPort()));
            // 1 MiB from the accepted socket while the client reads it, then the end of the stream.
            final byte[] data = pattern(1 << 20);
            Thread writer = new Thread(() -> {
                try {
                    accepted.getOutputStream().write(data);
                    accepted.shutdownOutput();
                } catch (IOException e) {
                    System.out.println("writer: " + e);
                }
            });
            writer.start();
            byte[] received = new byte[data.length];
            InputStream clientIn = client.getInputStream();
            readFully(clientIn, received);
            writer.join();
            System.out.println("1 MiB intact: " + Arrays.equals(data, received) + ", then end: " + clientIn.read());
        }
    }

    static void tcpOptions() throws Exception {
        try (Socket socket = new Socket()) {
            System.out.println("at first: nodelay " + socket.getTcpNoDelay() + ", keepalive " + socket.getKeepAlive()
                    + ", linger " + socket.getSoLinger() + ", timeout " + socket.getSoTimeout() + ", send "
                    + socket.getSendBufferSize() + ", receive " + socket.getReceiveBufferSize());
            socket.setTcpNoDelay(true);
            socket.setKeepAlive(true);
            socket.setSoLinger(true, 5);
            socket.setSendBufferSize(65_536);
            socket.setReceiveBufferSize(4_096);
            System.out.println("set: nodelay " + socket.getTcpNoDelay() + ", keepalive " + socket.getKeepAlive()
                    + ", linger " + socket.getSoLinger() + ", send " + socket.getSendBufferSize() + ", receive "
                    + socket.getReceiveBufferSize());
        }
        try (ServerSocket server = new ServerSocket()) {
            System.out.println("server: reuse address " + server.getReuseAddress() + ", receive "
                    + server.getReceiveBufferSize() + ", bound " + server.isBound());
        }
    }

    static void refused() throws Exception {
        int port;
        try (ServerSocket gone = new ServerSocket(0, 50, loopback4())) {
            port = gone.getLocalPort();
        }
        try (Socket socket = new Socket(loopback4(), port)) {
            System.out.println("connected");
        } catch (ConnectException e) {
            System.out.println(e.getClass().getName() + ", ECONNREFUSED: " + e.getMessage().contains("ECONNREFUSED"));
        }
    }

    static void timeout() throws Exception {
        try (ServerSocket server = new ServerSocket(0, 50, loopback4());
                Socket client = new Socket(loopback4(), server.getLocalPort());
                Socket accepted = server.accept()) {
            client.setSoTimeout(200);
            try {
                System.out.println("read: " + client.getInputStream().read());
            } catch (IOException e) {
                System.out.println(e.getClass().getName() + ": " + e.getMessage());
            }
        }
    }

    static byte[] pattern(int length) {
        byte[] data = new byte[length];
        for (int i = 0; i < length; i++) data[i] = (byte) (i * 31 + i / 251);
        return data;
    }

    static void readFully(InputStream in, byte[] into) throws IOException {
        for (int at = 0; at < into.length; ) {
            int got = in.read(into, at, into.length - at);
            if (got < 0) throw new IOException("ended after " + at);
            at += got;
        }
    }

    /** Sends `length` bytes to the socket itself and says what came back. */
    static String round(DatagramSocket socket, int length, boolean named) {
        byte[] data = pattern(length);
        try {
            if (named) {
                socket.send(new DatagramPacket(data, length, socket.getLocalSocketAddress()));
            } else {
                socket.send(new DatagramPacket(data, length));
            }
            byte[] room = new byte[70_000];
            DatagramPacket packet = new DatagramPacket(room, room.length);
            socket.receive(packet);
            boolean intact = packet.getLength() == length
                    && Arrays.equals(data, Arrays.copyOf(room, packet.getLength()));
            return length + ": " + packet.getLength() + (intact ? " intact" : " changed");
        } catch (IOException e) {
            return length + ": " + e.getClass().getName() + ": " + e.getMessage();
        }
    }

    /**
     * Datagrams to the socket itself, up to the longest, `longest`, also connected; then, from a fresh socket with
     * small buffers, one of `small` bytes, which lo does not fragment: Linux refuses a datagram whose fragments outgrow
     * twice SO_SNDBUF with ENOBUFS, which the runtime does not (ADR-0054 step c2's departure), and over IPv6 lo
     * fragments the longest. Fresh, as a disconnect differs (the same departures).
     */
    static void udp(InetAddress address, int longest, int small) throws Exception {
        try (DatagramSocket socket = new DatagramSocket(0, address)) {
            socket.setSoTimeout(5_000);
            System.out.println("buffers: send " + socket.getSendBufferSize() + ", receive "
                    + socket.getReceiveBufferSize());
            for (int length : new int[] {1, 9_216, 9_217, 20_000, longest, longest + 1}) {
                System.out.println(round(socket, length, true));
            }
            socket.connect(socket.getLocalSocketAddress());
            System.out.println("connected: " + round(socket, longest, false));
            System.out.println("connected: " + round(socket, longest + 1, false));
        }
        try (DatagramSocket socket = new DatagramSocket(0, address)) {
            socket.setSoTimeout(5_000);
            socket.setSendBufferSize(1_024);
            socket.setReceiveBufferSize(1_024);
            System.out.println("small buffers: send " + socket.getSendBufferSize() + ", receive "
                    + socket.getReceiveBufferSize());
            System.out.println(round(socket, small, true));
        }
    }

    static void selector() throws Exception {
        try (Selector selector = Selector.open();
                ServerSocketChannel server = ServerSocketChannel.open();
                SocketChannel client = SocketChannel.open()) {
            server.bind(new InetSocketAddress(loopback4(), 0));
            server.configureBlocking(false);
            server.register(selector, SelectionKey.OP_ACCEPT);
            client.configureBlocking(false);
            if (!client.connect(server.getLocalAddress())) client.register(selector, SelectionKey.OP_CONNECT);
            SocketChannel accepted = null;
            boolean connected = client.isConnected();
            while (accepted == null || !connected) {
                selector.select(5_000);
                for (SelectionKey key : selector.selectedKeys()) {
                    if (key.isAcceptable()) accepted = server.accept();
                    if (key.isConnectable()) {
                        connected = client.finishConnect();
                        key.cancel();
                    }
                }
                selector.selectedKeys().clear();
            }
            System.out.println("accepted and connected: " + accepted.isConnected() + ", " + client.isConnected());
            accepted.configureBlocking(false);
            SelectionKey readable = accepted.register(selector, SelectionKey.OP_READ);
            System.out.println("ready before a write: " + selector.selectNow());
            client.write(ByteBuffer.wrap("selected".getBytes(StandardCharsets.UTF_8)));
            int ready = selector.select(5_000);
            ByteBuffer buffer = ByteBuffer.allocate(64);
            int read = accepted.read(buffer);
            System.out.println("ready after a write: " + ready + ", readable " + readable.isReadable() + ", read "
                    + new String(buffer.array(), 0, read, StandardCharsets.UTF_8));
            selector.selectedKeys().clear();
            Thread waker = new Thread(selector::wakeup);
            waker.start();
            System.out.println("woken: " + selector.select(10_000));
            waker.join();
            client.close();
            selector.select(5_000);
            buffer.clear();
            System.out.println("after the client closed: " + accepted.read(buffer));
            accepted.close();
        }
    }

    static void asynchronous() throws Exception {
        try (AsynchronousServerSocketChannel server = AsynchronousServerSocketChannel.open();
                AsynchronousSocketChannel client = AsynchronousSocketChannel.open()) {
            server.bind(new InetSocketAddress(loopback4(), 0));
            java.util.concurrent.Future<AsynchronousSocketChannel> accepting = server.accept();
            client.connect(server.getLocalAddress()).get(5, TimeUnit.SECONDS);
            try (AsynchronousSocketChannel accepted = accepting.get(5, TimeUnit.SECONDS)) {
                int written = client.write(ByteBuffer.wrap("async".getBytes(StandardCharsets.UTF_8)))
                        .get(5, TimeUnit.SECONDS);
                ByteBuffer buffer = ByteBuffer.allocate(16);
                int read = accepted.read(buffer).get(5, TimeUnit.SECONDS);
                System.out.println("future: wrote " + written + ", read "
                        + new String(buffer.array(), 0, read, StandardCharsets.UTF_8));
                final CountDownLatch done = new CountDownLatch(1);
                final AtomicReference<String> result = new AtomicReference<>();
                final ByteBuffer second = ByteBuffer.allocate(16);
                accepted.read(second, null, new CompletionHandler<Integer, Void>() {
                    @Override
                    public void completed(Integer count, Void attachment) {
                        result.set("read " + new String(second.array(), 0, count, StandardCharsets.UTF_8));
                        done.countDown();
                    }

                    @Override
                    public void failed(Throwable thrown, Void attachment) {
                        result.set("failed: " + thrown);
                        done.countDown();
                    }
                });
                client.write(ByteBuffer.wrap("handler".getBytes(StandardCharsets.UTF_8))).get(5, TimeUnit.SECONDS);
                System.out.println("handler: " + done.await(5, TimeUnit.SECONDS) + ", " + result.get());
            }
        }
    }

    static void http() throws Exception {
        try (final ServerSocket server = new ServerSocket(0, 50, loopback4())) {
            final AtomicReference<String> seen = new AtomicReference<>();
            Thread serving = new Thread(() -> {
                try (Socket socket = server.accept()) {
                    BufferedReader in = new BufferedReader(
                            new InputStreamReader(socket.getInputStream(), StandardCharsets.US_ASCII));
                    String requestLine = in.readLine();
                    Map<String, String> headers = new TreeMap<>(String.CASE_INSENSITIVE_ORDER);
                    for (String line = in.readLine(); line != null && !line.isEmpty(); line = in.readLine()) {
                        int colon = line.indexOf(':');
                        headers.put(line.substring(0, colon), line.substring(colon + 1).trim());
                    }
                    seen.set(requestLine + ", headers " + headers.keySet() + ", X-Test " + headers.get("X-Test"));
                    OutputStream out = socket.getOutputStream();
                    String response = "HTTP/1.1 201 Made\r\nContent-Type: text/plain; charset=utf-8\r\n"
                            + "Content-Length: 6\r\nX-Served: loopback\r\nConnection: close\r\n\r\nhello\n";
                    out.write(response.getBytes(StandardCharsets.US_ASCII));
                    out.flush();
                } catch (IOException e) {
                    seen.set("server: " + e);
                }
            });
            serving.start();
            URL url = new URL("http://127.0.0.1:" + server.getLocalPort() + "/path?q=1");
            HttpURLConnection connection = (HttpURLConnection) url.openConnection();
            connection.setRequestProperty("X-Test", "net-test");
            connection.setConnectTimeout(5_000);
            connection.setReadTimeout(5_000);
            System.out.println("connection: " + connection.getClass().getName());
            int code = connection.getResponseCode();
            byte[] body = new byte[6];
            readFully(connection.getInputStream(), body);
            System.out.println("response: " + code + " " + connection.getResponseMessage() + ", "
                    + connection.getContentType() + ", " + connection.getContentLength() + ", X-Served "
                    + connection.getHeaderField("X-Served") + ", body "
                    + new String(body, StandardCharsets.UTF_8).trim());
            connection.disconnect();
            serving.join();
            System.out.println("request: " + seen.get());
        }
    }

    static PrivateKey key() throws Exception {
        return KeyFactory.getInstance("EC").generatePrivate(new PKCS8EncodedKeySpec(Base64.getDecoder().decode(KEY)));
    }

    static X509Certificate certificate() throws Exception {
        return (X509Certificate) CertificateFactory.getInstance("X.509")
                .generateCertificate(new ByteArrayInputStream(Base64.getDecoder().decode(CERTIFICATE)));
    }

    /** The default key store type, BKS, with the key: it seals the key as it takes it. */
    static void keyStore() throws Exception {
        KeyStore store = KeyStore.getInstance(KeyStore.getDefaultType());
        store.load(null, null);
        store.setKeyEntry("net-test", key(), PASSWORD, new Certificate[] {certificate()});
        System.out.println(store.getType() + ": " + Collections.list(store.aliases()) + ", key "
                + store.getKey("net-test", PASSWORD).getAlgorithm());
    }

    static void tls() throws Exception {
        PrivateKey key = key();
        X509Certificate certificate = certificate();
        KeyStore own = KeyStore.getInstance("PKCS12");
        own.load(null, null);
        own.setKeyEntry("net-test", key, PASSWORD, new Certificate[] {certificate});
        KeyManagerFactory keys = KeyManagerFactory.getInstance(KeyManagerFactory.getDefaultAlgorithm());
        keys.init(own, PASSWORD);
        KeyStore trusted = KeyStore.getInstance("PKCS12");
        trusted.load(null, null);
        trusted.setCertificateEntry("net-test", certificate);
        TrustManagerFactory trust = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
        trust.init(trusted);
        SSLContext context = SSLContext.getInstance("TLS");
        context.init(keys.getKeyManagers(), trust.getTrustManagers(), null);
        try (final SSLServerSocket server =
                (SSLServerSocket) context.getServerSocketFactory().createServerSocket(0, 50, loopback4())) {
            final AtomicReference<String> seen = new AtomicReference<>();
            Thread serving = new Thread(() -> {
                try (Socket socket = server.accept()) {
                    String line = readLine(socket);
                    seen.set(line);
                    write(socket, "echo: " + line + "\n");
                } catch (IOException e) {
                    seen.set("server: " + e);
                }
            });
            serving.start();
            try (SSLSocket client =
                    (SSLSocket) context.getSocketFactory().createSocket(loopback4(), server.getLocalPort())) {
                client.startHandshake();
                SSLSession session = client.getSession();
                System.out.println("session: " + session.getProtocol() + ", " + session.getCipherSuite() + ", peer "
                        + ((X509Certificate) session.getPeerCertificates()[0]).getSubjectX500Principal());
                write(client, "over tls\n");
                System.out.println(readLine(client));
            }
            serving.join();
            System.out.println("server read: " + seen.get());
        }
    }
}
