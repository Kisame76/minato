#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// ADR-0032, decision 2, the second way: asks LSApplicationWorkspace, a private class, twice to open the
/// app `bundleIdentifier`. The first request reaches this running process, the second starts Minato again
/// once the process is gone (LiveContainer's pattern; the idea only, no code). `answered` gets the answer
/// to the first request, on any thread; the caller ends the process when it opened. Returns NO, without
/// asking, when the class or its method is missing. All private API that Minato uses sits in this file (R10).
BOOL MinatoReopenThroughWorkspace(NSString *bundleIdentifier, void (^answered)(BOOL opened));

NS_ASSUME_NONNULL_END
