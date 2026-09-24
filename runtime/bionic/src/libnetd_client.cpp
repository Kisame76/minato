// Minato's libnetd_client.so (ADR-0021). bionic's libc.so opens it in every process at start
// (NetdClient.cpp) and passes each socket function to one of these init functions, which on Android
// wrap them so that the netd service can mark and route the sockets. There is no netd here: the
// functions leave bionic's own in place. The library exists so that the dlopen succeeds as on
// Android and leaves no error behind for the program's first dlerror().

extern "C" {

#define APKRT_KEEP_BIONIC_FUNCTION(name) \
  __attribute__((visibility("default"))) void name(void*) {}

APKRT_KEEP_BIONIC_FUNCTION(netdClientInitAccept4)
APKRT_KEEP_BIONIC_FUNCTION(netdClientInitConnect)
APKRT_KEEP_BIONIC_FUNCTION(netdClientInitSendmmsg)
APKRT_KEEP_BIONIC_FUNCTION(netdClientInitSendmsg)
APKRT_KEEP_BIONIC_FUNCTION(netdClientInitSendto)
APKRT_KEEP_BIONIC_FUNCTION(netdClientInitSocket)
APKRT_KEEP_BIONIC_FUNCTION(netdClientInitNetIdForResolv)
APKRT_KEEP_BIONIC_FUNCTION(netdClientInitDnsOpenProxy)

}  // extern "C"
