/* graphics-run's session layers (ADR-0043 c4 (c), ADR-0041 gap 4 (c)): with MINATO_SESSION_LAYERS=1, apk-run's
   --lifecycle gets the layers of apk_run_layers.h, as the App's session view holds them: a root layer of the session's
   size at scale 1, and in it, bottom to top, in Android 15's order of windows as APKSession's WindowOrder has it
   (docs/plans/adr-0041-second-windows.md, decision 1):
   - the window's MinatoMetalLayer at the session's bounds, the same at every start and return, opaque until the
     runtime reports otherwise (APKRT_REQUEST_WINDOW_OPAQUE);
   - a MinatoMetalLayer per surface of c4, at its frame in the window's pixels and opaque as its request says, and per
     requested window, at its frame in the session's pixels, opaque as its request says, with its alpha as its
     opacity: the activities' groups by the guest's key, the session's window in group 0, and the toasts above them;
     in a group the base window lowest and a starting window highest, the others newer above; around each window its
     children, the session window's surfaces and each window's sub-windows, by sublayer (a surface's z) and then newer
     above, below the window for a negative sublayer and above it otherwise;
   - one black dim at the session's bounds, below the topmost window that dims and below its children, at its amount.
   A place moves a layer, and its window's removal takes it out. The tree changes on a serial queue of its own, in the
   order asked, as the hooks may not block; the window's opacity changes at once, before its next post. With
   MINATO_WINDOW_WATCH=hidden, the window's layer is the one watched, and the window host prints:
   - each surface as it comes, when a place moves it and as it goes: "window host: a surface below the window at
     <left>, <top>, <width> x <height>" (or above it), "window host: a surface goes";
   - each requested window as it comes and at each place that changes it, and as it goes: "window host: window <n>,
     type <type>: at <left>, <top>, <width> x <height>, alpha <alpha>, dim <amount>, opaque" (or "not opaque"), the
     same from ", type" to the dim as the app's line of second-windows-test for its window, and "window host: window
     <n> goes", where <n> counts the requested windows made;
   - the tree whenever it changes: "window host: from the bottom: " and its layers, "the window", "surface <n>",
     "window <n>" and "dim", where <n> counts the surfaces or the requested windows made. */
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "APKRuntime/apkrt.h"
#include "apk_run_layers.h"
#include "minato_layer.h"
#include "session_layers.h"

/* MINATO_WINDOW_WATCH=hidden: CALayer's setHidden:, which prints each change of the window layer's hidden on the
   thread that makes it. Key-value observing never saw those changes [measured], probably as Core Animation makes
   its setters when they are first used [inferred]. */
static CALayer *watched;
static IMP set_hidden;

static void watched_set_hidden(id self, SEL selector, BOOL hidden) {
    if (self == watched && ((CALayer *)self).hidden != hidden) {
        fprintf(stderr, "window host: the layer is %s\n", hidden ? "hidden" : "shown");
    }
    ((void (*)(id, SEL, BOOL))set_hidden)(self, selector, hidden);
}

BOOL minato_test_watch_hidden(CALayer *layer) {
    const char *watch = getenv("MINATO_WINDOW_WATCH");
    if (watch == NULL || strcmp(watch, "hidden") != 0) {
        return NO;
    }
    watched = layer;
    layer.hidden = NO;  // the setter, made
    set_hidden = method_setImplementation(class_getInstanceMethod([layer class], @selector(setHidden:)),
                                          (IMP)watched_set_hidden);
    return YES;
}

/* MINATO_SESSION_LAYERS=1: the session's layers (apk_run_layers.h). The entries live on the tree's queue. */
@interface MinatoLayerEntry : NSObject {
@public
    MinatoMetalLayer *layer;
    uint64_t token;
    BOOL window;            // a requested window; else c4's surface, a child of the session's window
    int32_t z;              // a surface's
    int32_t type;           // a requested window's LayoutParams.type
    uint64_t parent;        // a sub-window's window: a requested window's token, or 0 for the session's window
    uint32_t group;         // an application window's activity
    float alpha, dim;       // a requested window's alpha, and its dim's amount, 0 when it does not dim
    unsigned long made;     // among the tree's makes, surfaces and windows alike
    unsigned long number;   // among its kind's makes, for the lines
}
@end

@implementation MinatoLayerEntry
@end

static dispatch_queue_t tree;
static CALayer *session_root, *dim_layer;
static MinatoMetalLayer *session_window;
static BOOL printing;
static NSMutableArray<MinatoLayerEntry *> *entries;  // in the order made
static unsigned long made, surfaces_made, windows_made;
static NSString *printed_order;  // the tree's last line

static void without_actions(void (^change)(void)) {
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    change();
    [CATransaction commit];
}

static void *window_layer(int32_t width, int32_t height) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        tree = dispatch_queue_create("window host: the session's layers", DISPATCH_QUEUE_SERIAL);
        entries = [NSMutableArray array];
        session_root = [CALayer layer];
        session_window = [MinatoMetalLayer layer];
        dim_layer = [CALayer layer];
        without_actions(^{
            const CGRect bounds = CGRectMake(0, 0, width, height);
            session_root.bounds = bounds;
            session_window.contentsScale = 1;
            session_window.frame = bounds;
            session_window.opaque = YES;
            dim_layer.frame = bounds;
            dim_layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
            [session_root addSublayer:session_window];
        });
        printed_order = @"the window";
        printing = minato_test_watch_hidden(session_window);
    });
    return (__bridge void *)session_window;
}

static void window_opaque(int opaque) {
    without_actions(^{
        session_window.opaque = opaque != 0;
    });
}

/* WindowManagerPolicy.getSubWindowLayerFromTypeLw (Android 15): a sub-window type's sublayer, 0 for an unknown one. */
static int32_t sublayer(int32_t type) {
    switch (type) {
        case 1001:  // TYPE_APPLICATION_MEDIA
            return -2;
        case 1004:  // TYPE_APPLICATION_MEDIA_OVERLAY
            return -1;
        case 1000:  // TYPE_APPLICATION_PANEL
        case 1003:  // TYPE_APPLICATION_ATTACHED_DIALOG
            return 1;
        case 1002:  // TYPE_APPLICATION_SUB_PANEL
            return 2;
        case 1005:  // TYPE_APPLICATION_ABOVE_SUB_PANEL
            return 3;
        default:
            return 0;
    }
}

static BOOL is_sub_window(MinatoLayerEntry *entry) {
    return entry->window && entry->type >= 1000 && entry->type <= 1999;
}

static int32_t child_sublayer(MinatoLayerEntry *entry) {
    return entry->window ? sublayer(entry->type) : entry->z;
}

/* An application window's place in its group: the base window lowest, a starting window highest. */
static int rank(MinatoLayerEntry *entry) {
    return entry->type == 1 ? 0 : entry->type == 3 ? 2 : 1;
}

static NSComparisonResult by_made(MinatoLayerEntry *a, MinatoLayerEntry *b) {
    return a->made < b->made ? NSOrderedAscending : a->made > b->made ? NSOrderedDescending : NSOrderedSame;
}

/* The order being built: the layers so far, and the topmost dimming window with the place its layers begin. */
@interface MinatoOrder : NSObject {
@public
    NSMutableArray<CALayer *> *layers;
    NSDictionary<NSNumber *, NSArray<MinatoLayerEntry *> *> *children;  // by the parent's token, 0 for the session's
    MinatoLayerEntry *dimming;
    NSUInteger dim_at;
}
@end

@implementation MinatoOrder
@end

static void place_entry(MinatoOrder *order, MinatoLayerEntry *entry);

/* A window's layer among its own children: those of a negative sublayer below it, the others above it, each by
   sublayer and then in the order made. `entry` is nil for the session's window. */
static void place_window(MinatoOrder *order, CALayer *layer, MinatoLayerEntry *entry,
                         NSArray<MinatoLayerEntry *> *own) {
    const NSUInteger start = order->layers.count;
    NSArray<MinatoLayerEntry *> *sorted = [own sortedArrayUsingComparator:^(MinatoLayerEntry *a, MinatoLayerEntry *b) {
        const int32_t first = child_sublayer(a), second = child_sublayer(b);
        return first != second ? (first < second ? NSOrderedAscending : NSOrderedDescending) : by_made(a, b);
    }];
    for (MinatoLayerEntry *child in sorted) {
        if (child_sublayer(child) < 0) {
            place_entry(order, child);
        }
    }
    [order->layers addObject:layer];
    if (entry != nil && entry->dim > 0) {
        order->dimming = entry;
        order->dim_at = start;
    }
    for (MinatoLayerEntry *child in sorted) {
        if (child_sublayer(child) >= 0) {
            place_entry(order, child);
        }
    }
}

static void place_entry(MinatoOrder *order, MinatoLayerEntry *entry) {
    if (!entry->window) {
        [order->layers addObject:entry->layer];
        return;
    }
    NSArray<MinatoLayerEntry *> *own = order->children[@(entry->token)];
    place_window(order, entry->layer, entry, own != nil ? own : @[]);
}

static void append(NSMutableDictionary<NSNumber *, NSMutableArray<MinatoLayerEntry *> *> *lists, NSNumber *key,
                   MinatoLayerEntry *entry) {
    if (lists[key] == nil) {
        lists[key] = [NSMutableArray array];
    }
    [lists[key] addObject:entry];
}

/* On the tree's queue: the session's layers bottom to top, as WindowOrder.layers gives them, the dim among them below
   the topmost dimming window, whose amount goes to *amount (0 without one). A sub-window whose parent is not live
   lies among the session's window's children; the runtime keeps a sub-window's parent live. */
static NSArray<CALayer *> *ordered(float *amount) {
    NSMutableSet<NSNumber *> *parents = [NSMutableSet set];
    for (MinatoLayerEntry *entry in entries) {
        if (entry->window && !is_sub_window(entry)) {
            [parents addObject:@(entry->token)];
        }
    }
    NSMutableDictionary<NSNumber *, NSMutableArray<MinatoLayerEntry *> *> *children = [NSMutableDictionary dictionary];
    NSMutableDictionary<NSNumber *, NSMutableArray<MinatoLayerEntry *> *> *groups = [NSMutableDictionary dictionary];
    groups[@0u] = [NSMutableArray array];
    NSMutableArray<MinatoLayerEntry *> *system_windows = [NSMutableArray array];
    for (MinatoLayerEntry *entry in entries) {
        if (!entry->window) {
            append(children, @0ull, entry);
        } else if (is_sub_window(entry)) {
            append(children, [parents containsObject:@(entry->parent)] ? @(entry->parent) : @0ull, entry);
        } else if (entry->type >= 2000) {
            [system_windows addObject:entry];
        } else {
            append(groups, @(entry->group), entry);
        }
    }
    MinatoOrder *order = [[MinatoOrder alloc] init];
    order->layers = [NSMutableArray array];
    order->children = children;
    for (NSNumber *group in [groups.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
        if (group.unsignedIntValue == 0) {
            NSArray<MinatoLayerEntry *> *own = children[@0ull];
            place_window(order, session_window, nil, own != nil ? own : @[]);
        }
        NSArray<MinatoLayerEntry *> *sorted =
            [groups[group] sortedArrayUsingComparator:^(MinatoLayerEntry *a, MinatoLayerEntry *b) {
                const int first = rank(a), second = rank(b);
                return first != second ? (first < second ? NSOrderedAscending : NSOrderedDescending) : by_made(a, b);
            }];
        for (MinatoLayerEntry *entry in sorted) {
            place_entry(order, entry);
        }
    }
    for (MinatoLayerEntry *entry in system_windows) {  // in the order made
        place_entry(order, entry);
    }
    *amount = 0;
    if (order->dimming != nil) {
        [order->layers insertObject:dim_layer atIndex:order->dim_at];
        *amount = order->dimming->dim;
    }
    return order->layers;
}

/* On the tree's queue: the root's layers in the order, the dim at its amount. */
static void arrange(void) {
    float amount = 0;
    NSArray<CALayer *> *order = ordered(&amount);
    without_actions(^{
        dim_layer.opacity = amount;
        session_root.sublayers = order;
    });
}

static NSString *name_of(CALayer *layer) {
    if (layer == session_window) {
        return @"the window";
    }
    if (layer == dim_layer) {
        return @"dim";
    }
    for (MinatoLayerEntry *entry in entries) {
        if (entry->layer == layer) {
            return entry->window ? [NSString stringWithFormat:@"window %lu", entry->number]
                                 : [NSString stringWithFormat:@"surface %lu", entry->number];
        }
    }
    return @"another layer";
}

/* With MINATO_WINDOW_WATCH=hidden: the tree from the bottom, when it changed since its last line. */
static void print_order(void) {
    if (!printing) {
        return;
    }
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    for (CALayer *layer in session_root.sublayers) {
        [names addObject:name_of(layer)];
    }
    NSString *line = [names componentsJoinedByString:@", "];
    if (![line isEqualToString:printed_order]) {
        printed_order = line;
        fprintf(stderr, "window host: from the bottom: %s\n", line.UTF8String);
    }
}

/* With MINATO_WINDOW_WATCH=hidden: the side of the window the surface's layer lies on and its frame. */
static void print_place(MinatoLayerEntry *entry) {
    if (!printing) {
        return;
    }
    NSArray<CALayer *> *order = session_root.sublayers;
    const CGRect frame = entry->layer.frame;
    fprintf(stderr, "window host: a surface %s the window at %.0f, %.0f, %.0f x %.0f\n",
            [order indexOfObject:entry->layer] < [order indexOfObject:session_window] ? "below" : "above",
            frame.origin.x, frame.origin.y, frame.size.width, frame.size.height);
}

/* With MINATO_WINDOW_WATCH=hidden: a requested window's type, frame, alpha, dim and opacity. */
static void print_window(MinatoLayerEntry *entry) {
    if (!printing) {
        return;
    }
    const CGRect frame = entry->layer.frame;
    fprintf(stderr, "window host: window %lu, type %d: at %.0f, %.0f, %.0f x %.0f, alpha %.2f, dim %.2f, %s\n",
            entry->number, entry->type, frame.origin.x, frame.origin.y, frame.size.width, frame.size.height,
            (double)entry->alpha, (double)entry->dim, entry->layer.opaque ? "opaque" : "not opaque");
}

static MinatoLayerEntry *found(uint64_t token, BOOL window) {
    for (MinatoLayerEntry *entry in entries) {
        if (entry->token == token && entry->window == window) {
            return entry;
        }
    }
    return nil;
}

static float float_of(uint32_t bits) {
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static CGRect frame_of(int32_t left, int32_t top, int32_t right, int32_t bottom) {
    return CGRectMake(left, top, right - left, bottom - top);
}

static void *surface_layer(const struct apkrt_surface_request *request) {
    const struct apkrt_surface_request asked = *request;
    MinatoMetalLayer *layer = [[MinatoMetalLayer alloc] init];
    without_actions(^{
        layer.contentsScale = 1;
        layer.opaque = asked.opaque != 0;
        layer.frame = frame_of(asked.left, asked.top, asked.right, asked.bottom);
    });
    dispatch_async(tree, ^{
        MinatoLayerEntry *entry = [[MinatoLayerEntry alloc] init];
        entry->layer = layer;
        entry->token = asked.token;
        entry->z = asked.z;
        entry->made = ++made;
        entry->number = ++surfaces_made;
        [entries addObject:entry];
        arrange();
        print_place(entry);
        print_order();
    });
    return (__bridge void *)layer;
}

/* A place: the frame, and another place by z when z changes; printed when either changes. */
static void surface_placed(const struct apkrt_surface_request *request) {
    const struct apkrt_surface_request asked = *request;
    dispatch_async(tree, ^{
        MinatoLayerEntry *entry = found(asked.token, NO);
        if (entry == nil) {
            return;
        }
        const CGRect frame = frame_of(asked.left, asked.top, asked.right, asked.bottom);
        const BOOL moved = !CGRectEqualToRect(entry->layer.frame, frame), turned = entry->z != asked.z;
        without_actions(^{
            entry->layer.frame = frame;
        });
        entry->z = asked.z;
        if (turned) {
            arrange();
        }
        if (moved || turned) {
            print_place(entry);
            print_order();
        }
    });
}

/* On the tree's queue: the entry whose layer went, out of the tree. */
static void take_out(void *layer, BOOL window) {
    CALayer *gone = (__bridge_transfer CALayer *)layer;
    for (MinatoLayerEntry *entry in entries) {
        if (entry->layer != gone || entry->window != window) {
            continue;
        }
        [entries removeObject:entry];
        arrange();
        if (printing && window) {
            fprintf(stderr, "window host: window %lu goes\n", entry->number);
        } else if (printing) {
            fprintf(stderr, "window host: a surface goes\n");
        }
        print_order();
        return;
    }
}

static void surface_removed(void *layer) {
    if (layer == NULL) {
        return;
    }
    dispatch_async(tree, ^{
        take_out(layer, NO);
    });
}

static void *requested_layer(const struct apkrt_window_request *request) {
    const struct apkrt_window_request asked = *request;
    MinatoMetalLayer *layer = [[MinatoMetalLayer alloc] init];
    const float alpha = float_of(asked.alpha);
    without_actions(^{
        layer.contentsScale = 1;
        layer.opaque = asked.opaque != 0;
        layer.opacity = alpha;
        layer.frame = frame_of(asked.left, asked.top, asked.right, asked.bottom);
    });
    dispatch_async(tree, ^{
        MinatoLayerEntry *entry = [[MinatoLayerEntry alloc] init];
        entry->layer = layer;
        entry->token = asked.token;
        entry->window = YES;
        entry->type = asked.type;
        entry->parent = asked.parent;
        entry->group = asked.group;
        entry->alpha = alpha;
        entry->dim = float_of(asked.dim);
        entry->made = ++made;
        entry->number = ++windows_made;
        [entries addObject:entry];
        arrange();
        print_window(entry);
        print_order();
    });
    return (__bridge void *)layer;
}

/* A place: the frame, the alpha and the dim, which may move the dim; printed when one of them changes. */
static void requested_placed(const struct apkrt_window_request *request) {
    const struct apkrt_window_request asked = *request;
    dispatch_async(tree, ^{
        MinatoLayerEntry *entry = found(asked.token, YES);
        if (entry == nil) {
            return;
        }
        const CGRect frame = frame_of(asked.left, asked.top, asked.right, asked.bottom);
        const float alpha = float_of(asked.alpha), dim = float_of(asked.dim);
        if (CGRectEqualToRect(entry->layer.frame, frame) && entry->alpha == alpha && entry->dim == dim) {
            return;
        }
        entry->alpha = alpha;
        entry->dim = dim;
        without_actions(^{
            entry->layer.frame = frame;
            entry->layer.opacity = alpha;
        });
        arrange();
        print_window(entry);
        print_order();
    });
}

static void requested_removed(void *layer) {
    if (layer == NULL) {
        return;
    }
    dispatch_async(tree, ^{
        take_out(layer, YES);
    });
}

static const struct apk_run_layer_hooks layer_hooks = {window_layer,     window_opaque,    surface_layer,
                                                       surface_placed,   surface_removed,  requested_layer,
                                                       requested_placed, requested_removed};

const struct apk_run_layer_hooks *apk_run_layers(void) {
    const char *layers = getenv("MINATO_SESSION_LAYERS");
    return layers != NULL && strcmp(layers, "1") == 0 ? &layer_hooks : NULL;
}

CALayer *minato_test_session_root(void) {
    return session_root;
}

void minato_test_session_settle(void) {
    if (tree != NULL) {
        dispatch_sync(tree, ^{
        });
    }
}
