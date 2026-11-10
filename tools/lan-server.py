#!/usr/bin/env python3
"""The Mac's side of lan-test (tools/lan-check.sh, tools/java-start-check.sh, tools/android-reference.sh): three tiny
servers on fixed ports, for one run only, with nothing else in them.

  lan-server.py [--bind ADDRESS] [--seconds N]

  47081 (TCP)  echoes each line it gets; a line "bye" is echoed and ends the run
  47082 (TCP)  answers every request with a 200 and the body "hello"
  47083 (UDP)  echoes each datagram

It binds ADDRESS (127.0.0.1 without --bind) only while it runs, ends at "bye" or after N seconds (90 without --seconds),
closes everything and prints what it served as counts, never an address or a name. The address is the caller's to pass,
at run time; nothing in the repository holds one. Python 3.9."""
import socket
import socketserver
import sys
import threading
import time

TCP_ECHO, HTTP, UDP_ECHO = 47081, 47082, 47083


class Counts:
    lock = threading.Lock()
    values = {"tcp lines": 0, "http requests": 0, "udp datagrams": 0}
    done = threading.Event()

    @classmethod
    def add(cls, key):
        with cls.lock:
            cls.values[key] += 1


class Echo(socketserver.StreamRequestHandler):
    def handle(self):
        for line in self.rfile:
            Counts.add("tcp lines")
            self.wfile.write(line)
            self.wfile.flush()
            if line.strip() == b"bye":
                Counts.done.set()
                return


class Hello(socketserver.StreamRequestHandler):
    def handle(self):
        while self.rfile.readline().strip():  # the request line and its headers
            pass
        Counts.add("http requests")
        body = b"hello"
        self.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n"
                         b"Connection: close\r\n\r\n%s" % (len(body), body))


class Datagrams(socketserver.BaseRequestHandler):
    def handle(self):
        data, sock = self.request
        Counts.add("udp datagrams")
        sock.sendto(data, self.client_address)


class Server(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True


class UdpServer(socketserver.ThreadingMixIn, socketserver.UDPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    args = sys.argv[1:]
    bind = args[args.index("--bind") + 1] if "--bind" in args else "127.0.0.1"
    seconds = int(args[args.index("--seconds") + 1]) if "--seconds" in args else 90
    servers = [Server((bind, TCP_ECHO), Echo), Server((bind, HTTP), Hello), UdpServer((bind, UDP_ECHO), Datagrams)]
    for server in servers:
        threading.Thread(target=server.serve_forever, daemon=True).start()
    print("lan-server: ready", flush=True)
    Counts.done.wait(seconds)
    time.sleep(0.2)
    for server in servers:
        server.shutdown()
        server.server_close()
    print("lan-server: " + ", ".join(f"{key} {value}" for key, value in sorted(Counts.values.items())), flush=True)


if __name__ == "__main__":
    main()
