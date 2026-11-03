/* The host's network for the guest's ConnectivityManager (ADR-0054 step d)): Network.framework's path monitor, on a
   queue of its own, reports each path to the runtime (apkrt_network_set) as APKRT_NETWORK_* flags, from
   minato_graphics_set_network on, for the process's life. */
#import <Network/Network.h>

#include "minato_graphics.h"

#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"

static uint32_t minato_network_state(nw_path_t path) {
    uint32_t state = 0;
    if (nw_path_get_status(path) == nw_path_status_satisfied) state |= APKRT_NETWORK_SATISFIED;
    if (nw_path_uses_interface_type(path, nw_interface_type_wifi)) state |= APKRT_NETWORK_WIFI;
    if (nw_path_uses_interface_type(path, nw_interface_type_cellular)) state |= APKRT_NETWORK_CELLULAR;
    if (nw_path_uses_interface_type(path, nw_interface_type_wired)) state |= APKRT_NETWORK_WIRED;
    if (nw_path_is_expensive(path)) state |= APKRT_NETWORK_EXPENSIVE;
    if (nw_path_is_constrained(path)) state |= APKRT_NETWORK_CONSTRAINED;
    return state;
}

int minato_graphics_set_network(void) {
    static nw_path_monitor_t monitor;  // kept for the process's life
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        monitor = nw_path_monitor_create();
        if (monitor == nil) return;
        nw_path_monitor_set_queue(monitor, dispatch_queue_create("minato.network", DISPATCH_QUEUE_SERIAL));
        nw_path_monitor_set_update_handler(monitor, ^(nw_path_t path) {
            apkrt_network_set(minato_network_state(path));
        });
        nw_path_monitor_start(monitor);
    });
    return monitor != nil ? 0 : -1;
}
