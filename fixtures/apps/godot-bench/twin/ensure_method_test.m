// The Mac check of ensure_method.m (tools/godot-check.py twin): a delegate without a window gets the twin's
// fallback once, and one with its own window, also an inherited one, keeps it. A layer whose context variable is nil
// gets the context made for it once and holds it strongly; one with a context keeps it, and an object without such a
// variable, with one of another type, or for which no context can be made is left as it is (c4.1).

#import <Foundation/Foundation.h>
#import <objc/message.h>

#import "ensure_method.h"

@interface Plain : NSObject
@end
@implementation Plain
@end

@interface Own : NSObject
@end
@implementation Own
- (id)window {
    return @"own";
}
@end

@interface Inherited : Own
@end
@implementation Inherited
@end

// As Godot's GDTOpenGLLayer holds its EAGLContext.
@interface Layer : NSObject {
  @public
    id context;
}
@end
@implementation Layer
@end

@interface Counter : NSObject {
  @public
    int context;
}
@end
@implementation Counter
@end

static id fallback(id self, SEL command) {
    (void)self;
    (void)command;
    return @"fallback";
}

static int failures = 0;

static void check(BOOL condition, const char *what) {
    printf("%s%s\n", condition ? "ok      " : "FAILED  ", what);
    failures += condition ? 0 : 1;
}

static id window(id object) {
    return [object respondsToSelector:@selector(window)] ? ((id(*)(id, SEL))objc_msgSend)(object, @selector(window)) : nil;
}

int main(void) {
    @autoreleasepool {
        Plain *plain = [Plain new];
        check(minato_godot_ensure_method(plain, @selector(window), (IMP)fallback, "@@:"), "a delegate without a window gets one");
        check([window(plain) isEqual:@"fallback"], "its window is the fallback's");
        check(!minato_godot_ensure_method(plain, @selector(window), (IMP)fallback, "@@:"), "it gets it once");
        Own *own = [Own new];
        check(!minato_godot_ensure_method(own, @selector(window), (IMP)fallback, "@@:") && [window(own) isEqual:@"own"],
              "a delegate with its own window keeps it");
        Inherited *inherited = [Inherited new];
        check(!minato_godot_ensure_method(inherited, @selector(window), (IMP)fallback, "@@:") &&
                  [window(inherited) isEqual:@"own"],
              "an inherited window is kept too");
        check(!minato_godot_ensure_method(nil, @selector(window), (IMP)fallback, "@@:"), "no delegate gets nothing");

        __block int made = 0;
        __block __weak id lastMade = nil;
        id (^make)(void) = ^id {
            made += 1;
            id context = [NSObject new];
            lastMade = context;
            return context;
        };
        Layer *layer = [Layer new];
        @autoreleasepool {
            check(minato_godot_ensure_ivar(layer, "context", make), "a layer without a context gets one");
        }
        check(made == 1 && lastMade != nil && layer->context == lastMade, "it holds the one made for it, strongly");
        check(!minato_godot_ensure_ivar(layer, "context", make) && made == 1, "it gets it once");
        Layer *held = [Layer new];
        held->context = @"own";
        check(!minato_godot_ensure_ivar(held, "context", make) && [held->context isEqual:@"own"] && made == 1,
              "a layer with a context keeps it");
        check(!minato_godot_ensure_ivar(plain, "context", make) && made == 1, "an object without the variable is left alone");
        Counter *counter = [Counter new];
        check(!minato_godot_ensure_ivar(counter, "context", make) && counter->context == 0 && made == 1,
              "a variable of another type is left alone");
        Layer *failing = [Layer new];
        check(!minato_godot_ensure_ivar(failing, "context", ^id { return nil; }) && failing->context == nil,
              "a layer whose context cannot be made stays without one");
        check(!minato_godot_ensure_ivar(nil, "context", make) && made == 1, "no layer gets nothing");
    }
    printf("ensure_method: %s\n", failures ? "failed" : "ok");
    return failures ? 1 : 0;
}
