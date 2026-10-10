// Minato (the Unity test APK probe): the log of failed resolutions. An app that catches the NoClassDefFoundError of a missing
// framework class, or whose library does, names no gap, and the next failure far from it is all a run shows (Unity's
// activity died on android.app.Dialog, and only a later NoSuchFieldError surfaced). So the class linker logs each
// class, method and field it fails to resolve, with the method that asks, as one warning with the tag minato-resolve:
// once per line, and no more than kMaxLines lines in a process, so a loop cannot flood the log. The method that asks
// is the one the verifier verifies on this thread, which resolves too, so a gap shows before its path runs, or else
// the innermost Java frame of the thread.

#ifndef ART_RUNTIME_MINATO_RESOLVE_LOG_H_
#define ART_RUNTIME_MINATO_RESOLVE_LOG_H_

#include <android/log.h>
#include <stddef.h>
#include <stdint.h>

#include <mutex>
#include <set>
#include <string>

#include "art_method.h"
#include "base/locks.h"
#include "dex/descriptors_names.h"
#include "dex/dex_file.h"
#include "thread.h"

namespace art {
namespace minato_resolve {

inline constexpr size_t kMaxLines = 256;

// The method the verifier verifies on this thread, if any.
struct Verifying {
  const DexFile* dex_file;
  uint32_t method_idx;
};
inline thread_local Verifying gVerifying{nullptr, 0u};

// Names the method being verified for the scope's resolutions, and the one before it again after them.
class VerifyingScope {
 public:
  VerifyingScope(const DexFile* dex_file, uint32_t method_idx) : saved_(gVerifying) {
    gVerifying = {dex_file, method_idx};
  }
  ~VerifyingScope() { gVerifying = saved_; }

 private:
  const Verifying saved_;
};

// "int a.B.f": DexFile::PrettyField names the type by its descriptor ("I"). Static or not, as the verifier's
// resolution (ResolveFieldJLS) does not know it, so a field's line is the same on every path.
inline std::string FieldName(const DexFile& dex_file, uint32_t field_idx) {
  const char* type = dex_file.GetFieldTypeDescriptor(dex_file.GetFieldId(field_idx));
  return PrettyDescriptor(type) + " " + dex_file.PrettyField(field_idx, /* with_type= */ false);
}

// Logs "missing <what> (from <method>)" the first time, with what being "class a.B", "method int a.B.m(int)", "field
// int a.B.f" and the like.
inline void Report(Thread* self, const std::string& what) REQUIRES_SHARED(Locks::mutator_lock_) {
  std::string from;
  if (gVerifying.dex_file != nullptr) {
    from = gVerifying.dex_file->PrettyMethod(gVerifying.method_idx, /* with_signature= */ false);
  } else {
    ArtMethod* method = self->GetCurrentMethod(nullptr, /* check_suspended= */ false, /* abort_on_error= */ false);
    from = method != nullptr ? method->PrettyMethod(/* with_signature= */ false) : "native code";
  }
  const std::string line = "missing " + what + " (from " + from + ")";
  static std::mutex lock;
  static std::set<std::string>* const seen = new std::set<std::string>();  // never freed, as threads may log late
  std::lock_guard<std::mutex> guard(lock);
  if (seen->size() > kMaxLines || !seen->insert(line).second) {
    return;
  }
  __android_log_write(ANDROID_LOG_WARN, "minato-resolve",
                      seen->size() > kMaxLines ? "further failed resolutions not logged" : line.c_str());
}

}  // namespace minato_resolve
}  // namespace art

#endif  // ART_RUNTIME_MINATO_RESOLVE_LOG_H_
