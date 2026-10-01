// libart-compiler's entry for ART's JIT (ADR-0033 a). The compiler is left out: ART runs interpreted only in M4
// (-Xint, ADR-0033 decision 4), and the runtime creates its JIT only when one is on. Asked for one anyway, it ends
// the process with a message.
#include "jit_create.h"

#include <android-base/logging.h>

namespace art HIDDEN {
namespace jit {

JitCompilerInterface* jit_create() {
    LOG(FATAL) << "This ART has no JIT compiler: it runs interpreted only (ADR-0033)";
    UNREACHABLE();
}

}  // namespace jit
}  // namespace art
