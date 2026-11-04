/* The host's network for the guest's ConnectivityManager (ADR-0054 step d)): Network.framework's path monitor, on a
   queue of its own, reports each path to the runtime (apkrt_network_set) as APKRT_NETWORK_* flags, from
   minato_graphics_set_network on, for the process's life. On iOS a cellular path carries its radio too, CoreTelephony's
   access technology of the data service as Android's NETWORK_TYPE_*, reported again when it changes. */
#import <Network/Network.h>
#include <TargetConditionals.h>
#if TARGET_OS_IOS
#import <CoreTelephony/CTTelephonyNetworkInfo.h>
#endif

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

#if TARGET_OS_IOS
/* CoreTelephony's access technology as TelephonyManager's NETWORK_TYPE_*: 5G next to LTE (NRNSA) is LTE, as
   Android's NetworkInfo has it (the NR shows in its display info only); 0 for none or one it does not name. */
static uint32_t minato_network_radio(CTTelephonyNetworkInfo *telephony) {
    NSString *service = telephony.dataServiceIdentifier;
    NSString *technology = service != nil ? telephony.serviceCurrentRadioAccessTechnology[service] : nil;
    if (technology == nil) return 0;
    NSDictionary<NSString *, NSNumber *> *types = @{
        CTRadioAccessTechnologyGPRS: @1,          CTRadioAccessTechnologyEdge: @2,
        CTRadioAccessTechnologyWCDMA: @3,         CTRadioAccessTechnologyCDMAEVDORev0: @5,
        CTRadioAccessTechnologyCDMAEVDORevA: @6,  CTRadioAccessTechnologyCDMA1x: @7,
        CTRadioAccessTechnologyHSDPA: @8,         CTRadioAccessTechnologyHSUPA: @9,
        CTRadioAccessTechnologyCDMAEVDORevB: @12, CTRadioAccessTechnologyLTE: @13,
        CTRadioAccessTechnologyeHRPD: @14,        CTRadioAccessTechnologyNRNSA: @13,
        CTRadioAccessTechnologyNR: @20,
    };
    return types[technology].unsignedIntValue;
}
#endif

int minato_graphics_set_network(void) {
    static nw_path_monitor_t monitor;  // kept for the process's life
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        monitor = nw_path_monitor_create();
        if (monitor == nil) return;
        dispatch_queue_t queue = dispatch_queue_create("minato.network", DISPATCH_QUEUE_SERIAL);
        nw_path_monitor_set_queue(monitor, queue);
#if TARGET_OS_IOS
        // The last path's flags and the radio, both on the queue; the radio's own changes come as a notification.
        static uint32_t path_state;
        static CTTelephonyNetworkInfo *telephony;
        telephony = [CTTelephonyNetworkInfo new];
        void (^report)(void) = ^{
            uint32_t state = path_state;
            if ((state & APKRT_NETWORK_CELLULAR) != 0) {
                state |= (minato_network_radio(telephony) << APKRT_NETWORK_RADIO_SHIFT) & APKRT_NETWORK_RADIO_MASK;
            }
            apkrt_network_set(state);
        };
        nw_path_monitor_set_update_handler(monitor, ^(nw_path_t path) {
            path_state = minato_network_state(path);
            report();
        });
        [NSNotificationCenter.defaultCenter addObserverForName:CTServiceRadioAccessTechnologyDidChangeNotification
                                                        object:nil
                                                         queue:nil
                                                    usingBlock:^(NSNotification *note) {
                                                        (void)note;
                                                        dispatch_async(queue, report);
                                                    }];
#else
        nw_path_monitor_set_update_handler(monitor, ^(nw_path_t path) {
            apkrt_network_set(minato_network_state(path));
        });
#endif
        nw_path_monitor_start(monitor);
    });
    return monitor != nil ? 0 : -1;
}
