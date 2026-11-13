/* graphics.session_layers (ADR-0041's second windows (c)): graphics-run's session layers
   (session_layers.m) through apk_run_layers.h's hooks, as apk-run's --lifecycle calls them, without a guest. The
   cases are WindowOrderTests' (APKSession), so that the Mac's tree and the App's order cannot drift: the session's
   layers bottom to top as Core Animation draws them, c4's surfaces and the requested windows in Android 15's order,
   one dim below the topmost dimming window, each window's frame, opacity and alpha, and the window host's lines as
   layers come and go (MINATO_WINDOW_WATCH=hidden). Exits 1 after the cases when one failed. */
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "APKRuntime/apkrt.h"
#include "apk_run_layers.h"
#include "session_layers.h"

// WindowManager.LayoutParams' types.
enum {
    BASE = 1,
    APPLICATION = 2,
    STARTING = 3,
    PANEL = 1000,
    MEDIA = 1001,
    SUB_PANEL = 1002,
    ATTACHED_DIALOG = 1003,
    MEDIA_OVERLAY = 1004,
    ABOVE_SUB_PANEL = 1005,
    TOAST = 2005,
};

static const int32_t WIDTH = 1290, HEIGHT = 2796;
static const struct apk_run_layer_hooks *hooks;
static CALayer *session_window;
static NSMapTable<CALayer *, NSString *> *names;  // each made layer's name: s<token> or w<token>
static NSMutableArray<NSString *> *made;          // the names in make order, for the removal
static NSMutableDictionary<NSString *, CALayer *> *layers;
static unsigned long surfaces_made, windows_made;
static NSMutableDictionary<NSString *, NSNumber *> *numbers;  // each name's number among its kind, for the lines
static int failures;

static void fail(const char *test, NSString *what) {
    fprintf(stderr, "FAILED  %s: %s\n", test, what.UTF8String);
    failures++;
}

static uint32_t bits(float value) {
    uint32_t result;
    memcpy(&result, &value, sizeof result);
    return result;
}

static struct apkrt_surface_request surface_request(uint64_t token, int32_t z) {
    struct apkrt_surface_request request = {0};
    request.op = APKRT_SURFACE_MAKE;
    request.token = token;
    request.z = z;
    request.left = 0;
    request.top = 0;
    request.right = 100;
    request.bottom = 100;
    request.width = 100;
    request.height = 100;
    request.format = 4;  // RGB_565
    request.opaque = 1;
    return request;
}

static struct apkrt_window_request window_request(uint64_t token, int32_t type, uint64_t parent, uint32_t group,
                                                  float dim) {
    struct apkrt_window_request request = {0};
    request.op = APKRT_WINDOW_MAKE;
    request.type = type;
    request.token = token;
    request.parent = parent;
    request.left = 0;
    request.top = 0;
    request.right = WIDTH;
    request.bottom = HEIGHT;
    request.width = WIDTH;
    request.height = HEIGHT;
    request.format = -3;  // PixelFormat.TRANSLUCENT
    request.opaque = 0;
    request.group = group;
    request.alpha = bits(1);
    request.dim = bits(dim);
    return request;
}

static void keep(NSString *name, void *layer) {
    CALayer *kept = (__bridge CALayer *)layer;
    [names setObject:name forKey:kept];
    [made addObject:name];
    layers[name] = kept;
}

static void surface(uint64_t token, int32_t z) {
    const struct apkrt_surface_request request = surface_request(token, z);
    NSString *name = [NSString stringWithFormat:@"s%llu", token];
    numbers[name] = @(++surfaces_made);
    keep(name, hooks->surface_layer(&request));
}

static void window(uint64_t token, int32_t type, uint64_t parent, uint32_t group, float dim) {
    const struct apkrt_window_request request = window_request(token, type, parent, group, dim);
    NSString *name = [NSString stringWithFormat:@"w%llu", token];
    numbers[name] = @(++windows_made);
    keep(name, hooks->requested_layer(&request));
}

/* The window's removal: the runtime hands the hook the reference apkrt_window_layer gave out. */
static void removed(NSString *name) {
    CALayer *layer = layers[name];
    void *reference = (void *)CFBridgingRetain(layer);
    if ([name hasPrefix:@"s"]) {
        hooks->surface_removed(reference);
    } else {
        hooks->requested_removed(reference);
    }
    [layers removeObjectForKey:name];
    [made removeObject:name];
}

/* Every layer the case made goes, the newest first, so that sub-windows go before their windows. */
static void clean(void) {
    while (made.count > 0) {
        removed(made.lastObject);
    }
    minato_test_session_settle();
}

/* The dim: a layer of the tree that no hook gave, with a color, drawn. */
static BOOL is_dim(CALayer *layer) {
    return [names objectForKey:layer] == nil && layer != session_window && layer.sublayers.count == 0 &&
           layer.backgroundColor != NULL && !layer.hidden;
}

/* The tree's layers in drawing order, containers opened: W for the session's window, dim, and the made layers'
   names. */
static void visit(CALayer *layer, NSMutableArray<NSString *> *order, NSMutableArray<CALayer *> *dims) {
    for (CALayer *sublayer in layer.sublayers) {
        NSString *name = [names objectForKey:sublayer];
        if (sublayer == session_window) {
            [order addObject:@"W"];
        } else if (name != nil) {
            [order addObject:name];
        } else if (is_dim(sublayer)) {
            [order addObject:@"dim"];
            [dims addObject:sublayer];
        } else {
            visit(sublayer, order, dims);
        }
    }
}

static NSArray<NSString *> *order(CALayer **dim) {
    minato_test_session_settle();
    NSMutableArray<NSString *> *result = [NSMutableArray array];
    NSMutableArray<CALayer *> *dims = [NSMutableArray array];
    visit(minato_test_session_root(), result, dims);
    if (dim != NULL) {
        *dim = dims.firstObject;
    }
    return result;
}

static void expect_order(const char *test, NSArray<NSString *> *expected) {
    NSArray<NSString *> *found = order(NULL);
    if (![found isEqualToArray:expected]) {
        fail(test, [NSString stringWithFormat:@"the layers from the bottom are %@, not %@",
                                              [found componentsJoinedByString:@" "],
                                              [expected componentsJoinedByString:@" "]]);
    }
}

/* The dim's opacity and place: black, at the session's bounds. */
static void expect_dim(const char *test, float amount) {
    CALayer *dim = nil;
    order(&dim);
    if (dim == nil) {
        fail(test, @"no dim");
        return;
    }
    const CGFloat *rgba = CGColorGetComponents(dim.backgroundColor);
    const size_t count = CGColorGetNumberOfComponents(dim.backgroundColor);
    const BOOL black = count >= 3 ? rgba[0] == 0 && rgba[1] == 0 && rgba[2] == 0 : rgba[0] == 0;
    if (fabsf(dim.opacity - amount) > 1e-6f || !black ||
        !CGRectEqualToRect(dim.frame, CGRectMake(0, 0, WIDTH, HEIGHT))) {
        fail(test, [NSString stringWithFormat:@"the dim has opacity %.3f, black %d, frame %@, not %.3f, 1, the "
                                              @"session's",
                                              dim.opacity, black, NSStringFromRect(NSRectFromCGRect(dim.frame)),
                                              amount]);
    }
}

static void theSessionsWindowAloneIsTheOnlyLayer(void) {
    expect_order(__func__, @[ @"W" ]);
    clean();
}

/* c4's surfaces as before: below the session's window for a negative z and above it for a positive one, by z. */
static void surfacesLieAroundTheSessionsWindowByZ(void) {
    surface(11, 2);
    surface(12, -1);
    surface(13, 1);
    surface(14, -2);
    expect_order(__func__, @[ @"s14", @"s12", @"W", @"s13", @"s11" ]);
    clean();
}

/* A dialog lies above its activity's window with that window's on-top surfaces, and its popup above it. */
static void aDialogAndItsPopupLieAboveTheWindowAndItsSurfaces(void) {
    surface(11, 1);
    window(7, APPLICATION, 0, 0, 0);
    window(8, PANEL, 7, 0, 0);
    surface(12, 2);
    expect_order(__func__, @[ @"W", @"s11", @"s12", @"w7", @"w8" ]);
    clean();
}

/* A second dialog lies above the first one and its popup. */
static void aNewerDialogLiesAboveTheOlderOneAndItsPopup(void) {
    window(7, APPLICATION, 0, 0, 0);
    window(8, PANEL, 7, 0, 0);
    window(9, APPLICATION, 0, 0, 0);
    expect_order(__func__, @[ @"W", @"w7", @"w8", @"w9" ]);
    clean();
}

/* A translucent activity's window starts a group above, with its own dialog; a dialog of group 0 made after them
   still lies below them. */
static void groupsLieByTheirKeyNotByMakeOrder(void) {
    window(20, BASE, 0, 1, 0);
    window(21, APPLICATION, 0, 1, 0);
    window(7, APPLICATION, 0, 0, 0);
    expect_order(__func__, @[ @"W", @"w7", @"w20", @"w21" ]);
    clean();
}

/* In a group the base window lies lowest and a starting window highest, whatever their make order. */
static void aGroupsBaseWindowLiesLowestAndItsStartingWindowHighest(void) {
    window(30, STARTING, 0, 2, 0);
    window(31, APPLICATION, 0, 2, 0);
    window(32, BASE, 0, 2, 0);
    expect_order(__func__, @[ @"W", @"w32", @"w31", @"w30" ]);
    clean();
}

/* Toasts lie above every group, in make order. */
static void toastsLieAboveEveryGroup(void) {
    window(40, TOAST, 0, 0, 0);
    window(20, BASE, 0, 1, 0);
    window(41, TOAST, 0, 0, 0);
    expect_order(__func__, @[ @"W", @"w20", @"w40", @"w41" ]);
    clean();
}

/* A sub-window's sublayer by its type (WindowManagerPolicy.getSubWindowLayerFromTypeLw), 0 for an unknown one:
   the session's window's sub-windows, made in another order. */
static void subWindowsTakeTheirTypesSublayer(void) {
    window(51, ABOVE_SUB_PANEL, 0, 0, 0);
    window(52, PANEL, 0, 0, 0);
    window(53, MEDIA, 0, 0, 0);
    window(54, 1999, 0, 0, 0);
    window(55, SUB_PANEL, 0, 0, 0);
    window(56, MEDIA_OVERLAY, 0, 0, 0);
    window(57, ATTACHED_DIALOG, 0, 0, 0);
    expect_order(__func__, @[ @"w53", @"w56", @"W", @"w54", @"w52", @"w57", @"w55", @"w51" ]);
    clean();
}

/* A SurfaceView in a translucent activity's window: its media sub-window below the window, a panel one above, and
   a sublayer of 0 above the window too. */
static void aSurfaceViewsSubWindowsLieAroundTheirWindow(void) {
    window(20, BASE, 0, 1, 0);
    window(22, PANEL, 20, 0, 0);
    window(23, MEDIA, 20, 0, 0);
    window(24, 1999, 20, 0, 0);
    expect_order(__func__, @[ @"W", @"w23", @"w20", @"w24", @"w22" ]);
    clean();
}

/* The session's window's sub-windows mix with c4's surfaces by sublayer, then newer above. */
static void theSessionsSubWindowsMixWithItsSurfaces(void) {
    surface(11, 1);
    window(8, PANEL, 0, 0, 0);
    surface(12, 2);
    surface(13, 1);
    window(9, MEDIA, 0, 0, 0);
    surface(14, -2);
    expect_order(__func__, @[ @"w9", @"s14", @"W", @"s11", @"w8", @"s13", @"s12" ]);
    clean();
}

/* One dim, below the topmost window that dims and below that window's sub-windows, at that window's amount; once
   that window has gone, the dim lies below the next one down, at its amount. */
static void theDimLiesBelowTheTopmostDimmingWindowOnlyAndMovesDown(void) {
    window(7, APPLICATION, 0, 0, 0.6f);
    window(20, BASE, 0, 1, 0);
    window(21, APPLICATION, 0, 1, 0.8f);
    window(25, MEDIA, 21, 0, 0);
    expect_order(__func__, @[ @"W", @"w7", @"w20", @"dim", @"w25", @"w21" ]);
    expect_dim(__func__, 0.8f);
    removed(@"w25");
    removed(@"w21");
    expect_order(__func__, @[ @"W", @"dim", @"w7", @"w20" ]);
    expect_dim(__func__, 0.6f);
    removed(@"w7");
    expect_order(__func__, @[ @"W", @"w20" ]);
    clean();
}

static void noWindowDimsNoDim(void) {
    window(7, APPLICATION, 0, 0, 0);
    expect_order(__func__, @[ @"W", @"w7" ]);
    clean();
}

/* A window's layer lies at its frame, opaque as its request says, with its alpha as its opacity; a place moves it,
   and its dim follows the place's. */
static void aWindowTakesItsFrameOpacityAndAlphaAndAPlaceMovesThem(void) {
    struct apkrt_window_request request = window_request(7, APPLICATION, 0, 0, 0);
    request.left = 63;
    request.top = 1032;
    request.right = 1227;
    request.bottom = 1764;
    request.width = 1164;
    request.height = 732;
    request.alpha = bits(0.5f);
    numbers[@"w7"] = @(++windows_made);
    keep(@"w7", hooks->requested_layer(&request));
    CALayer *layer = layers[@"w7"];
    minato_test_session_settle();
    if (!CGRectEqualToRect(layer.frame, CGRectMake(63, 1032, 1164, 732)) || layer.opaque ||
        fabsf(layer.opacity - 0.5f) > 1e-6f) {
        fail(__func__, [NSString stringWithFormat:@"the layer is at %@, opaque %d, opacity %.3f",
                                                  NSStringFromRect(NSRectFromCGRect(layer.frame)), layer.opaque,
                                                  layer.opacity]);
    }
    request.op = APKRT_WINDOW_PLACE;
    request.left = 0;
    request.top = 0;
    request.right = 645;
    request.bottom = 1398;
    request.alpha = bits(1);
    request.dim = bits(0.6f);
    hooks->requested_placed(&request);
    expect_order(__func__, @[ @"W", @"dim", @"w7" ]);
    expect_dim(__func__, 0.6f);
    if (!CGRectEqualToRect(layer.frame, CGRectMake(0, 0, 645, 1398)) || fabsf(layer.opacity - 1) > 1e-6f) {
        fail(__func__, [NSString stringWithFormat:@"after the place the layer is at %@, opacity %.3f",
                                                  NSStringFromRect(NSRectFromCGRect(layer.frame)), layer.opacity]);
    }
    request.dim = bits(0);
    hooks->requested_placed(&request);
    expect_order(__func__, @[ @"W", @"w7" ]);
    clean();
}

/* The window host's lines (MINATO_WINDOW_WATCH=hidden), from a pipe in the place of stderr: each window as it comes
   and at each place that changes it, the tree from the bottom whenever it changes, and each window as it goes. */
static void theLinesSayWhatComesAndGoes(void) {
    char path[] = "/tmp/session-layers-XXXXXX";
    const int file = mkstemp(path);
    fflush(stderr);
    const int saved = dup(STDERR_FILENO);
    dup2(file, STDERR_FILENO);

    surface(11, -2);
    struct apkrt_window_request request = window_request(7, APPLICATION, 0, 0, 0.6f);
    request.left = 63;
    request.top = 1032;
    request.right = 1227;
    request.bottom = 1764;
    numbers[@"w7"] = @(++windows_made);
    keep(@"w7", hooks->requested_layer(&request));
    request.op = APKRT_WINDOW_PLACE;
    request.alpha = bits(0.5f);
    hooks->requested_placed(&request);
    hooks->requested_placed(&request);  // nothing changes, nothing is printed
    window(8, PANEL, 7, 0, 0);
    clean();

    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);
    NSString *printed = [NSString stringWithContentsOfFile:@(path) encoding:NSUTF8StringEncoding error:NULL];
    if (printed == nil) {
        printed = @"";
    }
    close(file);
    unlink(path);
    const unsigned long s11 = numbers[@"s11"].unsignedLongValue, w7 = numbers[@"w7"].unsignedLongValue,
                        w8 = numbers[@"w8"].unsignedLongValue;
    NSArray<NSString *> *expected = @[
        @"window host: a surface below the window at 0, 0, 100 x 100",
        [NSString stringWithFormat:@"window host: from the bottom: surface %lu, the window", s11],
        [NSString stringWithFormat:@"window host: window %lu, type 2: at 63, 1032, 1164 x 732, alpha 1.00, dim 0.60, "
                                   @"not opaque", w7],
        [NSString stringWithFormat:@"window host: from the bottom: surface %lu, the window, dim, window %lu", s11, w7],
        [NSString stringWithFormat:@"window host: window %lu, type 2: at 63, 1032, 1164 x 732, alpha 0.50, dim 0.60, "
                                   @"not opaque", w7],
        [NSString stringWithFormat:@"window host: window %lu, type 1000: at 0, 0, 1290 x 2796, alpha 1.00, "
                                   @"dim 0.00, not opaque", w8],
        [NSString stringWithFormat:@"window host: from the bottom: surface %lu, the window, dim, window %lu, "
                                   @"window %lu", s11, w7, w8],
        [NSString stringWithFormat:@"window host: window %lu goes", w8],
        [NSString stringWithFormat:@"window host: from the bottom: surface %lu, the window, dim, window %lu", s11, w7],
        [NSString stringWithFormat:@"window host: window %lu goes", w7],
        [NSString stringWithFormat:@"window host: from the bottom: surface %lu, the window", s11],
        @"window host: a surface goes",
        @"window host: from the bottom: the window",
    ];
    NSMutableArray<NSString *> *lines = [[printed componentsSeparatedByString:@"\n"] mutableCopy];
    [lines removeObject:@""];
    if (![lines isEqualToArray:expected]) {
        fail(__func__, [NSString stringWithFormat:@"the lines are\n%@\nnot\n%@", [lines componentsJoinedByString:@"\n"],
                                                  [expected componentsJoinedByString:@"\n"]]);
    }
}

int main(void) {
    @autoreleasepool {
        setenv("MINATO_SESSION_LAYERS", "1", 1);
        setenv("MINATO_WINDOW_WATCH", "hidden", 1);
        hooks = apk_run_layers();
        if (hooks == NULL || hooks->requested_layer == NULL || hooks->requested_placed == NULL ||
            hooks->requested_removed == NULL) {
            fprintf(stderr, "FAILED  the session layers have no window hooks\n");
            return 1;
        }
        names = [NSMapTable mapTableWithKeyOptions:NSPointerFunctionsObjectPointerPersonality
                                      valueOptions:NSPointerFunctionsStrongMemory];
        made = [NSMutableArray array];
        layers = [NSMutableDictionary dictionary];
        numbers = [NSMutableDictionary dictionary];
        session_window = (__bridge CALayer *)hooks->window_layer(WIDTH, HEIGHT);
        if (!CGRectEqualToRect(minato_test_session_root().bounds, CGRectMake(0, 0, WIDTH, HEIGHT))) {
            fail("the root", @"not at the session's size");
        }
        theLinesSayWhatComesAndGoes();
        theSessionsWindowAloneIsTheOnlyLayer();
        surfacesLieAroundTheSessionsWindowByZ();
        aDialogAndItsPopupLieAboveTheWindowAndItsSurfaces();
        aNewerDialogLiesAboveTheOlderOneAndItsPopup();
        groupsLieByTheirKeyNotByMakeOrder();
        aGroupsBaseWindowLiesLowestAndItsStartingWindowHighest();
        toastsLieAboveEveryGroup();
        subWindowsTakeTheirTypesSublayer();
        aSurfaceViewsSubWindowsLieAroundTheirWindow();
        theSessionsSubWindowsMixWithItsSurfaces();
        theDimLiesBelowTheTopmostDimmingWindowOnlyAndMovesDown();
        noWindowDimsNoDim();
        aWindowTakesItsFrameOpacityAndAlphaAndAPlaceMovesThem();
        if (failures > 0) {
            fprintf(stderr, "%d failed\n", failures);
            return 1;
        }
        printf("session layers: 14 cases passed\n");
    }
    return 0;
}
