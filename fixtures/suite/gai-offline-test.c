// gai-offline-test: what getaddrinfo answers when the network cannot be reached (airplane mode, a denied network):
// the EAI code, its text, and errno, which libcore reads after a failed lookup (libcore_io_Linux.cpp throwGaiException
// attaches an ErrnoException when errno is not 0, and Inet6AddressImpl turns EACCES or EPERM into a SecurityException
// "missing INTERNET permission?" and anything else into UnknownHostException "Unable to resolve host ...: <text>").
// Three names: one that needs the network, one the hosts file answers (localhost), and one that exists nowhere. The
// run is meant for a process without a network: on Android in airplane mode, under runtime/tests/gai_offline_run.py
// on the Mac. Expected output: fixtures/expected/gai-offline-test.out
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

static void lookup(const char *name) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *result = NULL;
    errno = 0;
    const int error = getaddrinfo(name, "80", &hints, &result);
    const int saved = errno;
    if (error == 0) {
        int count = 0;
        for (const struct addrinfo *entry = result; entry != NULL; entry = entry->ai_next) count++;
        printf("gai-offline-test: %s: resolved %s\n", name, count > 0 ? "yes" : "none");
        freeaddrinfo(result);
        return;
    }
    printf("gai-offline-test: %s: rc=%d (%s) errno=%d (%s)\n", name, error, gai_strerror(error), saved,
           saved == 0 ? "none" : strerror(saved));
}

int main(void) {
    lookup("example.com");
    lookup("localhost");
    lookup("no-such-name.invalid");
    return 0;
}
