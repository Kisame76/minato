// The Mac check of ensure_method.m (tools/godot-check.py twin): a delegate without a window gets the twin's
// fallback once, and one with its own window, also an inherited one, keeps it.

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
    }
    printf("ensure_method: %s\n", failures ? "failed" : "ok");
    return failures ? 1 : 0;
}
