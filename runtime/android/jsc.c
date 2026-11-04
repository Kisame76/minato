// libminato-jsc.so, the JavaScriptCore switch's guest half (ADR-0071): the loader preloads it when it is among the
// system's libraries, and calls minato_jsc_set_environment before the constructors of a library that defines
// JSGlobalContextCreate (JavaScriptCore, as react-native's jsc-android ships it) run. JSC's JIT reserves its code pool
// and then mprotects it executable (OSAllocatorPOSIX commit, WebKit); the guest's answer to that is EACCES, which JSC
// answers with a crash. JSC reads its options from the environment (JSC_<option> through getenv on Linux and Android,
// Options.cpp) and takes JavaScriptCoreUseJIT=0 as "no JIT" before it creates the pool (VM.cpp, enableAssembler), so
// with both set it starts its interpreter (LLInt) and never asks for executable memory. Both are set whatever the app
// set itself: a JIT would only crash here.
#include <stdlib.h>

void minato_jsc_set_environment(void) {
    setenv("JSC_useJIT", "0", 1);
    setenv("JavaScriptCoreUseJIT", "0", 1);
}
