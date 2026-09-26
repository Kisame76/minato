#import "RestartWorkspace.h"

#import <objc/message.h>

BOOL MinatoReopenThroughWorkspace(NSString *bundleIdentifier, void (^answered)(BOOL opened)) {
    Class workspaceClass = NSClassFromString(@"LSApplicationWorkspace");
    SEL defaultWorkspace = NSSelectorFromString(@"defaultWorkspace");
    SEL open = NSSelectorFromString(@"openApplicationWithBundleIdentifier:configuration:completionHandler:");
    if (workspaceClass == Nil || ![workspaceClass respondsToSelector:defaultWorkspace]) return NO;
    id workspace = ((id (*)(Class, SEL))objc_msgSend)(workspaceClass, defaultWorkspace);
    if (workspace == nil || ![workspace respondsToSelector:open]) return NO;

    // The handler gets a flag first, not an object: taken as an object, iOS 27's YES (1) crashed ARC's
    // retain (tools/session-check.sh, 2026-09-25). The second request gets a handler too, which does
    // nothing, since the process may still run when its answer comes.
    typedef void (^Completion)(BOOL opened, NSError *_Nullable error);
    Completion first = ^(BOOL opened, NSError *_Nullable error) {
        answered(opened && error == nil);
    };
    Completion second = ^(BOOL opened, NSError *_Nullable error) {
        (void)opened;
        (void)error;
    };
    ((void (*)(id, SEL, NSString *, id _Nullable, Completion))objc_msgSend)(workspace, open, bundleIdentifier, nil, first);
    ((void (*)(id, SEL, NSString *, id _Nullable, Completion))objc_msgSend)(workspace, open, bundleIdentifier, nil, second);
    return YES;
}
