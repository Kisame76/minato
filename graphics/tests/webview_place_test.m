// ADR-0056 c2 (test 3's first half): graphics/host's WebViews placed in their window's view tree (webview.m) through
// the runtime's handler, without the runtime: at PLACE a clip view at the clip rectangle, just below the window's own
// view, with the WKWebView inside at the full frame, in points; a PLACE before the view's "make" waits for it; a
// window the host does not show and an empty clip hide the view; REMOVE takes it out. The host hears how many views
// each window shows, as its layer must let them through. The hit test gives the page the points of its last
// touchRegion (piece 2), and every other point to the window: none before the first. A page's viewport of
// "width=device-width" gets the view's width in CSS pixels, as Android's WebView gives it, not the screen's; and while
// useWideViewPort is off, its default, every page lays out at the view's width, one without a viewport tag too.
#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>

#include "APKRuntime/apkrt.h"
#include "minato_graphics.h"

#include <stdio.h>
#include <string.h>

@interface FlippedView : NSView
@end
@implementation FlippedView
- (BOOL)isFlipped {
    return YES;
}
@end

/// The window's view as the app's is: it takes no touches (its hit test skips it), the session's view does.
@interface WindowView : FlippedView
@end
@implementation WindowView
- (NSView *)hitTest:(NSPoint)point {
    (void)point;
    return nil;
}
@end

static int failures;
static NSView *root;
static NSView *container;
static NSView *window_view;
static uint32_t shown[16];
static int shown_calls;

#define CHECK(condition, ...)                                                                                          \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            fprintf(stderr, "FAIL line %d: ", __LINE__);                                                               \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fprintf(stderr, "\n");                                                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void *on_container(void *context, uint64_t window, void **above, double *scale) {
    (void)context;
    if (window != 7) return NULL;
    *above = (__bridge void *)window_view;
    *scale = 3;
    return (__bridge void *)container;
}

static void on_shown(void *context, uint64_t window, uint32_t count) {
    (void)context;
    if (window < 16) shown[window] = count;
    shown_calls++;
}

/// Lets the main queue carry out what the handler handed it.
static void settle(void) {
    __block BOOL done = NO;
    dispatch_async(dispatch_get_main_queue(), ^{
        done = YES;
    });
    while (!done) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
}

static void request(uint32_t op, uint64_t token, uint64_t window, const int32_t frame[4], const int32_t clip[4],
                    const char *payload) {
    struct apkrt_webview_call call;
    memset(&call, 0, sizeof call);
    call.op = op;
    call.token = token;
    call.window = window;
    if (frame != NULL) {
        call.left = frame[0], call.top = frame[1], call.right = frame[2], call.bottom = frame[3];
        call.clip_left = clip[0], call.clip_top = clip[1], call.clip_right = clip[2], call.clip_bottom = clip[3];
    }
    call.payload = payload;
    call.payload_length = payload != NULL ? (uint32_t)strlen(payload) : 0;
    call.fd = -1;
    minato_webview_handle(NULL, &call);
    settle();
}

static void make(uint64_t token) {
    request(APKRT_WEBVIEW_CALL, token, 0, NULL, NULL,
            "{\"call\":\"make\",\"schemes\":[\"minato-asset\"],\"settings\":{}}");
}

/// The clip view of the WKWebView in the container, or nil.
static NSView *clip_of_web_view(void) {
    for (NSView *view in container.subviews) {
        if (view.subviews.count == 1 && [view.subviews[0] isKindOfClass:WKWebView.class]) return view;
    }
    return nil;
}

/// The result of `script` in the page of the WKWebView in the container, once it has loaded, as a string.
static NSString *page_value(NSString *script) {
    WKWebView *page = (WKWebView *)clip_of_web_view().subviews[0];
    for (int i = 0; i < 500 && page.isLoading; i++) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
    __block NSString *value = nil;
    __block BOOL done = NO;
    [page evaluateJavaScript:script
           completionHandler:^(id result, NSError *error) {
               value = error == nil ? [NSString stringWithFormat:@"%@", result] : error.localizedDescription;
               done = YES;
           }];
    for (int i = 0; i < 500 && !done; i++) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
    return value;
}

/// Loads `html` in view `token`, as loadDataWithBaseURL without a base.
static void load_page(uint64_t token, const char *html) {
    NSString *call = [NSString
        stringWithFormat:@"{\"call\":\"loadDataWithBaseURL\",\"data\":\"%s\",\"mimeType\":\"text/html\"}", html];
    request(APKRT_WEBVIEW_CALL, token, 0, NULL, NULL, call.UTF8String);
}

/// What the hit test gives `point`, in the container's points, as AppKit asks it: from the root, in its superview's.
static NSView *container_hit(NSPoint point) {
    return [root hitTest:[root convertPoint:point fromView:container]];
}

/// Whether `view` is the page or inside it.
static BOOL in_page(NSView *view) {
    for (; view != nil; view = view.superview) {
        if ([view isKindOfClass:WKWebView.class]) return YES;
    }
    return NO;
}

static BOOL same(NSRect a, NSRect b) {
    return NSEqualRects(a, b);
}

int main(void) {
    @autoreleasepool {
        root = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 400, 900)];
        container = [[FlippedView alloc] initWithFrame:root.bounds];
        [root addSubview:container];
        window_view = [[WindowView alloc] initWithFrame:container.bounds];
        [container addSubview:window_view];
        static const struct minato_webview_windows windows = {NULL, on_container, on_shown};
        minato_webview_set_windows(&windows);

        // MAKE, "make", then PLACE: the clip at the clip rectangle below the window's view, the page at the frame.
        const int32_t frame[4] = {30, 60, 330, 660}, clip[4] = {30, 90, 330, 600};
        request(APKRT_WEBVIEW_MAKE, 1, 0, NULL, NULL, NULL);
        make(1);
        request(APKRT_WEBVIEW_PLACE, 1, 7, frame, clip, NULL);
        NSView *holder = clip_of_web_view();
        CHECK(holder != nil, "no clip view with the WKWebView in the window's container after PLACE");
        if (holder != nil) {
            NSArray *order = container.subviews;
            CHECK(order.count == 2 && order[0] == holder && order[1] == window_view,
                  "the clip view is not just below the window's view");
            CHECK(same(holder.frame, NSMakeRect(10, 30, 100, 170)), "clip frame %s, want {10,30,100,170}",
                  NSStringFromRect(holder.frame).UTF8String);
            CHECK(same(holder.subviews[0].frame, NSMakeRect(0, -10, 100, 200)), "page frame %s, want {0,-10,100,200}",
                  NSStringFromRect(holder.subviews[0].frame).UTF8String);
            CHECK(holder.isFlipped, "the clip view counts y from its bottom");
            CHECK(holder.clipsToBounds, "the clip view does not clip");
            CHECK(!holder.hidden, "the clip view is hidden");
        }
        CHECK(shown[7] == 1 && shown_calls == 1, "window 7 shows %u views after %d calls, want 1 after 1", shown[7],
              shown_calls);

        // The touch region: the page takes the points inside its rectangles, the window's view the rest.
        CHECK(container_hit(NSMakePoint(50, 100)) == container, "a point before any touchRegion reaches the page");
        request(APKRT_WEBVIEW_CALL, 1, 0, NULL, NULL,
                "{\"call\":\"touchRegion\",\"rects\":[[30,90,330,300],[30,300,180,600]]}");
        CHECK(in_page(container_hit(NSMakePoint(50, 50))), "a point in the first rectangle does not reach the page");
        CHECK(in_page(container_hit(NSMakePoint(20, 150))), "a point in the second rectangle does not reach the page");
        CHECK(container_hit(NSMakePoint(90, 150)) == container, "a point outside the region reaches the page");
        CHECK(container_hit(NSMakePoint(5, 5)) == container, "a point outside the clip reaches the page");
        request(APKRT_WEBVIEW_CALL, 1, 0, NULL, NULL, "{\"call\":\"touchRegion\",\"rects\":[]}");
        CHECK(container_hit(NSMakePoint(50, 50)) == container, "an empty touchRegion leaves the page its points");

        // The viewport: device-width is the view's width, 100 points, also after a load and a new width.
        request(APKRT_WEBVIEW_CALL, 1, 0, NULL, NULL,
                "{\"call\":\"loadDataWithBaseURL\",\"data\":\"<meta name='viewport' "
                "content='width=device-width, initial-scale=1'><p>page</p>\",\"mimeType\":\"text/html\"}");
        NSString *viewport = @"document.querySelector('meta[name=viewport]').content";
        NSString *got = page_value(viewport);
        CHECK([got isEqualToString:@"width=100, initial-scale=1"], "viewport %s, want width=100", got.UTF8String);

        // A new frame moves it; the count stays.
        const int32_t moved[4] = {0, 0, 300, 300};
        request(APKRT_WEBVIEW_PLACE, 1, 7, moved, moved, NULL);
        holder = clip_of_web_view();
        CHECK(holder != nil && same(holder.frame, NSMakeRect(0, 0, 100, 100)) &&
                  same(holder.subviews[0].frame, NSMakeRect(0, 0, 100, 100)),
              "the moved view is not at {0,0,100,100}");
        CHECK(shown_calls == 1, "a move told the host the count again");
        got = page_value(viewport);
        CHECK([got isEqualToString:@"width=100, initial-scale=1"], "viewport %s after the move, want width=100",
              got.UTF8String);
        const int32_t wide[4] = {0, 0, 600, 300};
        request(APKRT_WEBVIEW_PLACE, 1, 7, wide, wide, NULL);
        got = page_value(viewport);
        CHECK([got isEqualToString:@"width=200, initial-scale=1"], "viewport %s at 200 points, want width=200",
              got.UTF8String);
        request(APKRT_WEBVIEW_PLACE, 1, 7, moved, moved, NULL);
        NSString *any = @"(document.querySelector('meta[name=viewport]') || {content: 'none'}).content";
        load_page(1, "<p>no viewport tag</p>");
        got = page_value(any);
        CHECK([got isEqualToString:@"width=100"], "viewport %s without a tag, want width=100", got.UTF8String);
        load_page(1, "<meta name='viewport' content='width=980, initial-scale=0.5'><p>page</p>");
        got = page_value(any);
        CHECK([got isEqualToString:@"width=100, initial-scale=0.5"], "viewport %s, want width=100, initial-scale=0.5",
              got.UTF8String);
        // With useWideViewPort the page's own width stands, and a page without a tag has WebKit's.
        request(APKRT_WEBVIEW_CALL, 1, 0, NULL, NULL,
                "{\"call\":\"settings\",\"settings\":{\"wideViewPort\":true}}");
        load_page(1, "<meta name='viewport' content='width=980, initial-scale=0.5'><p>page</p>");
        got = page_value(any);
        CHECK([got isEqualToString:@"width=980, initial-scale=0.5"], "viewport %s with useWideViewPort, want 980",
              got.UTF8String);
        load_page(1, "<p>no viewport tag</p>");
        got = page_value(any);
        CHECK([got isEqualToString:@"none"], "viewport %s without a tag with useWideViewPort, want none",
              got.UTF8String);

        // An empty clip hides the view, a clip again shows it.
        const int32_t empty[4] = {0, 0, 0, 0};
        request(APKRT_WEBVIEW_PLACE, 1, 7, moved, empty, NULL);
        CHECK(clip_of_web_view() != nil && clip_of_web_view().hidden, "an empty clip leaves the view shown");
        CHECK(shown[7] == 0, "window 7 shows %u views with an empty clip, want 0", shown[7]);
        request(APKRT_WEBVIEW_PLACE, 1, 7, moved, moved, NULL);
        CHECK(clip_of_web_view() != nil && !clip_of_web_view().hidden && shown[7] == 1, "the view did not come back");

        // A window the host does not show takes the view out of the container.
        request(APKRT_WEBVIEW_PLACE, 1, 9, moved, moved, NULL);
        CHECK(clip_of_web_view() == nil, "a view of an unknown window stays in window 7's container");
        CHECK(shown[7] == 0, "window 7 shows %u views after its view left, want 0", shown[7]);

        // A PLACE before "make" waits for the view.
        request(APKRT_WEBVIEW_MAKE, 2, 0, NULL, NULL, NULL);
        request(APKRT_WEBVIEW_PLACE, 2, 7, frame, clip, NULL);
        CHECK(clip_of_web_view() == nil, "a view before its make is in the container");
        make(2);
        holder = clip_of_web_view();
        CHECK(holder != nil && same(holder.frame, NSMakeRect(10, 30, 100, 170)), "the place before make was lost");
        CHECK(shown[7] == 1, "window 7 shows %u views, want 1", shown[7]);

        // REMOVE takes it out.
        request(APKRT_WEBVIEW_REMOVE, 2, 0, NULL, NULL, NULL);
        CHECK(clip_of_web_view() == nil, "a removed view stays in the container");
        CHECK(shown[7] == 0, "window 7 shows %u views after REMOVE, want 0", shown[7]);
        request(APKRT_WEBVIEW_REMOVE, 1, 0, NULL, NULL, NULL);
        CHECK(container.subviews.count == 1, "the container keeps %lu views, want the window's alone",
              (unsigned long)container.subviews.count);
    }
    if (failures == 0) printf("webview place: ok\n");
    return failures == 0 ? 0 : 1;
}
