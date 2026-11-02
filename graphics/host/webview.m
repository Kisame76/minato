// The guest's WebViews on WKWebView (ADR-0056 step c1), for the app and graphics-run: the runtime's WebView handler
// (apkrt_session.webview), which takes each request of the guest's provider at once and carries it out on the main
// queue, and the records it posts back (apkrt_webview_post), in the payloads of step b2's table and in the order
// Chromium gives them where webview-test pins it.
// - A view is made at the CALL "make", which carries what WKWebViewConfiguration fixes: the package's data store
//   (WKWebsiteDataStore by identifier), the scheme of the app's assets and the first settings.
// - The scheme handler asks the guest for each minato-asset:// request with a RESOURCE record that waits; the guest
//   answers its head (JSON), its body in inline chunks or a descriptor, and an empty answer at the end. A late answer
//   for a task WebKit stopped is dropped; the view's removal fails its open tasks.
// - History as Chromium keeps it, by the host itself, since WebKit's back-forward list replaces its current entry for
//   loadHTMLString and loadData: a page of loadDataWithBaseURL is the entry "data:text/html;charset=utf-8;base64,"
//   at the base URL (about:blank without one), and a failed main-frame load commits Chromium's error page ("Webpage
//   not available") at the failed URL after its error and onPageFinished. Going back or forward loads the entry
//   again (a named difference: Chromium shows its cached page). A title comes after its page's commit, as Chromium's.
// - Answers the guest owes (a dialog, a resource's head) wait up to 10 s; then the default goes on: a closed dialog,
//   a failed resource.
// - Views (c2): at PLACE the WKWebView goes into a clip view at the clip rectangle, just below its window's own view in
//   the host's tree (minato_webview_set_windows), at the full frame, in points; the host hears how many views each
//   window shows, as its layer must let them through where the guest cleared its pixels. The hit test gives the
//   page the points of its last CALL "touchRegion" (rectangles in the window's pixels, the WebView's visible part
//   less the guest's views over it): scrolling, links and text fields; all other points stay the guest's.
//   A page's viewport "device-width" is the view's width in CSS pixels, as Android's WebView makes it, where WebKit
//   would take the screen's: a user script rewrites the main frame's viewport meta tags, again at each new width.
//   While useWideViewPort is off (Android's default) every page lays out at the view's width, as Android's does: the
//   script sets the tags' width, or adds a tag to a page without one.
// - Cookies (c3): each view, and the guest's CookieManager on a channel of its own (a "make" with "cookies", no view),
//   observes the package's store and posts its whole list (COOKIES) after each change and, for a view, before each
//   "finished", so onPageFinished's getCookie sees the page's cookies; setCookie and removeAllCookies are CALLs,
//   answered once the list after them has gone out.
// Not here (later steps): shouldOverrideUrlLoading, the JavaScript bridge, new windows, downloads and permissions.
#import <Foundation/Foundation.h>
#import <WebKit/WebKit.h>

#include "APKRuntime/apkrt.h"
#include "minato_graphics.h"

#include <unistd.h>

static const int64_t kAnswerWait = 10 * (int64_t)NSEC_PER_SEC;  // the guest's answer, then Android's default

/// `value`, or `fallback` where it is nil.
static id either(id value, id fallback) {
    return value != nil ? value : fallback;
}

/// The record ids of the host's records that wait for an ANSWER, apart from the guest's CALL ids.
static uint64_t next_record_id(void) {
    static uint64_t next = 1ull << 32;
    return __atomic_fetch_add(&next, 1, __ATOMIC_RELAXED);
}

/// `text` as JSON's string, escaped as Chromium's EscapeJSONString escapes it.
static void append_json_string(NSMutableString *out, NSString *text) {
    [out appendString:@"\""];
    const NSUInteger length = text.length;
    for (NSUInteger i = 0; i < length; i++) {
        const unichar c = [text characterAtIndex:i];
        switch (c) {
            case '"': [out appendString:@"\\\""]; break;
            case '\\': [out appendString:@"\\\\"]; break;
            case '\b': [out appendString:@"\\b"]; break;
            case '\f': [out appendString:@"\\f"]; break;
            case '\n': [out appendString:@"\\n"]; break;
            case '\r': [out appendString:@"\\r"]; break;
            case '\t': [out appendString:@"\\t"]; break;
            case '<': [out appendString:@"\\u003C"]; break;
            case 0x2028: [out appendString:@"\\u2028"]; break;
            case 0x2029: [out appendString:@"\\u2029"]; break;
            default:
                if (c < 0x20) {
                    [out appendFormat:@"\\u%04X", c];
                } else {
                    [out appendFormat:@"%C", c];
                }
        }
    }
    [out appendString:@"\""];
}

/// A script's value as evaluateJavascript gives it on Android: JSON as Chromium writes it, null for undefined.
static void append_json_value(NSMutableString *out, id value) {
    if (value == nil || value == NSNull.null) {
        [out appendString:@"null"];
    } else if ([value isKindOfClass:NSNumber.class]) {
        if (CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID()) {
            [out appendString:[value boolValue] ? @"true" : @"false"];
        } else {
            const double number = [value doubleValue];
            if (!isfinite(number)) {
                [out appendString:@"null"];
            } else {
                NSData *json = [NSJSONSerialization dataWithJSONObject:value options:NSJSONWritingFragmentsAllowed
                                                                 error:nil];
                [out appendString:either([[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding], @"null")];
            }
        }
    } else if ([value isKindOfClass:NSString.class]) {
        append_json_string(out, value);
    } else if ([value isKindOfClass:NSArray.class]) {
        [out appendString:@"["];
        BOOL first = YES;
        for (id item in (NSArray *)value) {
            if (!first) [out appendString:@","];
            first = NO;
            append_json_value(out, item);
        }
        [out appendString:@"]"];
    } else if ([value isKindOfClass:NSDictionary.class]) {
        [out appendString:@"{"];
        BOOL first = YES;
        for (id key in [[(NSDictionary *)value allKeys] sortedArrayUsingSelector:@selector(compare:)]) {
            if (!first) [out appendString:@","];
            first = NO;
            append_json_string(out, [key description]);
            [out appendString:@":"];
            append_json_value(out, ((NSDictionary *)value)[key]);
        }
        [out appendString:@"}"];
    } else {
        [out appendString:@"{}"];  // a Date or another object without a JSON form
    }
}

/// A record's payload, a JSON object.
static NSData *json_data(NSDictionary *object) {
    return either([NSJSONSerialization dataWithJSONObject:object options:NSJSONWritingWithoutEscapingSlashes error:nil],
                  [NSData dataWithBytes:"{}" length:2]);
}

static void post(uint64_t token, uint32_t type, uint64_t request_id, NSDictionary *object) {
    NSData *payload = json_data(object);
    apkrt_webview_post(token, type, request_id, payload.bytes, (uint32_t)payload.length);
}

/// The host's own world for its scripts, which the page cannot see.
static WKContentWorld *host_world(void) {
    return [WKContentWorld worldWithName:@"minato"];
}

/// A script of the host's world, at each document's start in the main frame: it reports the title as the parser sets
/// it, before the page's next script runs. WebKit's own title comes only at its load's end, Chromium's at once.
static NSString *const kTitleScript =
    @"(function() {"
    @"  var last = null;"
    @"  function report() {"
    @"    var title = document.title;"
    @"    if (title && title !== last) { last = title; window.webkit.messageHandlers.minatoTitle.postMessage(title); }"
    @"  }"
    @"  new MutationObserver(report).observe(document, {subtree: true, childList: true, characterData: true});"
    @"})();";

/// A script of the page's world, at each document's start in every frame: it reports the page's console messages and
/// its uncaught errors as onConsoleMessage has them (Chromium's levels: info is a log, debug a tip; an uncaught error
/// "Uncaught <message>"), with the line and source of the call, from the stack's frame of the caller; WebKit's frame
/// leaves out the document's query, which Chromium's source has.
static NSString *const kConsoleScript =
    @"(function() {"
    @"  var handler = window.webkit.messageHandlers.minatoConsole;"
    @"  function report(level, message, source, line) {"
    @"    try { handler.postMessage({level: level, message: message, source: source, line: line}); } catch (e) {}"
    @"  }"
    @"  var levels = {log: 'log', info: 'log', warn: 'warning', error: 'error', debug: 'tip'};"
    @"  Object.keys(levels).forEach(function(name) {"
    @"    var original = console[name];"
    @"    console[name] = function() {"
    @"      var frame = /@(.*):(\\d+):\\d+$/.exec((new Error().stack || '').split('\\n')[1] || '');"
    @"      var source = frame ? frame[1] : location.href;"
    @"      if (source === location.href.split(/[?#]/)[0]) source = location.href;"
    @"      report(levels[name], Array.prototype.map.call(arguments, String).join(' '), source, frame ? +frame[2] : 0);"
    @"      return original.apply(console, arguments);"
    @"    };"
    @"  });"
    @"  window.addEventListener('error', function(event) {"
    @"    report('error', 'Uncaught ' + event.message, event.filename || location.href, event.lineno || 0);"
    @"  });"
    @"})();";

/// The app's objects (addJavascriptInterface), in the page's world of every frame at each document's start: each
/// method calls the app at once through prompt(), which the host answers from the guest's JS_BRIDGE answer
/// {"value"} or {"error"}, thrown as an Error. The `%@` is the objects, {name: [method, ...]}.
static NSString *const kBridgeScript =
    @"(function(objects) {"
    @"  Object.keys(objects).forEach(function(name) {"
    @"    var object = {};"
    @"    objects[name].forEach(function(method) {"
    @"      object[method] = function() {"
    @"        var answer = prompt('%@' + JSON.stringify({object: name, method: method,"
    @"                                                     args: Array.prototype.slice.call(arguments)}));"
    @"        if (answer === null) return undefined;"
    @"        answer = JSON.parse(answer);"
    @"        if (answer.error !== undefined) throw new Error(answer.error);"
    @"        return answer.value;"
    @"      };"
    @"    });"
    @"    window[name] = object;"
    @"  });"
    @"})(%@);";
/// The start of a prompt that is a call of the app's objects.
static NSString *const kBridgePrompt = @"minato-bridge:";

/// Chromium's error page, which a failed main-frame load commits at its URL.
/// Rewrites the main frame's viewport meta tags for the view's width (the first `%@`), now and as the page adds or
/// changes them: with useWideViewPort (the second, true or false) only "device-width", without it the tags' width, and
/// a page without a tag gets one. The page's own value is kept, so a new width rewrites it again. In the host's world.
static NSString *const kViewportScript =
    @"(() => {"
     "  const state = window.minatoViewport || (window.minatoViewport = {metas: new Map(), observer: null});"
     "  state.width = '%@';"
     "  state.wide = %@;"
     "  const narrow = (own) => /(^|[,;\\s])width\\s*=/i.test(own)"
     "    ? own.replace(/(^|[,;\\s])width\\s*=\\s*[^,;\\s]*/i, '$1width=' + state.width)"
     "    : (own ? own + ', ' : '') + 'width=' + state.width;"
     "  const fix = (meta) => {"
     "    if ((meta.getAttribute('name') || '').toLowerCase() !== 'viewport' || meta === state.added) return;"
     "    const now = meta.getAttribute('content') || '';"
     "    let known = state.metas.get(meta);"
     "    if (!known || now !== known.applied) known = {own: now};"
     "    known.applied = state.wide ? known.own.replace(/device-width/gi, state.width) : narrow(known.own);"
     "    state.metas.set(meta, known);"
     "    if (known.applied !== now) meta.setAttribute('content', known.applied);"
     "  };"
     "  const all = () => {"
     "    document.querySelectorAll('meta').forEach(fix);"
     "    const own = [...document.querySelectorAll('meta')].some((meta) => meta !== state.added"
     "      && (meta.getAttribute('name') || '').toLowerCase() === 'viewport');"
     "    if (state.wide || own) {"
     "      if (state.added) state.added.remove();"
     "      state.added = null;"
     "    } else if (document.head) {"
     "      if (!state.added) {"
     "        state.added = document.createElement('meta');"
     "        state.added.setAttribute('name', 'viewport');"
     "      }"
     "      if (state.added.getAttribute('content') !== 'width=' + state.width) {"
     "        state.added.setAttribute('content', 'width=' + state.width);"
     "      }"
     "      if (!state.added.isConnected) document.head.appendChild(state.added);"
     "    }"
     "  };"
     "  if (!state.observer) {"
     "    state.observer = new MutationObserver(all);"
     "    state.observer.observe(document, {childList: true, subtree: true, attributes: true,"
     "                                      attributeFilter: ['content', 'name']});"
     "  }"
     "  all();"
     "})();";

static NSString *const kErrorPage =
    @"<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width\">"
    @"<title>Webpage not available</title></head><body><h1>Webpage not available</h1></body></html>";

/// A history entry as Chromium keeps it.
@interface MinatoWebItem : NSObject
@property(nonatomic, copy) NSString *url;
@property(nonatomic, copy) NSString *original;
@property(nonatomic, copy) NSString *title;
@property(nonatomic, copy) NSString *viewURL;          // the view's URL while it shows the entry, if not `url`
@property(nonatomic, strong) NSURLRequest *request;    // how to load the entry again: a request,
@property(nonatomic, strong) NSData *data;             // or a page's bytes at a base URL
@property(nonatomic, copy) NSString *mime;
@property(nonatomic, copy) NSString *encoding;
@property(nonatomic, strong) NSURL *baseURL;
@property(nonatomic) BOOL error;                       // Chromium's error page for `request`
@end

@implementation MinatoWebItem
@end

/// A minato-asset:// request that the guest answers.
@interface MinatoResource : NSObject
@property(nonatomic, strong) id<WKURLSchemeTask> task;
@property(nonatomic) BOOL headed;   // the head came, and the response went to WebKit
@property(nonatomic) BOOL stopped;  // WebKit stopped the task, or it failed or finished: later answers are dropped
@end

@implementation MinatoResource
@end

@class MinatoWebView;

#if TARGET_OS_IPHONE
typedef UIView MinatoHostView;
#else
typedef NSView MinatoHostView;
#endif

/// The page's clip view, which takes the points of the touch region only, in its own points; on the Mac it counts y
/// from its top, as the guest's frames do.
@interface MinatoClipView : MinatoHostView
@property(nonatomic, copy) NSArray<NSValue *> *region;
@end

@implementation MinatoClipView

- (BOOL)inRegion:(CGPoint)point {
    for (NSValue *rect in self.region) {
#if TARGET_OS_IPHONE
        if (CGRectContainsPoint(rect.CGRectValue, point)) return YES;
#else
        if (CGRectContainsPoint(rect.rectValue, point)) return YES;
#endif
    }
    return NO;
}

#if TARGET_OS_IPHONE
- (BOOL)pointInside:(CGPoint)point withEvent:(UIEvent *)event {
    return [self inRegion:point] && [super pointInside:point withEvent:event];
}
#else
- (BOOL)isFlipped {
    return YES;
}

- (NSView *)hitTest:(NSPoint)point {
    // `point` is in the superview's points.
    return [self inRegion:[self convertPoint:point fromView:self.superview]] ? [super hitTest:point] : nil;
}
#endif

@end

static const struct minato_webview_windows *host_windows;

/// The count of views each window shows, by window, as its host last heard it.
static NSMutableDictionary<NSNumber *, NSNumber *> *shown_counts(void) {
    static NSMutableDictionary *all;
    if (all == nil) all = [NSMutableDictionary new];
    return all;
}

static void count_shown(uint64_t window, int change) {
    if (window == 0) return;
    const uint32_t count = (uint32_t)((int)shown_counts()[@(window)].unsignedIntValue + change);
    shown_counts()[@(window)] = @(count);
    if (host_windows != NULL && host_windows->shown != NULL) host_windows->shown(host_windows->context, window, count);
}

/// The views by token, and the WKWebViews' delegates' way back to theirs, on the main queue only.
static NSMutableDictionary<NSNumber *, MinatoWebView *> *views(void) {
    static NSMutableDictionary *all;
    if (all == nil) all = [NSMutableDictionary new];
    return all;
}

/// One guest WebView.
@interface MinatoWebView : NSObject <WKNavigationDelegate, WKUIDelegate, WKURLSchemeHandler, WKScriptMessageHandler,
                                     WKHTTPCookieStoreObserver>
@property(nonatomic, strong) WKHTTPCookieStore *cookieStore;      // the package's store's, once made
@property(nonatomic) uint64_t token;
@property(nonatomic, strong) WKWebView *view;
@property(nonatomic, copy) NSDictionary *settings;
@property(nonatomic, copy) NSDictionary *interfaces;  // the app's objects for the next pages: {name: [method, ...]}
@property(nonatomic, strong) NSMutableDictionary *parts;         // a CALL that comes in parts, so far
@property(nonatomic, strong) NSMutableArray<MinatoWebItem *> *entries;  // the history
@property(nonatomic) NSInteger index;                            // the current entry, -1 before the first
@property(nonatomic) NSInteger target;                           // the entry a back or forward goes to, else -1
@property(nonatomic, strong) MinatoWebItem *pending;             // the entry of the load the host made, coming
@property(nonatomic) BOOL errorPage;                             // the page loading is the error page
@property(nonatomic) BOOL reload;                                // the navigation is a reload
@property(nonatomic) BOOL committing;                            // a main-frame load started and has not committed
@property(nonatomic, copy) NSString *heldTitle;                  // a title that came before its page's commit
@property(nonatomic, strong) NSMutableDictionary<NSNumber *, void (^)(NSData *, int, int64_t, int64_t)> *waiting;
@property(nonatomic, strong) NSMutableDictionary<NSNumber *, MinatoResource *> *resources;
@property(nonatomic) BOOL placed;                                // a PLACE came: window, frame and clip hold
@property(nonatomic) uint64_t window;
@property(nonatomic) CGRect frame;                               // in the window's pixels
@property(nonatomic) CGRect clip;
@property(nonatomic, strong) MinatoClipView *holder;             // the clip view, once placed
@property(nonatomic, copy) NSArray<NSValue *> *touchRegion;      // the last touchRegion, in the window's pixels
@property(nonatomic) uint64_t shownIn;                           // the window whose count has the view, or 0
@property(nonatomic) CGFloat cssWidth;                           // the viewport's device-width, 0 before a place
@property(nonatomic) BOOL wideViewPort;                          // the setting useWideViewPort
@end

@implementation MinatoWebView

- (instancetype)initWithToken:(uint64_t)token {
    if ((self = [super init])) {
        _token = token;
        _entries = [NSMutableArray new];
        _index = -1;
        _target = -1;
        _waiting = [NSMutableDictionary new];
        _resources = [NSMutableDictionary new];
    }
    return self;
}

// MARK: The guest's calls

/// The store of the data store id `identifier` (a UUID), or a store of the process alone for a bad one: one object per
/// id for the process, as two of the same id do not share their session cookies.
static WKWebsiteDataStore *data_store(id identifier) {
    static NSMutableDictionary<NSUUID *, WKWebsiteDataStore *> *stores;
    if (stores == nil) stores = [NSMutableDictionary new];
    NSUUID *store = [[NSUUID alloc] initWithUUIDString:[identifier isKindOfClass:NSString.class] ? identifier : @""];
    if (store == nil) return [WKWebsiteDataStore nonPersistentDataStore];
    if (stores[store] == nil) stores[store] = [WKWebsiteDataStore dataStoreForIdentifier:store];
    return stores[store];
}

/// The cookies of the store this view (or CookieManager's channel) has, observed from now on.
- (void)observeCookies:(WKWebsiteDataStore *)store {
    self.cookieStore = store.httpCookieStore;
    [self.cookieStore addObserver:self];
}

- (void)cookiesDidChangeInCookieStore:(WKHTTPCookieStore *)cookieStore {
    (void)cookieStore;
    [self postCookies:nil];
}

/// COOKIES: the store's whole list, then `then`. "created" is only an order: Created's ms since 2001, or the index.
- (void)postCookies:(void (^)(void))then {
    const uint64_t token = self.token;
    [self.cookieStore getAllCookies:^(NSArray<NSHTTPCookie *> *cookies) {
        NSMutableArray *list = [NSMutableArray new];
        NSUInteger index = 0;
        for (NSHTTPCookie *cookie in cookies) {
            id created = cookie.properties[@"Created"];
            [list addObject:@{
                @"name" : cookie.name,
                @"value" : cookie.value,
                @"domain" : [cookie.domain hasPrefix:@"."] ? [cookie.domain substringFromIndex:1] : cookie.domain,
                @"hostOnly" : [cookie.domain hasPrefix:@"."] ? @NO : @YES,
                @"path" : either(cookie.path, @"/"),
                @"secure" : @(cookie.isSecure),
                @"httpOnly" : @(cookie.isHTTPOnly),
                @"expires" : cookie.expiresDate != nil ? @((int64_t)(cookie.expiresDate.timeIntervalSince1970 * 1000))
                                                        : NSNull.null,
                @"created" : [created isKindOfClass:NSNumber.class] ? @((int64_t)([created doubleValue] * 1000))
                                                                      : @(index),
            }];
            index++;
        }
        post(token, APKRT_WEBVIEW_REC_COOKIES, 0, @{@"cookies" : list});
        if (then != nil) then();
    }];
}

/// setCookie: `value` as a Set-Cookie header at `url`; answered after the list it is in, true when WebKit took it.
- (void)setCookie:(NSDictionary *)call requestId:(uint64_t)requestId {
    NSURL *url = [call[@"url"] isKindOfClass:NSString.class] ? [NSURL URLWithString:call[@"url"]] : nil;
    NSString *value = [call[@"value"] isKindOfClass:NSString.class] ? call[@"value"] : nil;
    NSArray<NSHTTPCookie *> *cookies = url != nil && value != nil
        ? [NSHTTPCookie cookiesWithResponseHeaderFields:@{@"Set-Cookie" : value} forURL:url]
        : @[];
    const uint64_t token = self.token;
    __weak MinatoWebView *weakSelf = self;
    void (^done)(BOOL) = ^(BOOL result) {
        [weakSelf postCookies:^{
            post(token, APKRT_WEBVIEW_REC_JS_RESULT, requestId, @{@"json" : result ? @"true" : @"false"});
        }];
    };
    if (cookies.count == 0) {
        done(NO);
        return;
    }
    __block NSUInteger left = cookies.count;
    for (NSHTTPCookie *cookie in cookies) {
        [self.cookieStore setCookie:cookie
                  completionHandler:^{
                      if (--left == 0) done(YES);
                  }];
    }
}

/// removeAllCookies: every cookie of the store; answered after the list without them, true when there were any.
- (void)removeAllCookies:(uint64_t)requestId {
    const uint64_t token = self.token;
    __weak MinatoWebView *weakSelf = self;
    WKHTTPCookieStore *store = self.cookieStore;
    [store getAllCookies:^(NSArray<NSHTTPCookie *> *cookies) {
        void (^done)(void) = ^{
            [weakSelf postCookies:^{
                NSString *removed = cookies.count > 0 ? @"true" : @"false";
                post(token, APKRT_WEBVIEW_REC_JS_RESULT, requestId, @{@"json" : removed});
            }];
        };
        if (cookies.count == 0) {
            done();
            return;
        }
        __block NSUInteger left = cookies.count;
        for (NSHTTPCookie *cookie in cookies) {
            [store deleteCookie:cookie
                completionHandler:^{
                    if (--left == 0) done();
                }];
        }
    }];
}

- (void)make:(NSDictionary *)call {
    if (self.view != nil || self.cookieStore != nil) return;
    if ([call[@"cookies"] boolValue]) {  // CookieManager's channel: the store alone
        [self observeCookies:data_store(call[@"dataStore"])];
        [self postCookies:nil];
        return;
    }
    WKWebViewConfiguration *configuration = [WKWebViewConfiguration new];
    configuration.websiteDataStore = data_store(call[@"dataStore"]);
    [self observeCookies:configuration.websiteDataStore];
    for (NSString *scheme in call[@"schemes"]) {
        if ([scheme isKindOfClass:NSString.class] && ![WKWebView handlesURLScheme:scheme]) {
            [configuration setURLSchemeHandler:self forURLScheme:scheme];
        }
    }
    self.settings = call[@"settings"];
#if TARGET_OS_IPHONE
    configuration.allowsInlineMediaPlayback = YES;
    configuration.mediaTypesRequiringUserActionForPlayback =
        [self.settings[@"mediaNeedsGesture"] boolValue] ? WKAudiovisualMediaTypeAll : WKAudiovisualMediaTypeNone;
#endif
    WKUserContentController *content = configuration.userContentController;
    [content addUserScript:[[WKUserScript alloc] initWithSource:kTitleScript
                                                  injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                               forMainFrameOnly:YES
                                                 inContentWorld:host_world()]];
    [content addUserScript:[[WKUserScript alloc] initWithSource:kConsoleScript
                                                  injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                               forMainFrameOnly:NO
                                                 inContentWorld:WKContentWorld.pageWorld]];
    [self addBridgeTo:content];
    [content addScriptMessageHandler:self contentWorld:host_world() name:@"minatoTitle"];
    [content addScriptMessageHandler:self contentWorld:WKContentWorld.pageWorld name:@"minatoConsole"];
    WKWebView *view = [[WKWebView alloc] initWithFrame:CGRectMake(0, 0, 1, 1) configuration:configuration];
    view.navigationDelegate = self;
    view.UIDelegate = self;
#if DEBUG
    view.inspectable = YES;  // decision 9: Safari's Web Inspector sees the guest's pages
#endif
    [view addObserver:self forKeyPath:@"title" options:NSKeyValueObservingOptionNew context:NULL];
    self.view = view;
    [self applySettings];
    [self show];
}

- (void)applySettings {
    NSString *agent = self.settings[@"userAgent"];
    self.view.customUserAgent = [agent isKindOfClass:NSString.class] ? agent : nil;
    const BOOL wide = [self.settings[@"wideViewPort"] boolValue];
    if (wide != self.wideViewPort) {
        self.wideViewPort = wide;
        [self updateViewport];
    }
}

/// A call's whole object, once its last part came (Runtime's parts: the cut field split, the rest in part 0).
- (NSDictionary *)joined:(NSDictionary *)call {
    if (call[@"part"] == nil) return call;
    if ([call[@"part"] integerValue] == 0) {
        self.parts = [call mutableCopy];
    } else if (self.parts != nil) {
        for (NSString *key in call) {
            if ([key isEqualToString:@"call"] || [key isEqualToString:@"part"] || [key isEqualToString:@"last"])
                continue;
            NSString *before = self.parts[key];
            if ([before isKindOfClass:NSString.class] && [call[key] isKindOfClass:NSString.class]) {
                self.parts[key] = [before stringByAppendingString:call[key]];
            }
        }
    }
    if (![call[@"last"] boolValue] || self.parts == nil) return nil;
    NSDictionary *whole = self.parts;
    self.parts = nil;
    return whole;
}

- (void)call:(NSDictionary *)given requestId:(uint64_t)requestId {
    NSDictionary *call = [self joined:given];
    if (call == nil) return;
    NSString *name = call[@"call"];
    if ([name isEqualToString:@"make"]) {
        [self make:call];
        return;
    }
    if ([name isEqualToString:@"settings"]) {
        self.settings = call[@"settings"];
        [self applySettings];
        return;
    }
    if ([name isEqualToString:@"interfaces"]) {
        self.interfaces = [call[@"objects"] isKindOfClass:NSDictionary.class] ? call[@"objects"] : nil;
        [self updateBridge];
        return;
    }
    if ([name isEqualToString:@"evaluateJavascript"]) {
        [self evaluate:call[@"script"] requestId:requestId];
        return;
    }
    if ([name isEqualToString:@"setCookie"]) {  // a view's or CookieManager's channel
        [self setCookie:call requestId:requestId];
        return;
    }
    if ([name isEqualToString:@"removeAllCookies"]) {
        [self removeAllCookies:requestId];
        return;
    }
    WKWebView *view = self.view;
    if (view == nil) return;
    if ([name isEqualToString:@"loadUrl"] || [name isEqualToString:@"postUrl"]) {
        NSURL *url = [NSURL URLWithString:either(call[@"url"], @"")];
        if (url == nil) return;
        NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
        NSDictionary *headers = call[@"headers"];
        if ([headers isKindOfClass:NSDictionary.class]) {
            for (NSString *key in headers) [request setValue:[headers[key] description] forHTTPHeaderField:key];
        }
        if ([name isEqualToString:@"postUrl"]) {
            request.HTTPMethod = @"POST";
            request.HTTPBody = [[NSData alloc] initWithBase64EncodedString:either(call[@"body"], @"") options:0];
        }
        [self open:[self entryFor:request]];
    } else if ([name isEqualToString:@"loadData"]) {
        NSString *mime = [call[@"mimeType"] isKindOfClass:NSString.class] ? call[@"mimeType"] : @"text/html";
        const BOOL base64 = [call[@"encoding"] isEqual:@"base64"];
        NSString *data = either(call[@"data"], @"");
        NSString *url = [NSString stringWithFormat:@"data:%@%@,%@", mime, base64 ? @";base64" : @"",
                                                   base64 ? data
                                                          : [data stringByAddingPercentEncodingWithAllowedCharacters:
                                                                      NSCharacterSet.URLQueryAllowedCharacterSet]];
        NSURL *dataURL = [NSURL URLWithString:url];
        if (dataURL != nil) [self open:[self entryFor:[NSURLRequest requestWithURL:dataURL]]];
    } else if ([name isEqualToString:@"loadDataWithBaseURL"]) {
        [self loadData:call];
    } else if ([name isEqualToString:@"reload"]) {
        [view reload];
    } else if ([name isEqualToString:@"stopLoading"]) {
        [view stopLoading];
    } else if ([name isEqualToString:@"goBack"]) {
        [self go:-1];
    } else if ([name isEqualToString:@"goForward"]) {
        [self go:1];
    } else if ([name isEqualToString:@"goBackOrForward"]) {
        [self go:[call[@"steps"] integerValue]];
    } else if ([name isEqualToString:@"clearHistory"]) {
        if (self.index >= 0) {
            MinatoWebItem *current = self.entries[(NSUInteger)self.index];
            [self.entries setArray:@[ current ]];
            self.index = 0;
        }
    } else if ([name isEqualToString:@"touchRegion"]) {
        NSMutableArray<NSValue *> *region = [NSMutableArray new];
        for (NSArray *rect in either(call[@"rects"], @[])) {
            if (![rect isKindOfClass:NSArray.class] || rect.count != 4) continue;
            const CGFloat left = [rect[0] doubleValue], top = [rect[1] doubleValue];
            const CGRect value = CGRectMake(left, top, [rect[2] doubleValue] - left, [rect[3] doubleValue] - top);
#if TARGET_OS_IPHONE
            if (!CGRectIsEmpty(value)) [region addObject:[NSValue valueWithCGRect:value]];
#else
            if (!CGRectIsEmpty(value)) [region addObject:[NSValue valueWithRect:value]];
#endif
        }
        self.touchRegion = region;
        [self show];
    } else if ([name isEqualToString:@"clearCache"]) {
        NSMutableSet *types = [NSMutableSet setWithObjects:WKWebsiteDataTypeMemoryCache, nil];
        if ([call[@"includeDiskFiles"] boolValue]) [types addObject:WKWebsiteDataTypeDiskCache];
        [view.configuration.websiteDataStore removeDataOfTypes:types modifiedSince:NSDate.distantPast
                                             completionHandler:^{}];
    } else if ([name isEqualToString:@"pause"]) {
        [view pauseAllMediaPlaybackWithCompletionHandler:^{}];
    }
}

/// loadDataWithBaseURL: the page at its base URL (about:blank without one, or with one WebKit cannot be the origin
/// of), its entry "data:text/html;charset=utf-8;base64," or the history URL, as Chromium keeps it.
- (void)loadData:(NSDictionary *)call {
    NSString *base = [call[@"baseUrl"] isKindOfClass:NSString.class] ? call[@"baseUrl"] : nil;
    NSURL *baseURL = base.length > 0 ? [NSURL URLWithString:base] : nil;
    NSString *scheme = baseURL.scheme.lowercaseString;
    if (scheme == nil || [scheme isEqualToString:@"data"] || [scheme isEqualToString:@"about"] ||
        [scheme isEqualToString:@"javascript"]) {
        baseURL = nil;
    }
    NSString *history = [call[@"historyUrl"] isKindOfClass:NSString.class] && [call[@"historyUrl"] length] > 0
                            ? call[@"historyUrl"]
                            : @"data:text/html;charset=utf-8;base64,";
    MinatoWebItem *item = [MinatoWebItem new];
    item.url = history;
    item.original = history;
    item.title = @"";
    item.viewURL = baseURL != nil ? base : @"about:blank";
    NSString *mime = [call[@"mimeType"] isKindOfClass:NSString.class] ? call[@"mimeType"] : @"text/html";
    NSString *encoding = [call[@"encoding"] isKindOfClass:NSString.class] ? call[@"encoding"] : @"utf-8";
    NSString *data = either(call[@"data"], @"");
    NSData *bytes = [encoding isEqualToString:@"base64"] ? [[NSData alloc] initWithBase64EncodedString:data options:0]
                                                          : [data dataUsingEncoding:NSUTF8StringEncoding];
    if ([encoding isEqualToString:@"base64"]) encoding = @"utf-8";
    item.data = either(bytes, [NSData data]);
    item.mime = mime;
    item.encoding = encoding;
    item.baseURL = either(baseURL, [NSURL URLWithString:@"about:blank"]);
    [self open:item];
}

/// A new entry for a request the host loads.
- (MinatoWebItem *)entryFor:(NSURLRequest *)request {
    MinatoWebItem *item = [MinatoWebItem new];
    item.url = either(request.URL.absoluteString, @"about:blank");
    item.original = item.url;
    item.title = @"";
    item.request = request;
    return item;
}

/// Loads `item`, the coming commit's entry.
- (void)open:(MinatoWebItem *)item {
    self.pending = item;
    if (item.error) {
        [self.view loadRequest:item.request];  // fails again, and its error page commits at the entry
    } else if (item.request != nil) {
        [self.view loadRequest:item.request];
    } else {
        [self.view loadData:item.data MIMEType:item.mime characterEncodingName:item.encoding baseURL:item.baseURL];
    }
}

/// Back or forward by `steps` entries, loading the entry again.
- (void)go:(NSInteger)steps {
    const NSInteger to = self.index + steps;
    if (steps == 0 || to < 0 || to >= (NSInteger)self.entries.count) return;
    self.target = to;
    MinatoWebItem *entry = self.entries[(NSUInteger)to];
    [self open:entry];
    self.pending = nil;  // the commit takes the target's entry
}

- (void)evaluate:(NSString *)script requestId:(uint64_t)requestId {
    const uint64_t token = self.token;
    void (^answer)(id) = ^(id value) {
        NSMutableString *json = [NSMutableString new];
        append_json_value(json, value);
        post(token, APKRT_WEBVIEW_REC_JS_RESULT, requestId, @{@"json" : json});
    };
    if (self.view == nil || ![script isKindOfClass:NSString.class]) {
        answer(nil);
        return;
    }
    [self.view evaluateJavaScript:script completionHandler:^(id value, NSError *error) {
        answer(error != nil ? nil : value);
    }];
}

/// The guest's ANSWER to a record that waits.
- (void)answer:(uint64_t)requestId payload:(NSData *)payload fd:(int)fd offset:(int64_t)offset length:(int64_t)length {
    void (^waiter)(NSData *, int, int64_t, int64_t) = self.waiting[@(requestId)];
    if (waiter == nil) {
        if (fd >= 0) close(fd);
        return;
    }
    waiter(payload, fd, offset, length);
}

/// Waits for the guest's answer to the record `requestId`: `answered` with it, or `fallback` after 10 s or at the
/// view's end. A resource's waiter stays for its later answers.
- (void)wait:(uint64_t)requestId answered:(void (^)(NSData *, int, int64_t, int64_t))answered
    fallback:(void (^)(void))fallback {
    __block BOOL done = NO;
    __weak MinatoWebView *weakSelf = self;
    self.waiting[@(requestId)] = ^(NSData *payload, int fd, int64_t offset, int64_t length) {
        done = YES;
        answered(payload, fd, offset, length);
    };
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kAnswerWait), dispatch_get_main_queue(), ^{
        MinatoWebView *strong = weakSelf;
        if (done || strong == nil || strong.waiting[@(requestId)] == nil) return;
        [strong.waiting removeObjectForKey:@(requestId)];
        fallback();
    });
}

/// The view's end (REMOVE): its open resources fail, the answers it waits for take their defaults.
- (void)remove {
    [self.cookieStore removeObserver:self];
    self.cookieStore = nil;
    for (MinatoResource *resource in self.resources.allValues) {
        if (!resource.stopped) {
            resource.stopped = YES;
            [resource.task didFailWithError:[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorCancelled
                                                            userInfo:nil]];
        }
    }
    [self.resources removeAllObjects];
    NSArray *waiters = self.waiting.allValues;
    [self.waiting removeAllObjects];
    for (void (^waiter)(NSData *, int, int64_t, int64_t) in waiters) waiter(nil, -1, 0, 0);
    self.placed = NO;
    [self show];
    [self.view stopLoading];
    [self.view removeObserver:self forKeyPath:@"title"];
    [self.view.configuration.userContentController removeAllScriptMessageHandlers];
    self.view.navigationDelegate = nil;
    self.view.UIDelegate = nil;
    self.view = nil;
}

// MARK: The view's place (c2)

- (void)placeIn:(uint64_t)window frame:(CGRect)frame clip:(CGRect)clip {
    self.placed = YES;
    self.window = window;
    self.frame = frame;
    self.clip = clip;
    [self show];
}

/// Puts the view where its last PLACE says, or takes it out: before its "make", after REMOVE, or for a window the
/// host does not show. An empty clip hides it.
- (void)show {
    void *above = NULL;
    double scale = 1;
    MinatoHostView *container = nil;
    if (self.view != nil && self.placed && host_windows != NULL && host_windows->container != NULL) {
        container = (__bridge MinatoHostView *)host_windows->container(host_windows->context, self.window, &above,
                                                                       &scale);
    }
    MinatoHostView *windowView = (__bridge MinatoHostView *)above;
    if (container == nil || windowView.superview != container) {
        [self.holder removeFromSuperview];
        [self count:0];
        return;
    }
    if (self.holder == nil) {
        self.holder = [[MinatoClipView alloc] initWithFrame:CGRectZero];
        self.holder.clipsToBounds = YES;
        [self.holder addSubview:self.view];
    }
    const NSUInteger at = [container.subviews indexOfObject:windowView];
    if (self.holder.superview != container || at == 0 || container.subviews[at - 1] != self.holder) {
        [self.holder removeFromSuperview];
#if TARGET_OS_IPHONE
        [container insertSubview:self.holder belowSubview:windowView];
#else
        [container addSubview:self.holder positioned:NSWindowBelow relativeTo:windowView];
#endif
    }
    const CGFloat points = scale > 0 ? (CGFloat)scale : 1;
    // In the container's points, from the window's view's origin.
    CGRect clip = CGRectMake(windowView.frame.origin.x + self.clip.origin.x / points,
                             windowView.frame.origin.y + self.clip.origin.y / points, self.clip.size.width / points,
                             self.clip.size.height / points);
    const CGRect page = CGRectMake((self.frame.origin.x - self.clip.origin.x) / points,
                                   (self.frame.origin.y - self.clip.origin.y) / points,
                                   self.frame.size.width / points, self.frame.size.height / points);
#if !TARGET_OS_IPHONE
    if (!container.isFlipped) {
        clip.origin.y = NSMaxY(windowView.frame) - (self.clip.origin.y + self.clip.size.height) / points;
    }
#endif
    self.holder.frame = clip;
    self.view.frame = page;
    [self viewportWidth:page.size.width];
    NSMutableArray<NSValue *> *region = [NSMutableArray new];
    for (NSValue *value in self.touchRegion) {
#if TARGET_OS_IPHONE
        const CGRect rect = value.CGRectValue;
#else
        const CGRect rect = value.rectValue;
#endif
        const CGRect local = CGRectMake((rect.origin.x - self.clip.origin.x) / points,
                                        (rect.origin.y - self.clip.origin.y) / points, rect.size.width / points,
                                        rect.size.height / points);
#if TARGET_OS_IPHONE
        [region addObject:[NSValue valueWithCGRect:local]];
#else
        [region addObject:[NSValue valueWithRect:local]];
#endif
    }
    self.holder.region = region;
    const BOOL visible = !CGRectIsEmpty(self.clip);
    self.holder.hidden = !visible;
    [self count:visible ? self.window : 0];
}

/// A new width for "device-width", in whole CSS pixels: the scripts of later pages get it, and the page now shows takes
/// it at once.
- (void)viewportWidth:(CGFloat)width {
    width = (CGFloat)lround(width);
    if (width <= 0 || width == self.cssWidth) return;
    self.cssWidth = width;
    [self updateViewport];
}

/// The app's objects' script, if it has any, to `content`.
- (void)addBridgeTo:(WKUserContentController *)content {
    if (self.interfaces.count == 0) return;
    NSData *json = [NSJSONSerialization dataWithJSONObject:self.interfaces options:0 error:nil];
    if (json == nil) return;
    NSString *objects = [[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding];
    [content addUserScript:[[WKUserScript alloc]
                                 initWithSource:[NSString stringWithFormat:kBridgeScript, kBridgePrompt, objects]
                                  injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                               forMainFrameOnly:NO
                                 inContentWorld:WKContentWorld.pageWorld]];
}

/// The scripts again with the app's objects now, for the next pages; the page now shown keeps its own, as Android's.
- (void)updateBridge {
    if (self.view == nil) return;
    if (self.cssWidth > 0) {
        [self updateViewport];
        return;
    }
    WKUserContentController *content = self.view.configuration.userContentController;
    [content removeAllUserScripts];
    [content addUserScript:[[WKUserScript alloc] initWithSource:kTitleScript
                                                  injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                               forMainFrameOnly:YES
                                                 inContentWorld:host_world()]];
    [content addUserScript:[[WKUserScript alloc] initWithSource:kConsoleScript
                                                  injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                               forMainFrameOnly:NO
                                                 inContentWorld:WKContentWorld.pageWorld]];
    [self addBridgeTo:content];
}

/// A page's call of the app's objects: the JS_BRIDGE record, then the guest's answer, as the prompt's text; none
/// (undefined) after 10 s or at the view's end.
- (void)bridge:(NSString *)prompt done:(void (^)(NSString *))done {
    NSData *data = [[prompt substringFromIndex:kBridgePrompt.length] dataUsingEncoding:NSUTF8StringEncoding];
    NSDictionary *call = data != nil ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    if (![call isKindOfClass:NSDictionary.class]) {
        done(nil);
        return;
    }
    const uint64_t rid = next_record_id();
    [self wait:rid
        answered:^(NSData *payload, int fd, int64_t offset, int64_t length) {
            (void)offset;
            (void)length;
            if (fd >= 0) close(fd);
            [self.waiting removeObjectForKey:@(rid)];
            done(payload != nil ? [[NSString alloc] initWithData:payload encoding:NSUTF8StringEncoding] : nil);
        }
        fallback:^{
            done(nil);
        }];
    post(self.token, APKRT_WEBVIEW_REC_JS_BRIDGE, rid, call);
}

/// The viewport script for the width and the setting now: for later pages and the page now shown.
- (void)updateViewport {
    if (self.cssWidth <= 0 || self.view == nil) return;
    NSString *script = [NSString stringWithFormat:kViewportScript, @(self.cssWidth),
                                                  self.wideViewPort ? @"true" : @"false"];
    WKUserContentController *content = self.view.configuration.userContentController;
    [content removeAllUserScripts];
    [content addUserScript:[[WKUserScript alloc] initWithSource:kTitleScript
                                                  injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                               forMainFrameOnly:YES
                                                 inContentWorld:host_world()]];
    [content addUserScript:[[WKUserScript alloc] initWithSource:kConsoleScript
                                                  injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                               forMainFrameOnly:NO
                                                 inContentWorld:WKContentWorld.pageWorld]];
    [self addBridgeTo:content];
    [content addUserScript:[[WKUserScript alloc] initWithSource:script
                                                  injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                               forMainFrameOnly:YES
                                                 inContentWorld:host_world()]];
    [self.view evaluateJavaScript:script inFrame:nil inContentWorld:host_world() completionHandler:nil];
}

/// Moves the view's share of a window's count of shown views to `window` (0: none).
- (void)count:(uint64_t)window {
    if (window == self.shownIn) return;
    count_shown(self.shownIn, -1);
    self.shownIn = window;
    count_shown(window, 1);
}

// MARK: Records

- (void)navigation:(NSString *)phase url:(NSString *)url {
    post(self.token, APKRT_WEBVIEW_REC_NAVIGATION, 0, @{@"phase" : phase, @"url" : either(url, NSNull.null)});
}

- (void)progress:(int)value {
    post(self.token, APKRT_WEBVIEW_REC_PROGRESS, 0, @{@"progress" : @(value)});
}

/// The URL the view has while it shows `item`.
static NSString *view_url(MinatoWebItem *item) {
    return item.viewURL != nil ? item.viewURL : item.url;
}

/// The page committed: the history and the view's URL, as the guest mirrors them; then a title that came before.
- (void)committed {
    MinatoWebItem *entry;
    if (self.target >= 0 && self.target < (NSInteger)self.entries.count) {
        self.index = self.target;
        entry = self.entries[(NSUInteger)self.index];
    } else if (self.reload && self.index >= 0 && self.pending == nil) {
        entry = self.entries[(NSUInteger)self.index];
    } else {
        entry = self.pending;
        if (entry == nil) {  // the page's own navigation: a link, a form, a script
            NSURL *shown = either(self.view.URL, [NSURL URLWithString:@"about:blank"]);
            entry = [self entryFor:[NSURLRequest requestWithURL:shown]];
        }
        if (self.index + 1 < (NSInteger)self.entries.count) {
            [self.entries removeObjectsInRange:NSMakeRange((NSUInteger)(self.index + 1),
                                                           self.entries.count - (NSUInteger)(self.index + 1))];
        }
        [self.entries addObject:entry];
        self.index = (NSInteger)self.entries.count - 1;
    }
    self.target = -1;
    self.pending = nil;
    self.committing = NO;
    NSMutableArray *history = [NSMutableArray new];
    for (MinatoWebItem *item in self.entries) {
        [history addObject:@{@"url" : item.url, @"originalUrl" : item.original, @"title" : either(item.title, @"")}];
    }
    post(self.token, APKRT_WEBVIEW_REC_NAVIGATION, 0, @{
        @"phase" : @"committed",
        @"url" : view_url(entry),
        @"originalUrl" : entry.original,
        @"index" : @(self.index),
        @"history" : history,
        @"reload" : @(self.reload)
    });
    if (self.heldTitle != nil) {
        NSString *title = self.heldTitle;
        self.heldTitle = nil;
        [self title:title];
    }
}

/// The current page's title, once it is known and new.
- (void)title:(NSString *)title {
    if (title.length == 0 || self.index < 0) return;
    if (self.committing) {
        self.heldTitle = title;
        return;
    }
    MinatoWebItem *current = self.entries[(NSUInteger)self.index];
    if ([current.title isEqualToString:title]) return;
    current.title = title;
    post(self.token, APKRT_WEBVIEW_REC_TITLE, 0, @{@"title" : title});
}

/// The URL the guest knows the view by.
- (NSString *)currentURL {
    return self.index >= 0 ? view_url(self.entries[(NSUInteger)self.index]) : nil;
}

- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object change:(NSDictionary *)change
                       context:(void *)context {
    (void)change;
    (void)context;
    if (![keyPath isEqualToString:@"title"] || object != self.view) return;
    [self title:self.view.title];
}

/// A WebKit or network error as Chromium's onReceivedError gives it: Android's ERROR_* code and net's name.
static void chromium_error(NSError *error, int *code, NSString **description) {
    *code = -1;  // ERROR_UNKNOWN
    *description = @"net::ERR_FAILED";
    if (![error.domain isEqualToString:NSURLErrorDomain]) return;
    switch (error.code) {
        case NSURLErrorFileDoesNotExist: *code = -1; *description = @"net::ERR_FILE_NOT_FOUND"; break;
        case NSURLErrorCannotFindHost:
        case NSURLErrorDNSLookupFailed: *code = -2; *description = @"net::ERR_NAME_NOT_RESOLVED"; break;
        case NSURLErrorNotConnectedToInternet: *code = -2; *description = @"net::ERR_INTERNET_DISCONNECTED"; break;
        case NSURLErrorCannotConnectToHost: *code = -6; *description = @"net::ERR_CONNECTION_REFUSED"; break;
        case NSURLErrorNetworkConnectionLost: *code = -6; *description = @"net::ERR_CONNECTION_RESET"; break;
        case NSURLErrorTimedOut: *code = -8; *description = @"net::ERR_TIMED_OUT"; break;
        case NSURLErrorHTTPTooManyRedirects: *code = -9; *description = @"net::ERR_TOO_MANY_REDIRECTS"; break;
        case NSURLErrorUnsupportedURL: *code = -10; *description = @"net::ERR_UNKNOWN_URL_SCHEME"; break;
        case NSURLErrorSecureConnectionFailed: *code = -11; *description = @"net::ERR_SSL_PROTOCOL_ERROR"; break;
        case NSURLErrorBadURL: *code = -12; *description = @"net::ERR_INVALID_URL"; break;
        case NSURLErrorServerCertificateUntrusted:
        case NSURLErrorServerCertificateHasBadDate:
        case NSURLErrorServerCertificateHasUnknownRoot:
        case NSURLErrorServerCertificateNotYetValid: *code = -11; *description = @"net::ERR_CERT_INVALID"; break;
        default: break;
    }
}

// MARK: WKNavigationDelegate

- (void)webView:(WKWebView *)webView decidePolicyForNavigationAction:(WKNavigationAction *)action
                                                         preferences:(WKWebpagePreferences *)preferences
                                                     decisionHandler:(void (^)(WKNavigationActionPolicy,
                                                                               WKWebpagePreferences *))decision {
    id javaScript = self.settings[@"javaScript"];
    preferences.allowsContentJavaScript = javaScript == nil || [javaScript boolValue];
    if (action.targetFrame.isMainFrame) self.reload = action.navigationType == WKNavigationTypeReload;
    decision(WKNavigationActionPolicyAllow, preferences);
}

- (void)webView:(WKWebView *)webView decidePolicyForNavigationResponse:(WKNavigationResponse *)response
                                                       decisionHandler:(void (^)(WKNavigationResponsePolicy))decision {
    NSHTTPURLResponse *http = [response.response isKindOfClass:NSHTTPURLResponse.class]
                                  ? (NSHTTPURLResponse *)response.response
                                  : nil;
    if (http != nil && http.statusCode >= 400 && ![http.URL.scheme isEqualToString:@"minato-asset"]) {
        NSMutableDictionary *headers = [NSMutableDictionary new];
        [http.allHeaderFields enumerateKeysAndObjectsUsingBlock:^(id key, id value, BOOL *stop) {
            (void)stop;
            headers[[key description]] = [value description];
        }];
        post(self.token, APKRT_WEBVIEW_REC_ERROR, 0, @{
            @"http" : @YES,
            @"url" : either(http.URL.absoluteString, @""),
            @"mainFrame" : @(response.isForMainFrame),
            @"status" : @(http.statusCode),
            @"reason" : [NSHTTPURLResponse localizedStringForStatusCode:http.statusCode],
            @"headers" : headers
        });
    }
    decision(WKNavigationResponsePolicyAllow);
}

- (void)webView:(WKWebView *)webView didStartProvisionalNavigation:(WKNavigation *)navigation {
    (void)navigation;
    self.committing = YES;
    self.heldTitle = nil;
    if (self.errorPage) return;
    MinatoWebItem *coming = self.target >= 0 ? self.entries[(NSUInteger)self.target] : self.pending;
    [self navigation:@"started" url:coming != nil ? view_url(coming) : webView.URL.absoluteString];
    [self progress:10];
}

- (void)webView:(WKWebView *)webView didCommitNavigation:(WKNavigation *)navigation {
    [self committed];
}

- (void)webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation {
    (void)navigation;
    // WebKit reports the title after the load's end; Chromium before onPageFinished: so the host reads it first, in
    // a world of its own, which the page's JavaScript setting does not reach.
    const BOOL error = self.errorPage;
    self.errorPage = NO;
    __weak MinatoWebView *weakSelf = self;
    [webView evaluateJavaScript:@"document.title" inFrame:nil inContentWorld:host_world()
              completionHandler:^(id value, NSError *failure) {
                  MinatoWebView *strong = weakSelf;
                  if (strong == nil) return;
                  if (failure == nil && [value isKindOfClass:NSString.class]) [strong title:value];
                  // The store's cookies before onPageFinished, as Chromium's are there at once.
                  [strong postCookies:^{
                      [strong progress:100];
                      if (!error) {
                          [strong navigation:@"finished" url:either(strong.currentURL, webView.URL.absoluteString)];
                      }
                  }];
              }];
}

/// A main-frame load that failed: its error, onPageFinished, then Chromium's error page at its URL.
- (void)failed:(NSError *)error url:(NSString *)url {
    if ([error.domain isEqualToString:NSURLErrorDomain] && error.code == NSURLErrorCancelled) return;
    // WebKit's "frame load interrupted by policy change" (WebKitErrorDomain 102): a load that became a download.
    if ([error.domain isEqualToString:@"WebKitErrorDomain"] && error.code == 102) return;
    if (self.errorPage) {
        self.errorPage = NO;
        return;
    }
    self.committing = NO;
    int code;
    NSString *description;
    chromium_error(error, &code, &description);
    NSString *failed = either(url, @"");
    post(self.token, APKRT_WEBVIEW_REC_ERROR, 0,
         @{@"mainFrame" : @YES, @"code" : @(code), @"description" : description, @"url" : failed});
    [self navigation:@"finished" url:failed];
    // The error page's entry: at the failed URL, loading it again on back or forward.
    MinatoWebItem *entry = self.target >= 0 ? nil : [self entryFor:[NSURLRequest requestWithURL:
                                                                     either([NSURL URLWithString:failed],
                                                                            [NSURL URLWithString:@"about:blank"])]];
    entry.error = YES;
    self.pending = entry;
    self.errorPage = YES;
    [self.view loadHTMLString:kErrorPage baseURL:[NSURL URLWithString:failed]];
}

- (void)webView:(WKWebView *)webView didFailProvisionalNavigation:(WKNavigation *)navigation
      withError:(NSError *)error {
    NSURL *url = error.userInfo[NSURLErrorFailingURLErrorKey];
    [self failed:error url:either(url.absoluteString, webView.URL.absoluteString)];
}

- (void)webView:(WKWebView *)webView didFailNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    NSURL *url = error.userInfo[NSURLErrorFailingURLErrorKey];
    [self failed:error url:either(url.absoluteString, webView.URL.absoluteString)];
}

// MARK: WKScriptMessageHandler

- (void)userContentController:(WKUserContentController *)controller didReceiveScriptMessage:(WKScriptMessage *)message {
    (void)controller;
    if ([message.name isEqualToString:@"minatoTitle"] && [message.body isKindOfClass:NSString.class]) {
        [self title:message.body];
    } else if ([message.name isEqualToString:@"minatoConsole"] && [message.body isKindOfClass:NSDictionary.class]) {
        NSDictionary *body = message.body;
        post(self.token, APKRT_WEBVIEW_REC_CONSOLE, 0, @{
            @"level" : [body[@"level"] isKindOfClass:NSString.class] ? body[@"level"] : @"log",
            @"message" : [body[@"message"] isKindOfClass:NSString.class] ? body[@"message"] : @"",
            @"source" : [body[@"source"] isKindOfClass:NSString.class] ? body[@"source"] : @"",
            @"line" : [body[@"line"] isKindOfClass:NSNumber.class] ? body[@"line"] : @0,
        });
    }
}

// MARK: WKUIDelegate

/// A script's dialog: the JS_DIALOG record, then the guest's answer {"result", "value"}; a closed dialog after 10 s.
- (void)dialog:(NSString *)kind frame:(WKFrameInfo *)frame message:(NSString *)message default:(NSString *)value
    done:(void (^)(BOOL, NSString *))done {
    const uint64_t rid = next_record_id();
    NSMutableDictionary *record = [@{
        @"kind" : kind,
        @"url" : either(frame.request.URL.absoluteString, either(self.currentURL, @"")),
        @"message" : either(message, @"")
    } mutableCopy];
    if (value != nil) record[@"default"] = value;
    [self wait:rid
        answered:^(NSData *payload, int fd, int64_t offset, int64_t length) {
            (void)offset;
            (void)length;
            if (fd >= 0) close(fd);
            NSDictionary *answer = payload != nil ? [NSJSONSerialization JSONObjectWithData:payload options:0 error:nil]
                                                  : nil;
            [self.waiting removeObjectForKey:@(rid)];
            const BOOL result = [answer isKindOfClass:NSDictionary.class] && [answer[@"result"] boolValue];
            NSString *text = [answer isKindOfClass:NSDictionary.class] ? answer[@"value"] : nil;
            if (![text isKindOfClass:NSString.class]) text = nil;
            done(result, text);
        }
        fallback:^{
            done(NO, nil);
        }];
    post(self.token, APKRT_WEBVIEW_REC_JS_DIALOG, rid, record);
}

- (void)webView:(WKWebView *)webView runJavaScriptAlertPanelWithMessage:(NSString *)message
    initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(void))completion {
    [self dialog:@"alert" frame:frame message:message default:nil done:^(BOOL result, NSString *text) {
        (void)result;
        (void)text;
        completion();
    }];
}

- (void)webView:(WKWebView *)webView runJavaScriptConfirmPanelWithMessage:(NSString *)message
    initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(BOOL))completion {
    [self dialog:@"confirm" frame:frame message:message default:nil done:^(BOOL result, NSString *text) {
        (void)text;
        completion(result);
    }];
}

- (void)webView:(WKWebView *)webView runJavaScriptTextInputPanelWithPrompt:(NSString *)prompt
          defaultText:(NSString *)text initiatedByFrame:(WKFrameInfo *)frame
    completionHandler:(void (^)(NSString *))completion {
    if ([prompt hasPrefix:kBridgePrompt]) {
        [self bridge:prompt done:completion];
        return;
    }
    [self dialog:@"prompt" frame:frame message:prompt default:either(text, @"") done:^(BOOL result, NSString *value) {
        completion(result ? either(value, @"") : nil);
    }];
}

// MARK: WKURLSchemeHandler

/// The guest serves the request: its head, then its body in answers, then an empty answer.
- (void)webView:(WKWebView *)webView startURLSchemeTask:(id<WKURLSchemeTask>)task {
    const uint64_t rid = next_record_id();
    MinatoResource *resource = [MinatoResource new];
    resource.task = task;
    self.resources[@(rid)] = resource;
    NSURLRequest *request = task.request;
    NSMutableDictionary *headers = [NSMutableDictionary new];
    [request.allHTTPHeaderFields enumerateKeysAndObjectsUsingBlock:^(NSString *key, NSString *value, BOOL *stop) {
        (void)stop;
        headers[key] = value;
    }];
    const BOOL main = request.mainDocumentURL == nil || [request.mainDocumentURL isEqual:request.URL];
    __weak MinatoWebView *weakSelf = self;
    [self wait:rid
        answered:^(NSData *payload, int fd, int64_t offset, int64_t length) {
            [weakSelf resource:rid answer:payload fd:fd offset:offset length:length];
        }
        fallback:^{
            [weakSelf resource:rid answer:nil fd:-1 offset:0 length:0];
        }];
    post(self.token, APKRT_WEBVIEW_REC_RESOURCE, rid, @{
        @"url" : either(request.URL.absoluteString, @""),
        @"method" : either(request.HTTPMethod, @"GET"),
        @"headers" : headers,
        @"mainFrame" : @(main)
    });
}

- (void)webView:(WKWebView *)webView stopURLSchemeTask:(id<WKURLSchemeTask>)task {
    for (MinatoResource *resource in self.resources.allValues) {
        if (resource.task == task) resource.stopped = YES;
    }
}

/// One answer of the guest for a resource: the head, a piece of the body, or the end (empty). nil: the guest never
/// answered, or the view ended.
- (void)resource:(uint64_t)rid answer:(NSData *)payload fd:(int)fd offset:(int64_t)offset length:(int64_t)length {
    MinatoResource *resource = self.resources[@(rid)];
    const BOOL end = payload == nil || (payload.length == 0 && fd < 0);
    if (resource == nil || resource.stopped) {
        if (fd >= 0) close(fd);
        if (end) {
            [self.resources removeObjectForKey:@(rid)];
            [self.waiting removeObjectForKey:@(rid)];
        }
        return;
    }
    id<WKURLSchemeTask> task = resource.task;
    if (!resource.headed) {
        NSDictionary *head = payload.length > 0 ? [NSJSONSerialization JSONObjectWithData:payload options:0 error:nil]
                                                : nil;
        const NSInteger status = [head isKindOfClass:NSDictionary.class] ? [head[@"status"] integerValue] : 0;
        if (status < 100 || status >= 400) {
            resource.stopped = YES;
            [task didFailWithError:[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorFileDoesNotExist
                                                   userInfo:@{NSURLErrorFailingURLErrorKey : task.request.URL}]];
            if (end) {
                [self.resources removeObjectForKey:@(rid)];
                [self.waiting removeObjectForKey:@(rid)];
            }
            return;
        }
        NSMutableDictionary *headers = [NSMutableDictionary new];
        if ([head[@"headers"] isKindOfClass:NSDictionary.class]) {
            [head[@"headers"] enumerateKeysAndObjectsUsingBlock:^(id key, id value, BOOL *stop) {
                (void)stop;
                headers[[key description]] = [value description];
            }];
        }
        NSString *mime = [head[@"mimeType"] isKindOfClass:NSString.class] ? head[@"mimeType"] : nil;
        NSString *encoding = [head[@"encoding"] isKindOfClass:NSString.class] ? head[@"encoding"] : nil;
        if (mime != nil && headers[@"Content-Type"] == nil) {
            headers[@"Content-Type"] = encoding != nil ? [NSString stringWithFormat:@"%@; charset=%@", mime, encoding]
                                                       : mime;
        }
        NSHTTPURLResponse *response = [[NSHTTPURLResponse alloc] initWithURL:task.request.URL statusCode:status
                                                                 HTTPVersion:@"HTTP/1.1" headerFields:headers];
        resource.headed = YES;
        [task didReceiveResponse:response];
        return;
    }
    if (fd >= 0) {
        if (offset > 0) lseek(fd, offset, SEEK_SET);
        uint8_t buffer[65536];
        int64_t left = length;
        while (left != 0) {
            const size_t want = left < 0 || left > (int64_t)sizeof buffer ? sizeof buffer : (size_t)left;
            const ssize_t got = pread(fd, buffer, want, offset);
            if (got <= 0) break;
            [task didReceiveData:[NSData dataWithBytes:buffer length:(NSUInteger)got]];
            offset += got;
            if (left > 0) left -= got;
        }
        close(fd);
        return;
    }
    if (!end) {
        [task didReceiveData:payload];
        return;
    }
    resource.stopped = YES;
    [task didFinish];
    [self.resources removeObjectForKey:@(rid)];
    [self.waiting removeObjectForKey:@(rid)];
}

@end

// MARK: The runtime's handler

/// A request copied off the guest's thread.
struct webview_request {
    uint32_t op;
    uint64_t token;
    uint64_t request_id;
    uint64_t window;
    int fd;
    int64_t offset;
    int64_t length;
};

static void on_main(struct webview_request request, NSData *payload, CGRect frame, CGRect clip) {
    NSNumber *key = @(request.token);
    MinatoWebView *view = views()[key];
    switch (request.op) {
        case APKRT_WEBVIEW_MAKE:
            if (view == nil) views()[key] = [[MinatoWebView alloc] initWithToken:request.token];
            break;
        case APKRT_WEBVIEW_REMOVE:
            [view remove];
            [views() removeObjectForKey:key];
            break;
        case APKRT_WEBVIEW_CALL: {
            NSDictionary *call = payload.length > 0 ? [NSJSONSerialization JSONObjectWithData:payload options:0
                                                                                        error:nil]
                                                    : nil;
            if ([call isKindOfClass:NSDictionary.class]) [view call:call requestId:request.request_id];
            break;
        }
        case APKRT_WEBVIEW_ANSWER:
            if (view != nil) {
                [view answer:request.request_id payload:payload fd:request.fd offset:request.offset
                      length:request.length];
            } else if (request.fd >= 0) {
                close(request.fd);
            }
            break;
        case APKRT_WEBVIEW_PLACE:
            [view placeIn:request.window frame:frame clip:clip];
            break;
        default:
            break;
    }
}

void minato_webview_set_windows(const struct minato_webview_windows *windows) {
    dispatch_async(dispatch_get_main_queue(), ^{
        host_windows = windows;
    });
}

void minato_webview_windows_changed(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        for (MinatoWebView *view in views().allValues) [view show];
    });
}

void minato_webview_handle(void *context, const struct apkrt_webview_call *call) {
    (void)context;
    struct webview_request request = {call->op, call->token, call->request_id, call->window,
                                      call->fd, call->offset, call->length};
    const CGRect frame = CGRectMake(call->left, call->top, call->right - call->left, call->bottom - call->top);
    const CGRect clip = CGRectMake(call->clip_left, call->clip_top, call->clip_right - call->clip_left,
                                   call->clip_bottom - call->clip_top);
    NSData *payload = call->payload != NULL && call->payload_length > 0
                          ? [NSData dataWithBytes:call->payload length:call->payload_length]
                          : (call->op == APKRT_WEBVIEW_ANSWER ? [NSData data] : nil);
    if (call->op == APKRT_WEBVIEW_MAKE) {
        apkrt_webview_post(call->token, APKRT_WEBVIEW_REC_CREATED, 0, "{}", 2);
    } else if (call->op == APKRT_WEBVIEW_PLACE) {
        apkrt_webview_post(call->token, APKRT_WEBVIEW_REC_PLACED, 0, "{}", 2);
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        on_main(request, payload, frame, clip);
    });
}
