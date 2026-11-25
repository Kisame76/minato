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
#include <stdio.h>
#include <string.h>

#include <mutex>
#include <set>
#include <string>

#include "art_method.h"
#include "base/locks.h"
#include "dex/descriptors_names.h"
#include "dex/dex_file.h"
#include "mirror/throwable.h"
#include "obj_ptr.h"
#include "runtime.h"
#include "stack.h"
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

// The last line of this thread for the report of its uncaught pre-allocated NoClassDefFoundError
// (ReportUncaughtPreAllocated): its last miss in FindClass's boot branch and its last throw of that error elsewhere.
// Plain buffers, as a thread_local with a destructor needs the C++ runtime's thread exit hooks.
inline constexpr size_t kThreadLine = 400;  // per thread, in its static TLS: kept small
inline constexpr size_t kLastLine = 640;    // the rings' lines (Remember)
inline thread_local char gLastBootMiss[kThreadLine] = {};
inline thread_local char gLastThrow[kThreadLine] = {};
inline void Keep(char (&slot)[kThreadLine], const std::string& line) {
  snprintf(slot, kThreadLine, "%s", line.c_str());
}

// The latest misses of the boot branch at run time, of every thread, oldest first, for the report of an uncaught
// pre-allocated error: the error is one object, which a library may keep and throw on another thread. A line already
// held is not held twice.
inline constexpr size_t kRecent = 12;
inline constexpr size_t kOutside = 32;
template <size_t kSize>
struct RecentMisses {
  std::mutex lock;
  char lines[kSize][kLastLine] = {};
  size_t next = 0u;
};
// Every miss at run time; and those that code outside the platform reached (a library's or the app's frame on the
// thread's stack), which a library may keep and throw later, however long ago they were.
inline RecentMisses<kRecent>& Recent() {
  static RecentMisses<kRecent>* const recent = new RecentMisses<kRecent>();  // never freed, as threads may log late
  return *recent;
}
inline RecentMisses<kOutside>& Outside() {
  static RecentMisses<kOutside>* const outside = new RecentMisses<kOutside>();  // never freed, as above
  return *outside;
}
template <size_t kSize>
inline void Remember(RecentMisses<kSize>& recent, const std::string& line) {
  std::lock_guard<std::mutex> guard(recent.lock);
  for (const auto& held : recent.lines) {
    if (line.compare(0, kLastLine - 1, held) == 0) {
      return;
    }
  }
  snprintf(recent.lines[recent.next % kSize], kLastLine, "%s", line.c_str());
  ++recent.next;
}

template <size_t kSize>
inline void Print(RecentMisses<kSize>& recent, const std::string& prefix) {
  std::lock_guard<std::mutex> guard(recent.lock);
  const size_t held = recent.next < kSize ? recent.next : kSize;
  for (size_t index = 0u; index < held; ++index) {
    const std::string line = prefix + std::to_string(index + 1u) + "/" + std::to_string(held) + ": " +
                             recent.lines[(recent.next - held + index) % kSize];
    __android_log_write(ANDROID_LOG_WARN, "minato-resolve", line.c_str());
  }
}

// Set while FindClass's boot branch sets the pre-allocated error, which ReportBootMiss names, so that
// ReportPreAllocatedThrow leaves that throw alone.
inline thread_local bool gInBootMiss = false;
class BootMissScope {
 public:
  BootMissScope() { gInBootMiss = true; }
  ~BootMissScope() { gInBootMiss = false; }
};

// Logs `line` as a warning with the tag minato-resolve the first time, up to kMaxLines lines in a process for the
// lines the verifier's resolutions give and as many again for all others: verifying the framework's own classes
// names every platform class the boot class path lacks, which must not use up the lines of a lookup that fails at
// run time.
inline void Write(const std::string& line, bool verifier = false) {
  static std::mutex lock;
  static std::set<std::string>* const seen = new std::set<std::string>();  // never freed, as threads may log late
  static size_t counts[2] = {0u, 0u};
  std::lock_guard<std::mutex> guard(lock);
  size_t& count = counts[verifier ? 1 : 0];
  if (count > kMaxLines || seen->count(line) != 0u) {
    return;
  }
  seen->insert(line);
  ++count;
  __android_log_write(ANDROID_LOG_WARN, "minato-resolve",
                      count <= kMaxLines ? line.c_str()
                                         : verifier ? "further failed resolutions of the verifier not logged"
                                                    : "further failed resolutions not logged");
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
  Write("missing " + what + " (from " + from + ")", gVerifying.dex_file != nullptr);
}

// A dotted class or method name of the platform (java., javax., sun., android., com.android., dalvik., libcore.).
inline bool IsPlatformName(const std::string& name) {
  static constexpr const char* kPrefixes[] = {"java.", "javax.", "sun.", "android.", "com.android.", "dalvik.",
                                              "libcore."};
  for (const char* prefix : kPrefixes) {
    if (name.compare(0, strlen(prefix), prefix) == 0) {
      return true;
    }
  }
  return false;
}

// The library family of a dotted name, as the framework's SelfKill names it (com.android.minato.os), or null: an
// engine, Google Play's libraries, Kotlin's, the common ad, attribution and network libraries. Checked before the
// platform's prefixes, since Play's licensing interface lives in com.android.
inline const char* FamilyOf(const std::string& name) {
  static constexpr const char* kFamilies[][2] = {
      {"com.unity3d.", "unity"},
      {"org.godotengine.", "godot"},
      {"com.google.android.gms.", "play-services"},
      {"com.google.android.play.", "play-core"},
      {"com.google.android.vending.licensing.", "play-licensing"},
      {"com.android.vending.", "play-licensing"},
      {"com.google.firebase.", "firebase"},
      {"kotlin.", "kotlin"},
      {"kotlinx.", "kotlin"},
      {"com.facebook.", "facebook"},
      {"com.applovin.", "applovin"},
      {"com.ironsource.", "ironsource"},
      {"com.vungle.", "vungle"},
      {"com.adjust.", "adjust"},
      {"com.appsflyer.", "appsflyer"},
      {"com.chartboost.", "chartboost"},
      {"com.inmobi.", "inmobi"},
      {"com.mbridge.", "mintegral"},
      {"com.bytedance.", "pangle"},
      {"com.fyber.", "fyber"},
      {"io.bidmachine.", "bidmachine"},
      {"com.startapp.", "startapp"},
      {"okhttp3.", "okhttp"},
      {"com.squareup.", "square"},
  };
  for (const auto& family : kFamilies) {
    if (name.compare(0, strlen(family[0]), family[0]) == 0) {
      return family[1];
    }
  }
  return nullptr;
}

// A dotted name under the session log's mask, which a user's crash report may carry: a library family, the
// platform's own name, or `app` for every other name, so that an app's own classes are never named.
inline std::string Masked(const std::string& name, const char* app) {
  if (const char* family = FamilyOf(name)) {
    return family;
  }
  return IsPlatformName(name) ? name : std::string(app);
}

// The Java frames of this thread from the innermost on, masked: the platform's methods by name, library families by
// family, every other method as "app", a native method marked, a run of one name folded with its count, and at most
// kMaxCallers entries, then "+N more". "none" when the thread has no Java frame.
inline constexpr size_t kMaxCallers = 12;
inline std::string Callers(Thread* self, bool* outside = nullptr) REQUIRES_SHARED(Locks::mutator_lock_) {
  std::string line;
  std::string last;
  size_t run = 0u;
  size_t entries = 0u;
  size_t dropped = 0u;
  auto flush = [&]() {
    if (run == 0u) {
      return;
    }
    if (entries < kMaxCallers) {
      line += (line.empty() ? "" : " < ") + last + (run > 1u ? " x" + std::to_string(run) : "");
      ++entries;
    } else {
      dropped += run;
    }
  };
  StackVisitor::WalkStack(
      [&](const StackVisitor* visitor) REQUIRES_SHARED(Locks::mutator_lock_) {
        ArtMethod* method = visitor->GetMethod();
        if (method == nullptr || method->IsRuntimeMethod()) {
          return true;
        }
        const std::string pretty = method->PrettyMethod(/* with_signature= */ false);
        if (outside != nullptr && (FamilyOf(pretty) != nullptr || !IsPlatformName(pretty))) {
          *outside = true;
        }
        std::string name = Masked(pretty, "app");
        if (method->IsNative()) {
          name += " (native)";
        }
        if (name == last) {
          ++run;
        } else {
          flush();
          last = std::move(name);
          run = 1u;
        }
        return true;
      },
      self,
      /* context= */ nullptr,
      StackVisitor::StackWalkKind::kIncludeInlinedFrames,
      /* check_suspended= */ false);
  flush();
  if (line.empty()) {
    return "none";
  }
  return dropped > 0u ? line + " < +" + std::to_string(dropped) + " more" : line;
}

// A lookup that FindClass's boot branch misses (no class loader, so the boot class path only), which gets the
// pre-allocated NoClassDefFoundError without a name or a stack: JNI's FindClass from a platform method or from a
// thread with no Java frame and no system class loader, a boot class's own reference, Class.classForName with no
// loader. A platform class is named, as missing from the boot class path; any other class only by its library
// family or as "app". The asker is the method the verifier verifies, or else the thread's Java frames (Callers).
inline void ReportBootMiss(Thread* self, const char* descriptor) REQUIRES_SHARED(Locks::mutator_lock_) {
  const std::string name = PrettyDescriptor(descriptor);
  const char* family = FamilyOf(name);
  const std::string what = family == nullptr && IsPlatformName(name)
                               ? "class " + name + " (boot class path)"
                               : std::string("an app class with no class loader (") +
                                     (family != nullptr ? family : "app") + ")";
  if (gVerifying.dex_file != nullptr) {
    const std::string from =
        Masked(gVerifying.dex_file->PrettyMethod(gVerifying.method_idx, /* with_signature= */ false), "an app method");
    Write("missing " + what + " (from the verifier of " + from + ")", /* verifier= */ true);
    return;
  }
  bool outside = false;
  const std::string line = "missing " + what + " (callers: " + Callers(self, &outside) + ")";
  Keep(gLastBootMiss, line);
  const std::string tagged = "[t" + std::to_string(self->GetThreadId()) + "] " + line;
  Remember(Recent(), tagged);
  if (outside) {
    Remember(Outside(), tagged);
  }
  Write(line);
}

// The pre-allocated error that is the runtime's, once it started (the runtime makes it while it starts).
inline bool IsPreAllocatedNoClassDefFoundError(mirror::Object* exception) REQUIRES_SHARED(Locks::mutator_lock_) {
  Runtime* runtime = Runtime::Current();
  return exception != nullptr && runtime != nullptr && runtime->IsStarted() &&
         exception == runtime->GetPreAllocatedNoClassDefFoundError();
}

// The pre-allocated NoClassDefFoundError set as this thread's exception outside FindClass's boot branch (Thread's
// SetException): a rethrow of it, which Class.forName makes of a ClassNotFoundException's cause, the lookup of a
// runtime thread or of a thread that cannot load classes. Named with the thread's Java frames, once per line, and kept
// for the report of an uncaught one.
inline void ReportPreAllocatedThrow(Thread* self, ObjPtr<mirror::Throwable> exception)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (gInBootMiss || !IsPreAllocatedNoClassDefFoundError(exception.Ptr())) {
    return;
  }
  const char* kind = self->IsRuntimeThread()    ? "set on a runtime thread"
                     : !self->CanLoadClasses() ? "set on a thread that cannot load classes"
                                               : "rethrown";
  const std::string line =
      std::string("pre-allocated NoClassDefFoundError ") + kind + " (callers: " + Callers(self) + ")";
  Keep(gLastThrow, line);
  Write(line);
}

// An uncaught pre-allocated NoClassDefFoundError (Thread's HandleUncaughtExceptions), which carries no name and no
// stack: one line with this thread's last miss in the boot branch and its last throw of the error elsewhere, then the
// misses that code outside the platform reached and the recent misses of every thread (Remember), logged every time,
// outside the caps, as a thread ends so once at most.
inline void ReportUncaughtPreAllocated(ObjPtr<mirror::Object> exception) REQUIRES_SHARED(Locks::mutator_lock_) {
  if (!IsPreAllocatedNoClassDefFoundError(exception.Ptr())) {
    return;
  }
  std::string line = "uncaught pre-allocated NoClassDefFoundError; this thread's last boot class path miss: ";
  line += gLastBootMiss[0] != '\0' ? gLastBootMiss : "none";
  line += "; its last throw of the error elsewhere: ";
  line += gLastThrow[0] != '\0' ? gLastThrow : "none";
  __android_log_write(ANDROID_LOG_WARN, "minato-resolve", line.c_str());
  const std::string thread = "uncaught pre-allocated NoClassDefFoundError on [t" +
                             std::to_string(Thread::Current()->GetThreadId()) + "]: ";
  Print(Outside(), thread + "boot class path miss reached from outside the platform ");
  Print(Recent(), thread + "recent boot class path miss ");
}

}  // namespace minato_resolve
}  // namespace art

#endif  // ART_RUNTIME_MINATO_RESOLVE_LOG_H_
