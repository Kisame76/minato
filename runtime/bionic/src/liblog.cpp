// Minato's liblog.so (ADR-0021): the NDK's logging API (android/log.h), and what platform code calls beyond it
// (ADR-0033 a), guest code next to bionic. Android's liblog sends messages to the logd service; there is none here, so the device's
// logger writes "<priority>/<tag>: <message>" lines to stderr, which the runtime owns. Everything a
// program can observe follows the NDK's documentation: return values, loggability (no log.tag
// properties exist here, so only the minimum priority and the caller's default count), loggers,
// the default tag and the aborter.
#include <android/log.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern "C" void android_set_abort_message(const char* message);

namespace {

// Android's liblog formats printf-style messages into a buffer of this size (LOG_BUF_SIZE).
constexpr size_t kBufferSize = 1024;
// The largest message logd takes (LOGGER_ENTRY_MAX_PAYLOAD); the default tag is cut to it.
constexpr size_t kMaxPayload = 4068;

// Settings any thread may change; read and written with __atomic builtins.
int32_t minimum_priority = ANDROID_LOG_DEFAULT;
__android_logger_function logger = __android_log_logd_logger;
__android_aborter_function aborter = __android_log_default_aborter;
char default_tag[kMaxPayload];
bool default_tag_set = false;

const char* tag_or_default(const char* tag) {
  if (tag != nullptr) return tag;
  return __atomic_load_n(&default_tag_set, __ATOMIC_ACQUIRE) ? default_tag : getprogname();
}

// Writes all of `text` to stderr, as far as it goes.
void write_all(const char* text, size_t length) {
  while (length > 0) {
    const ssize_t written = write(STDERR_FILENO, text, length);
    if (written <= 0) {
      if (written < 0 && errno == EINTR) continue;
      return;
    }
    text += written;
    length -= static_cast<size_t>(written);
  }
}

char priority_character(int32_t priority) {
  static const char characters[] = "??VDIWEF";  // UNKNOWN and DEFAULT have none
  return priority >= 0 && priority < ANDROID_LOG_SILENT ? characters[priority] : 'F';
}

int write_message(int buffer_id, int priority, const char* tag, const char* message) {
  if (!__android_log_is_loggable(priority, tag, ANDROID_LOG_VERBOSE)) return -EPERM;
  __android_log_message log_message = {sizeof(__android_log_message), buffer_id, priority, tag, nullptr, 0,
                                       message};
  __android_log_write_log_message(&log_message);
  return 1;
}

}  // namespace

extern "C" {

int __android_log_is_loggable_len(int priority, const char*, size_t, int default_priority) {
  const int32_t minimum = __atomic_load_n(&minimum_priority, __ATOMIC_RELAXED);
  return priority >= (minimum != ANDROID_LOG_DEFAULT ? minimum : default_priority) ? 1 : 0;
}

int __android_log_is_loggable(int priority, const char* tag, int default_priority) {
  return __android_log_is_loggable_len(priority, tag, tag != nullptr ? strlen(tag) : 0, default_priority);
}

int32_t __android_log_set_minimum_priority(int32_t priority) {
  return __atomic_exchange_n(&minimum_priority, priority, __ATOMIC_RELAXED);
}

int32_t __android_log_get_minimum_priority() {
  return __atomic_load_n(&minimum_priority, __ATOMIC_RELAXED);
}

void __android_log_set_default_tag(const char* tag) {
  strlcpy(default_tag, tag != nullptr ? tag : "", sizeof default_tag);
  __atomic_store_n(&default_tag_set, true, __ATOMIC_RELEASE);
}

void __android_log_set_logger(__android_logger_function function) {
  __atomic_store_n(&logger, function, __ATOMIC_RELEASE);
}

void __android_log_set_aborter(__android_aborter_function function) {
  __atomic_store_n(&aborter, function, __ATOMIC_RELEASE);
}

void __android_log_call_aborter(const char* message) {
  __atomic_load_n(&aborter, __ATOMIC_ACQUIRE)(message);
}

void __android_log_default_aborter(const char* message) {
  android_set_abort_message(message);
  abort();
}

// The device's logger: logd on Android, stderr here, as `logcat -v tag` prints a message.
void __android_log_logd_logger(const __android_log_message* message) {
  char line[kMaxPayload + 64];
  const int length = snprintf(line, sizeof line, "%c/%-8s: %s\n", priority_character(message->priority),
                              message->tag != nullptr ? message->tag : "", message->message);
  if (length > 0) write_all(line, static_cast<size_t>(length) < sizeof line ? static_cast<size_t>(length) : sizeof line - 1);
}

// As Android's: tag, priority, local time, process and thread, then the message.
void __android_log_stderr_logger(const __android_log_message* message) {
  time_t now = time(nullptr);
  tm local;
  localtime_r(&now, &local);
  char timestamp[32];
  strftime(timestamp, sizeof timestamp, "%m-%d %H:%M:%S", &local);
  const char* tag = message->tag != nullptr ? message->tag : "nullptr";
  if (message->file != nullptr) {
    fprintf(stderr, "%s %c %s %5d %5d %s:%u] %s\n", tag, priority_character(message->priority), timestamp,
            getpid(), gettid(), message->file, message->line, message->message);
  } else {
    fprintf(stderr, "%s %c %s %5d %5d %s\n", tag, priority_character(message->priority), timestamp, getpid(),
            gettid(), message->message);
  }
}

void __android_log_write_log_message(__android_log_message* message) {
  if (message->buffer_id != LOG_ID_DEFAULT && (message->buffer_id < LOG_ID_MIN || message->buffer_id >= LOG_ID_MAX)) {
    return;
  }
  message->tag = tag_or_default(message->tag);
  __atomic_load_n(&logger, __ATOMIC_ACQUIRE)(message);
}

int __android_log_buf_write(int buffer_id, int priority, const char* tag, const char* text) {
  return write_message(buffer_id, priority, tag, text);
}

int __android_log_write(int priority, const char* tag, const char* text) {
  return write_message(LOG_ID_MAIN, priority, tag, text);
}

int __android_log_vprint(int priority, const char* tag, const char* format, va_list arguments) {
  if (!__android_log_is_loggable(priority, tag, ANDROID_LOG_VERBOSE)) return -EPERM;
  char buffer[kBufferSize];
  vsnprintf(buffer, sizeof buffer, format, arguments);
  return write_message(LOG_ID_MAIN, priority, tag, buffer);
}

int __android_log_print(int priority, const char* tag, const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  const int result = __android_log_vprint(priority, tag, format, arguments);
  va_end(arguments);
  return result;
}

int __android_log_buf_print(int buffer_id, int priority, const char* tag, const char* format, ...) {
  if (!__android_log_is_loggable(priority, tag, ANDROID_LOG_VERBOSE)) return -EPERM;
  char buffer[kBufferSize];
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(buffer, sizeof buffer, format, arguments);
  va_end(arguments);
  return write_message(buffer_id, priority, tag, buffer);
}

void __android_log_assert(const char* condition, const char* tag, const char* format, ...) {
  char buffer[kBufferSize];
  if (format != nullptr) {
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(buffer, sizeof buffer, format, arguments);
    va_end(arguments);
  } else if (condition != nullptr) {
    snprintf(buffer, sizeof buffer, "Assertion failed: %s", condition);
  } else {
    strlcpy(buffer, "Unspecified assertion failed", sizeof buffer);
  }
  // Also on stderr, as the NDK documents it.
  write_all(buffer, strlen(buffer));
  write_all("\n", 1);
  __android_log_write(ANDROID_LOG_FATAL, tag, buffer);
  __android_log_call_aborter(buffer);
  abort();
}

// Platform code's SafetyNet event (log/log_safetynet.h, ADR-0033 a), which libziparchive and ART write on a malformed
// input. Android puts it into the events buffer, whose binary entries logcat leaves out unless asked; the guest has
// no events buffer, so the event goes nowhere, and the call reports success as a write does.
int __android_log_error_write(int, const char*, int32_t, const char*, uint32_t) {
  return 1;
}

// Platform code's events (log/log_event_list.h, ADR-0033 a), which ART writes on lock contention: a context collects
// a list of values for the events buffer. The guest has none (see __android_log_error_write): a context holds only
// its tag, the values go nowhere, and the list's write reports success. As Android's, the calls fail with -EBADF
// without a context from create_android_logger.
struct android_log_context_internal {
  uint32_t tag;
};
typedef android_log_context_internal* android_log_context;

android_log_context create_android_logger(uint32_t tag) {
  auto* context = static_cast<android_log_context>(malloc(sizeof(android_log_context_internal)));
  if (context != nullptr) context->tag = tag;
  return context;
}

int android_log_write_list_begin(android_log_context context) {
  return context != nullptr ? 0 : -EBADF;
}

int android_log_write_list_end(android_log_context context) {
  return context != nullptr ? 0 : -EBADF;
}

int android_log_write_int32(android_log_context context, int32_t) {
  return context != nullptr ? 0 : -EBADF;
}

int android_log_write_int64(android_log_context context, int64_t) {
  return context != nullptr ? 0 : -EBADF;
}

int android_log_write_string8(android_log_context context, const char*) {
  return context != nullptr ? 0 : -EBADF;
}

int android_log_write_string8_len(android_log_context context, const char*, size_t) {
  return context != nullptr ? 0 : -EBADF;
}

int android_log_write_float32(android_log_context context, float) {
  return context != nullptr ? 0 : -EBADF;
}

int android_log_write_list(android_log_context context, log_id_t) {
  return context != nullptr ? 1 : -EBADF;
}

int android_log_destroy(android_log_context* context) {
  if (context == nullptr || *context == nullptr) return -EBADF;
  free(*context);
  *context = nullptr;
  return 0;
}

}  // extern "C"
