// log-test: liblog, the NDK's logging API (M2 step 5c). What a program can observe of it: the return
// values, which messages are loggable, and every message through a logger of its own. Messages for
// the default logger go to logcat on Android and to stderr in the runtime (log-test.err), and so does
// the text of __android_log_assert on both.
// Expected output: fixtures/expected/log-test.out and log-test.err
#include <android/log.h>
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>

static jmp_buf after_abort;

static void capture(const struct __android_log_message *message) {
    printf("logger: buffer %d, priority %d, tag %s, file %s:%u, message [%s]\n", message->buffer_id,
           message->priority, message->tag, message->file != NULL ? message->file : "(none)",
           message->file != NULL ? message->line : 0, message->message);
}

static void keep_running(const char *message) {
    printf("aborter: [%s]\n", message);
    longjmp(after_abort, 1);
}

static const char *result(int value) {
    return value == 1 ? "1" : value == -EPERM ? "-EPERM" : "unexpected";
}

int main(void) {
    printf("minimum priority at the start: %d\n", __android_log_get_minimum_priority());
    printf("loggable by default: verbose %d; with info as default: debug %d, info %d\n",
           __android_log_is_loggable(ANDROID_LOG_VERBOSE, "log-test", ANDROID_LOG_VERBOSE),
           __android_log_is_loggable(ANDROID_LOG_DEBUG, "log-test", ANDROID_LOG_INFO),
           __android_log_is_loggable(ANDROID_LOG_INFO, "log-test", ANDROID_LOG_INFO));
    printf("__android_log_write: %s\n", result(__android_log_write(ANDROID_LOG_INFO, "log-test", "plain message")));
    printf("__android_log_print: %s\n",
           result(__android_log_print(ANDROID_LOG_WARN, "log-test", "formatted %d %s", 42, "text")));

    printf("previous minimum priority: %d\n", __android_log_set_minimum_priority(ANDROID_LOG_WARN));
    printf("minimum priority now: %d\n", __android_log_get_minimum_priority());
    printf("below the minimum: %s\n", result(__android_log_write(ANDROID_LOG_INFO, "log-test", "dropped")));
    printf("above the minimum: %s\n", result(__android_log_write(ANDROID_LOG_ERROR, "log-test", "kept")));
    printf("loggable below the minimum, whatever the default: %d\n",
           __android_log_is_loggable_len(ANDROID_LOG_INFO, "log-test", 8, ANDROID_LOG_VERBOSE));
    __android_log_set_minimum_priority(ANDROID_LOG_DEFAULT);

    __android_log_set_logger(capture);
    __android_log_write(ANDROID_LOG_DEBUG, "tag", "to my logger");
    __android_log_buf_write(LOG_ID_SYSTEM, ANDROID_LOG_INFO, "tag", "system buffer");
    __android_log_buf_print(LOG_ID_MAIN, ANDROID_LOG_VERBOSE, "tag", "%s=%d", "answer", 42);
    __android_log_write(ANDROID_LOG_INFO, NULL, "without a tag");
    __android_log_set_default_tag("custom");
    __android_log_write(ANDROID_LOG_INFO, NULL, "with the default tag");
    struct __android_log_message message = {sizeof message, LOG_ID_MAIN, ANDROID_LOG_ERROR, "direct",
                                            "file.c", 42, "with a file"};
    __android_log_write_log_message(&message);

    __android_log_set_aborter(keep_running);
    if (setjmp(after_abort) == 0) __android_log_assert("x > 0", "assert", "failed: %d", 7);
    if (setjmp(after_abort) == 0) __android_log_assert("x > 0", "assert", NULL);
    if (setjmp(after_abort) == 0) __android_log_call_aborter("called directly");

    // Back to the device's logger: logd on Android, stderr in the runtime.
    __android_log_set_logger(__android_log_logd_logger);
    __android_log_write(ANDROID_LOG_FATAL, "log-test", "through the logd logger");
    __android_log_write(ANDROID_LOG_INFO, "short", "logcat pads the tag to 8 characters");
    // logcat prints each line of a message with the tag, as ART's stack traces come; a newline at the end makes
    // no line of its own, one in the middle an empty one, and so does an empty message.
    __android_log_write(ANDROID_LOG_WARN, "log-test", "a message\n\tof two lines");
    __android_log_write(ANDROID_LOG_WARN, "log-test", "a newline at the end\n");
    __android_log_write(ANDROID_LOG_WARN, "log-test", "before\n\nafter");
    __android_log_write(ANDROID_LOG_WARN, "log-test", "");
    printf("done\n");
    return 0;
}
