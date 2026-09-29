// ADR-0037 b): graphics.recorder's checks of a MinatoMetalLayer's recording of a run drawn in lockstep, for
// graphics-bench-host and graphics-app-test. posts[k] is when the test posted the tick of frame k, and the post
// after the last frame closes it. Every drawable once and in order: as many drawables as frames, none lost or nil,
// each presented handler run once and for its own drawable, the IDs rising, drawable k taken between posts[k] and
// posts[k + 1] (during frame k), and a present time of 0 (the Mac's layers off screen) or after its nextDrawable.
#ifndef RECORDING_CHECK_H
#define RECORDING_CHECK_H

#import "minato_layer.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

/// What is wrong with the recording, empty when nothing is.
inline std::vector<std::string> recording_problems(MinatoLayerRecording *recording,
                                                   const std::vector<std::int64_t> &posts, std::uint64_t frames) {
    std::vector<std::string> problems;
    const auto add = [&](const char *format, auto... values) {
        char text[256];
        std::snprintf(text, sizeof text, format, values...);
        problems.emplace_back(text);
    };
    if (recording == nil) {
        problems.emplace_back("no recording");
        return problems;
    }
    const std::uint64_t count = recording.count;
    if (count != frames) {
        add("recorded %llu drawables for %llu frames", (unsigned long long)count, (unsigned long long)frames);
    }
    if (recording.lost != 0) add("%llu drawables lost", (unsigned long long)recording.lost);
    if (recording.nils != 0) add("%llu nextDrawable calls returned nil", (unsigned long long)recording.nils);
    if (recording.unanswered != 0) {
        add("%llu presented handlers did not run", (unsigned long long)recording.unanswered);
    }
    if (posts.size() < count + 1) add("%zu ticks posted for %llu drawables", posts.size(), (unsigned long long)count);
    for (std::uint64_t index = 0; index < count && problems.size() < 12; ++index) {
        const struct minato_layer_drawable drawable = [recording drawable:index];
        if (drawable.handlers != 1) {
            add("drawable %llu: its presented handler ran %u times", (unsigned long long)index, drawable.handlers);
        }
        if (drawable.presented_id != drawable.drawable_id) {
            add("drawable %llu: ID %llu, its handler saw %llu", (unsigned long long)index,
                (unsigned long long)drawable.drawable_id, (unsigned long long)drawable.presented_id);
        }
        if (index > 0 && drawable.drawable_id <= [recording drawable:index - 1].drawable_id) {
            add("drawable %llu: ID %llu after %llu", (unsigned long long)index,
                (unsigned long long)drawable.drawable_id,
                (unsigned long long)[recording drawable:index - 1].drawable_id);
        }
        if (index + 1 < posts.size() &&
            (drawable.next_drawable_ns <= posts[index] || drawable.next_drawable_ns >= posts[index + 1])) {
            add("drawable %llu taken at %lld, outside frame %llu's ticks %lld to %lld", (unsigned long long)index,
                (long long)drawable.next_drawable_ns, (unsigned long long)index, (long long)posts[index],
                (long long)posts[index + 1]);
        }
        if (drawable.presented_ns != 0 && drawable.presented_ns <= drawable.next_drawable_ns) {
            add("drawable %llu presented at %lld, before it was taken at %lld", (unsigned long long)index,
                (long long)drawable.presented_ns, (long long)drawable.next_drawable_ns);
        }
    }
    return problems;
}

/// The recording's notes, for the test's output.
inline std::string recording_notes(MinatoLayerRecording *recording) {
    if (recording == nil) return "no recording";
    char text[256];
    std::snprintf(text, sizeof text, "QoS %s, pixel format %llu, %.0f x %.0f, at most %llu drawables",
                  recording.qosName.UTF8String, (unsigned long long)recording.pixelFormat,
                  recording.drawableSize.width, recording.drawableSize.height,
                  (unsigned long long)recording.maximumDrawableCount);
    return text;
}

#endif
