// liblog for the host build tools, aidl (ADR-0045 decision 1) and aapt2 (ADR-0039 b1): the 10 functions that
// libbase's logging.cpp calls (system/libbase at android-15.0.0_r1), and the 3 that libutils, libcutils and
// libandroidfw call for aapt2, with liblog's host behavior and every message on stderr. The tools' log is
// diagnostics only; their errors reach stderr through their own writers.
//   libbase: __android_log_set_logger, __android_log_write_log_message, __android_log_logd_logger,
//   __android_log_buf_print, __android_log_is_loggable, __android_log_set_minimum_priority,
//   __android_log_get_minimum_priority, __android_log_set_default_tag, __android_log_set_aborter,
//   __android_log_call_aborter
//   aapt2's libraries: __android_log_print, __android_log_assert, and __android_log_error_write (the SafetyNet
//   event log, which has no reader on the host and is dropped)

#include <android/log.h>
#include <log/log_safetynet.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

const char kPriorities[] = "??VDIWEFS";

std::string& DefaultTag() {
  static std::string tag = getprogname();
  return tag;
}

void WriteToStderr(const __android_log_message* message) {
  int priority = message->priority;
  char letter = priority >= 0 && priority < static_cast<int>(sizeof(kPriorities) - 1) ? kPriorities[priority] : '?';
  const char* tag = message->tag != nullptr ? message->tag : DefaultTag().c_str();
  if (message->file != nullptr) {
    fprintf(stderr, "%s %c %s:%u] %s\n", tag, letter, message->file, message->line, message->message);
  } else {
    fprintf(stderr, "%s %c %s\n", tag, letter, message->message);
  }
}

void AbortWithMessage(const char*) {
  abort();
}

std::atomic<__android_logger_function> logger{WriteToStderr};
std::atomic<__android_aborter_function> aborter{AbortWithMessage};
// ANDROID_LOG_DEFAULT: each caller's default priority decides.
std::atomic<int32_t> minimum_priority{ANDROID_LOG_DEFAULT};

}  // namespace

extern "C" {

void __android_log_set_logger(__android_logger_function function) {
  logger = function != nullptr ? function : WriteToStderr;
}

int __android_log_is_loggable(int prio, const char*, int default_prio) {
  int32_t minimum = minimum_priority;
  return prio >= (minimum == ANDROID_LOG_DEFAULT ? default_prio : minimum);
}

void __android_log_write_log_message(__android_log_message* log_message) {
  if (log_message->buffer_id != LOG_ID_DEFAULT && log_message->buffer_id != LOG_ID_MAIN &&
      log_message->buffer_id != LOG_ID_SYSTEM && log_message->buffer_id != LOG_ID_RADIO &&
      log_message->buffer_id != LOG_ID_CRASH) {
    return;
  }
  if (log_message->tag == nullptr) log_message->tag = DefaultTag().c_str();
  logger.load()(log_message);
}

// On the host there is no logd: what would go there goes to stderr.
void __android_log_logd_logger(const __android_log_message* log_message) {
  WriteToStderr(log_message);
}

int __android_log_buf_print(int bufID, int prio, const char* tag, const char* fmt, ...) {
  char text[1024];
  va_list arguments;
  va_start(arguments, fmt);
  vsnprintf(text, sizeof(text), fmt, arguments);
  va_end(arguments);
  __android_log_message message = {sizeof(__android_log_message), bufID, prio, tag, nullptr, 0, text};
  WriteToStderr(&message);
  return 1;
}

int __android_log_print(int prio, const char* tag, const char* fmt, ...) {
  char text[1024];
  va_list arguments;
  va_start(arguments, fmt);
  vsnprintf(text, sizeof(text), fmt, arguments);
  va_end(arguments);
  __android_log_message message = {sizeof(__android_log_message), LOG_ID_MAIN, prio, tag, nullptr, 0, text};
  if (!__android_log_is_loggable(prio, tag, ANDROID_LOG_VERBOSE)) return 0;
  __android_log_write_log_message(&message);
  return 1;
}

void __android_log_assert(const char* cond, const char* tag, const char* fmt, ...) {
  char text[1024];
  if (fmt != nullptr) {
    va_list arguments;
    va_start(arguments, fmt);
    vsnprintf(text, sizeof(text), fmt, arguments);
    va_end(arguments);
  } else {
    snprintf(text, sizeof(text), "Assertion failed: %s", cond != nullptr ? cond : "(unknown)");
  }
  __android_log_message message = {sizeof(__android_log_message), LOG_ID_CRASH, ANDROID_LOG_FATAL, tag, nullptr, 0,
                                   text};
  WriteToStderr(&message);
  __android_log_call_aborter(text);
  abort();
}

int __android_log_error_write(int, const char*, int32_t, const char*, uint32_t) {
  return 0;
}

int32_t __android_log_set_minimum_priority(int32_t priority) {
  return minimum_priority.exchange(priority);
}

int32_t __android_log_get_minimum_priority() {
  return minimum_priority;
}

void __android_log_set_default_tag(const char* tag) {
  DefaultTag() = tag;
}

void __android_log_set_aborter(__android_aborter_function function) {
  aborter = function != nullptr ? function : AbortWithMessage;
}

void __android_log_call_aborter(const char* abort_message) {
  aborter.load()(abort_message);
}

}  // extern "C"
