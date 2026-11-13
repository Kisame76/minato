// Checks apk-run's headless host (headless_host.h, ADR-0043 c4, ADR-0041): its answers to the session's surfaces
// and requested windows, recorded instead of made, and its buffers, one per window, as apkrt.h's struct
// apkrt_window_buffers asks them to behave.
#include "headless_host.h"

#include "check.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

// --- Surfaces ------------------------------------------------------------------------------------

/// What the host did, in order: "add 1290x726 at 0,300 z -2 -> 2", "gate 2 open", "size 2 640x480", "remove 2",
/// the posts as "post <type> <a0> <a1> <a2>", and the places that reach the layer as "place 2 at 0,300 z -2".
struct Record {
    std::vector<std::string> calls;
    std::uint64_t next_window = 2;  // 1 is the main window's

    headless::Calls calls_of() {
        return {
            [this](const apkrt_surface_request &request) {
                calls.push_back("add " + std::to_string(request.width) + "x" + std::to_string(request.height) +
                                " at " + std::to_string(request.left) + "," + std::to_string(request.top) + " z " +
                                std::to_string(request.z) + " -> " + std::to_string(next_window));
                return next_window++;
            },
            [this](std::uint64_t window) { calls.push_back("gate " + std::to_string(window) + " open"); },
            [this](std::uint64_t window, std::int32_t width, std::int32_t height) {
                calls.push_back("size " + std::to_string(window) + " " + std::to_string(width) + "x" +
                                std::to_string(height));
            },
            [this](std::uint64_t window) { calls.push_back("remove " + std::to_string(window)); },
            [this](std::uint32_t type, std::uint64_t a0, std::uint64_t a1, std::uint64_t a2) {
                calls.push_back("post " + name(type) + " " + std::to_string(a0) + " " + std::to_string(a1) + " " +
                                std::to_string(a2));
            },
            [this](std::uint64_t window, const apkrt_surface_request &request) {
                calls.push_back("place " + std::to_string(window) + " at " + std::to_string(request.left) + "," +
                                std::to_string(request.top) + " z " + std::to_string(request.z));
            },
        };
    }

    static std::string name(std::uint32_t type) {
        switch (type) {
            case APKRT_EVENT_SURFACE_CREATED: return "created";
            case APKRT_EVENT_SURFACE_RESIZED: return "resized";
            case APKRT_EVENT_SURFACE_DESTROYED: return "destroyed";
            default: return std::to_string(type);
        }
    }

    std::vector<std::string> take() {
        auto taken = std::move(calls);
        calls.clear();
        return taken;
    }
};

std::string size(std::int32_t width, std::int32_t height) {
    return std::to_string(headless::pair(width, height));
}

/// A request as the runtime hands it to the host: checked, with its buffer's size.
apkrt_surface_request request(std::uint32_t op, std::uint64_t token, std::int32_t width = 1290,
                              std::int32_t height = 726) {
    apkrt_surface_request request{};
    request.op = op;
    request.token = token;
    if (op == APKRT_SURFACE_REMOVE) return request;
    request.z = -2;
    request.top = 300;
    request.right = 1290;
    request.bottom = 1026;
    request.width = width;
    request.height = height;
    request.format = 4;
    request.opaque = 1;
    return request;
}

TEST(a_make_gets_a_window_of_its_own_then_its_event_then_an_open_gate) {
    Record record;
    headless::Surfaces surfaces(record.calls_of());
    surfaces.request(request(APKRT_SURFACE_MAKE, 7));
    const std::vector<std::string> expected{"add 1290x726 at 0,300 z -2 -> 2", "post created 7 2 " + size(1290, 726),
                                            "gate 2 open"};
    CHECK(record.take() == expected);
}

TEST(a_place_reaches_the_layer_and_only_a_new_size_resizes_the_window_with_an_event) {
    Record record;
    headless::Surfaces surfaces(record.calls_of());
    surfaces.request(request(APKRT_SURFACE_MAKE, 7));
    record.take();
    // A move only, above the window now: the layer follows, without an event.
    auto moved = request(APKRT_SURFACE_PLACE, 7);
    moved.top = 0;
    moved.z = 1;
    surfaces.request(moved);
    const std::vector<std::string> move{"place 2 at 0,0 z 1"};
    CHECK(record.take() == move);
    surfaces.request(request(APKRT_SURFACE_PLACE, 7, 640, 480));
    const std::vector<std::string> expected{"place 2 at 0,300 z -2", "size 2 640x480",
                                            "post resized 7 2 " + size(640, 480)};
    CHECK(record.take() == expected);
    surfaces.request(request(APKRT_SURFACE_PLACE, 7, 640, 480));
    const std::vector<std::string> same{"place 2 at 0,300 z -2"};
    CHECK(record.take() == same);
}

TEST(a_remove_takes_the_window_away_without_an_event) {
    Record record;
    headless::Surfaces surfaces(record.calls_of());
    surfaces.request(request(APKRT_SURFACE_MAKE, 7));
    record.take();
    surfaces.request(request(APKRT_SURFACE_REMOVE, 7));
    const std::vector<std::string> expected{"remove 2"};
    CHECK(record.take() == expected);
    // Nothing is left of it.
    surfaces.request(request(APKRT_SURFACE_PLACE, 7, 640, 480));
    surfaces.request(request(APKRT_SURFACE_REMOVE, 7));
    CHECK(record.take().empty());
}

TEST(at_the_background_the_windows_go_first_and_the_events_come_later_in_the_order_made) {
    Record record;
    headless::Surfaces surfaces(record.calls_of());
    surfaces.request(request(APKRT_SURFACE_MAKE, 7));
    surfaces.request(request(APKRT_SURFACE_MAKE, 9));
    surfaces.request(request(APKRT_SURFACE_MAKE, 8));
    record.take();
    surfaces.set_background(true);
    surfaces.take_away();  // with the main window, before PAUSE
    const std::vector<std::string> removed{"remove 2", "remove 3", "remove 4"};
    CHECK(record.take() == removed);
    // The guest removes 9 on its own before the events: no event for it.
    surfaces.request(request(APKRT_SURFACE_REMOVE, 9));
    CHECK(record.take().empty());
    surfaces.post_destroyed();  // after PAUSE and the focus
    const std::vector<std::string> destroyed{"post destroyed 7 2 0", "post destroyed 8 4 0"};
    CHECK(record.take() == destroyed);
    surfaces.post_destroyed();
    CHECK(record.take().empty());
    // The guest's place and remove of a surface the host took away do nothing, its layer gone with its window;
    // the remove forgets it.
    surfaces.request(request(APKRT_SURFACE_PLACE, 7, 640, 480));
    surfaces.request(request(APKRT_SURFACE_REMOVE, 7));
    surfaces.request(request(APKRT_SURFACE_REMOVE, 8));
    surfaces.take_away();
    surfaces.post_destroyed();
    CHECK(record.take().empty());
}

TEST(a_surface_made_in_the_background_opens_its_gate_at_the_return) {
    Record record;
    headless::Surfaces surfaces(record.calls_of());
    surfaces.set_background(true);
    surfaces.request(request(APKRT_SURFACE_MAKE, 7));
    const std::vector<std::string> made{"add 1290x726 at 0,300 z -2 -> 2", "post created 7 2 " + size(1290, 726)};
    CHECK(record.take() == made);
    surfaces.set_background(false);
    const std::vector<std::string> opened{"gate 2 open"};
    CHECK(record.take() == opened);
    // A make again with the same token after the guest's remove is a new window.
    surfaces.take_away();
    surfaces.post_destroyed();
    surfaces.request(request(APKRT_SURFACE_REMOVE, 7));
    record.take();
    surfaces.request(request(APKRT_SURFACE_MAKE, 7));
    const std::vector<std::string> again{"add 1290x726 at 0,300 z -2 -> 3", "post created 7 3 " + size(1290, 726),
                                         "gate 3 open"};
    CHECK(record.take() == again);
}

// --- Windows (ADR-0041) --------------------------------------------------------------------------

/// What the host did with the requested windows, as Record has it for the surfaces: "add 1098x796 at 96,1000 type 2
/// -> 2", "place 2 at 96,1000 alpha 1065353216", the posts with the windows' event names.
struct WindowRecord {
    std::vector<std::string> calls;
    std::uint64_t next_window = 2;  // 1 is the main window's

    headless::WindowCalls calls_of() {
        return {
            [this](const apkrt_window_request &request) {
                calls.push_back("add " + std::to_string(request.width) + "x" + std::to_string(request.height) +
                                " at " + std::to_string(request.left) + "," + std::to_string(request.top) + " type " +
                                std::to_string(request.type) + " -> " + std::to_string(next_window));
                return next_window++;
            },
            [this](std::uint64_t window) { calls.push_back("gate " + std::to_string(window) + " open"); },
            [this](std::uint64_t window, std::int32_t width, std::int32_t height) {
                calls.push_back("size " + std::to_string(window) + " " + std::to_string(width) + "x" +
                                std::to_string(height));
            },
            [this](std::uint64_t window) { calls.push_back("remove " + std::to_string(window)); },
            [this](std::uint32_t type, std::uint64_t a0, std::uint64_t a1, std::uint64_t a2) {
                calls.push_back("post " + name(type) + " " + std::to_string(a0) + " " + std::to_string(a1) + " " +
                                std::to_string(a2));
            },
            [this](std::uint64_t window, const apkrt_window_request &request) {
                calls.push_back("place " + std::to_string(window) + " at " + std::to_string(request.left) + "," +
                                std::to_string(request.top) + " alpha " + std::to_string(request.alpha));
            },
        };
    }

    static std::string name(std::uint32_t type) {
        switch (type) {
            case APKRT_EVENT_WINDOW_CREATED: return "created";
            case APKRT_EVENT_WINDOW_RESIZED: return "resized";
            case APKRT_EVENT_WINDOW_DESTROYED: return "destroyed";
            default: return std::to_string(type);
        }
    }

    std::vector<std::string> take() {
        auto taken = std::move(calls);
        calls.clear();
        return taken;
    }
};

/// A window request as the runtime hands it to the host: checked, with its buffer's size; a dialog by default.
apkrt_window_request window_request(std::uint32_t op, std::uint64_t token, std::int32_t type = 2,
                                    std::int32_t width = 1098, std::int32_t height = 796) {
    apkrt_window_request request{};
    request.op = op;
    request.token = token;
    if (op == APKRT_WINDOW_REMOVE) return request;
    request.type = type;
    request.left = 96;
    request.top = 1000;
    request.right = 1194;
    request.bottom = 1796;
    request.width = width;
    request.height = height;
    request.format = -3;
    request.alpha = 0x3f800000;  // 1.0f
    return request;
}

TEST(a_windows_make_gets_a_window_of_its_own_then_its_event_then_an_open_gate) {
    WindowRecord record;
    headless::Windows windows(record.calls_of());
    windows.request(window_request(APKRT_WINDOW_MAKE, 7));
    const std::vector<std::string> expected{"add 1098x796 at 96,1000 type 2 -> 2",
                                            "post created 7 2 " + size(1098, 796), "gate 2 open"};
    CHECK(record.take() == expected);
}

TEST(a_windows_place_reaches_its_layer_and_only_a_new_size_resizes_it_with_an_event) {
    WindowRecord record;
    headless::Windows windows(record.calls_of());
    windows.request(window_request(APKRT_WINDOW_MAKE, 7));
    record.take();
    auto faded = window_request(APKRT_WINDOW_PLACE, 7);
    faded.top = 0;
    faded.alpha = 0x3f000000;  // 0.5f: the layer follows, without an event
    windows.request(faded);
    const std::vector<std::string> move{"place 2 at 96,0 alpha " + std::to_string(0x3f000000)};
    CHECK(record.take() == move);
    windows.request(window_request(APKRT_WINDOW_PLACE, 7, 2, 640, 480));
    const std::vector<std::string> expected{"place 2 at 96,1000 alpha " + std::to_string(0x3f800000),
                                            "size 2 640x480", "post resized 7 2 " + size(640, 480)};
    CHECK(record.take() == expected);
}

TEST(a_windows_remove_takes_it_away_without_an_event) {
    WindowRecord record;
    headless::Windows windows(record.calls_of());
    windows.request(window_request(APKRT_WINDOW_MAKE, 7));
    record.take();
    windows.request(window_request(APKRT_WINDOW_REMOVE, 7));
    const std::vector<std::string> expected{"remove 2"};
    CHECK(record.take() == expected);
    windows.request(window_request(APKRT_WINDOW_PLACE, 7, 2, 640, 480));
    windows.request(window_request(APKRT_WINDOW_REMOVE, 7));
    CHECK(record.take().empty());
}

TEST(at_the_background_the_windows_go_first_and_their_events_come_later_sub_windows_first) {
    WindowRecord record;
    headless::Windows windows(record.calls_of());
    windows.request(window_request(APKRT_WINDOW_MAKE, 7));         // a dialog: window 2
    windows.request(window_request(APKRT_WINDOW_MAKE, 8, 1000));   // its panel: 3
    windows.request(window_request(APKRT_WINDOW_MAKE, 9, 2005));   // a toast: 4
    windows.request(window_request(APKRT_WINDOW_MAKE, 10, 1001));  // a media sub-window on the session's window: 5
    record.take();
    windows.set_background(true);
    windows.take_away();  // with the main window, before PAUSE
    const std::vector<std::string> removed{"remove 2", "remove 3", "remove 4", "remove 5"};
    CHECK(record.take() == removed);
    // A window the guest makes after them, in the background, stays until the next take_away: no event for it.
    windows.request(window_request(APKRT_WINDOW_MAKE, 11));
    const std::vector<std::string> later{"add 1098x796 at 96,1000 type 2 -> 6", "post created 11 6 " + size(1098, 796)};
    CHECK(record.take() == later);
    windows.request(window_request(APKRT_WINDOW_REMOVE, 9));  // the toast ends on its own before the events
    CHECK(record.take().empty());
    windows.post_destroyed();  // after PAUSE and the focus: the sub-windows' first, each in the order made
    const std::vector<std::string> destroyed{"post destroyed 8 3 0", "post destroyed 10 5 0", "post destroyed 7 2 0"};
    CHECK(record.take() == destroyed);
    windows.post_destroyed();
    CHECK(record.take().empty());
    for (const std::uint64_t token : {8ull, 10ull, 7ull}) windows.request(window_request(APKRT_WINDOW_REMOVE, token));
    windows.take_away();
    windows.post_destroyed();
    const std::vector<std::string> at_end{"remove 6", "post destroyed 11 6 0"};
    CHECK(record.take() == at_end);
}

TEST(a_window_made_in_the_background_opens_its_gate_at_the_return) {
    WindowRecord record;
    headless::Windows windows(record.calls_of());
    windows.set_background(true);
    windows.request(window_request(APKRT_WINDOW_MAKE, 7));
    const std::vector<std::string> made{"add 1098x796 at 96,1000 type 2 -> 2", "post created 7 2 " + size(1098, 796)};
    CHECK(record.take() == made);
    windows.set_background(false);
    const std::vector<std::string> opened{"gate 2 open"};
    CHECK(record.take() == opened);
}

// --- Buffers -------------------------------------------------------------------------------------

apkrt_window_buffer buffer(std::int32_t width, std::int32_t height, std::int32_t format = 1) {
    apkrt_window_buffer buffer{};
    buffer.width = width;
    buffer.height = height;
    buffer.format = format;
    buffer.dirty[0] = 10;
    buffer.dirty[1] = 20;
    buffer.dirty[2] = 30;
    buffer.dirty[3] = 40;
    return buffer;
}

bool dirty(const apkrt_window_buffer &buffer, std::int32_t left, std::int32_t top, std::int32_t right,
           std::int32_t bottom) {
    return buffer.dirty[0] == left && buffer.dirty[1] == top && buffer.dirty[2] == right && buffer.dirty[3] == bottom;
}

TEST(each_window_has_a_buffer_of_its_own_so_two_locks_hold_at_once) {
    headless::Buffers buffers;
    auto main = buffer(1290, 2796);
    auto surface = buffer(640, 480, 4);
    CHECK(buffers.lock(1, main) == 0);
    CHECK(buffers.lock(2, surface) == 0);
    CHECK(main.bits != nullptr && surface.bits != nullptr && main.bits != surface.bits);
    CHECK(main.stride == 1290 && surface.stride == 640);
    std::memset(main.bits, 0xff, 1290 * 2796 * 4);
    std::memset(surface.bits, 0xff, 640 * 480 * 2);
    auto again = buffer(1290, 2796);
    CHECK(buffers.lock(1, again) == -EBUSY);
    bool first = false;
    CHECK(buffers.post(1, first) == 0 && first);
    CHECK(buffers.post(2, first) == 0 && first);
    CHECK(buffers.lock(1, again) == 0);
    CHECK(buffers.post(1, first) == 0 && !first);
}

TEST(a_lock_dirties_the_whole_buffer_when_it_is_new_and_the_guests_rectangle_cut_to_it_after) {
    headless::Buffers buffers;
    auto first_lock = buffer(100, 50);
    CHECK(buffers.lock(1, first_lock) == 0 && dirty(first_lock, 0, 0, 100, 50));
    bool first = false;
    CHECK(buffers.post(1, first) == 0);
    auto inside = buffer(100, 50);
    CHECK(buffers.lock(1, inside) == 0 && dirty(inside, 10, 20, 30, 40));
    CHECK(buffers.post(1, first) == 0);
    auto beyond = buffer(100, 50);
    beyond.dirty[0] = -5;
    beyond.dirty[2] = 500;
    CHECK(buffers.lock(1, beyond) == 0 && dirty(beyond, 0, 20, 100, 40));
    CHECK(buffers.post(1, first) == 0);
    auto nothing = buffer(100, 50);
    nothing.dirty[0] = 200;
    nothing.dirty[2] = 300;
    CHECK(buffers.lock(1, nothing) == 0 && dirty(nothing, 0, 0, 0, 0));
    CHECK(buffers.post(1, first) == 0);
    auto other_format = buffer(100, 50, 2);
    CHECK(buffers.lock(1, other_format) == 0 && dirty(other_format, 0, 0, 100, 50));
    CHECK(buffers.post(1, first) == 0);
}

TEST(a_lock_refuses_what_it_cannot_hold_and_a_post_without_a_lock) {
    headless::Buffers buffers;
    auto rgb = buffer(100, 50, 3);  // RGB_888: not one of the window formats
    CHECK(buffers.lock(1, rgb) == -EINVAL);
    auto empty = buffer(0, 50);
    CHECK(buffers.lock(1, empty) == -EINVAL);
    bool first = false;
    CHECK(buffers.post(1, first) == -EINVAL);
    CHECK(buffers.post(5, first) == -EINVAL);
    auto once = buffer(100, 50);
    CHECK(buffers.lock(3, once) == 0 && buffers.post(3, first) == 0);
    CHECK(buffers.post(3, first) == -EINVAL);  // its lock is posted already
}

TEST(a_removed_window_keeps_its_locked_buffer_until_the_post_and_takes_no_lock_after) {
    headless::Buffers buffers;
    auto locked = buffer(100, 50);
    CHECK(buffers.lock(1, locked) == 0);
    buffers.removed(1);
    std::memset(locked.bits, 0xff, 100 * 50 * 4);  // still the writer's until its post
    bool first = false;
    CHECK(buffers.post(1, first) == 0);
    auto later = buffer(100, 50);
    CHECK(buffers.lock(1, later) == -ENODEV);
    auto unlocked = buffer(100, 50);
    CHECK(buffers.lock(2, unlocked) == 0 && buffers.post(2, first) == 0);
    buffers.removed(2);
    CHECK(buffers.lock(2, unlocked) == -ENODEV);
    CHECK(buffers.post(2, first) == -EINVAL);
}

// The frame counts (the corpus harness's "frame" level): each window's posts, and after a mark whether a frame differs.

/// One frame of window `id`, its 100 x 50 RGBA_8888 pixels all `value`: a lock, the drawing and the post.
bool draw(headless::Buffers &buffers, std::uint64_t id, int value) {
    auto frame = buffer(100, 50);
    if (buffers.lock(id, frame) != 0) return false;
    std::memset(frame.bits, value, 100 * 50 * 4);
    bool first = false;
    return buffers.post(id, first) == 0;
}

/// Window `id`'s line of since_mark(), or one with ID 0 without it.
headless::Buffers::Since since(headless::Buffers &buffers, std::uint64_t id) {
    for (const auto &window : buffers.since_mark()) {
        if (window.id == id) return window;
    }
    return {};
}

TEST(a_mark_counts_the_posts_after_it_and_tells_whether_a_frame_differed_from_the_last_before) {
    headless::Buffers buffers;
    CHECK(draw(buffers, 1, 1) && draw(buffers, 1, 1) && draw(buffers, 1, 2));
    buffers.mark();
    auto window = since(buffers, 1);
    CHECK(window.id == 1 && window.posts == 3 && window.since == 0 && !window.changed && !window.made &&
          !window.removed);
    buffers.mark();
    CHECK(draw(buffers, 1, 2));  // the same picture again
    window = since(buffers, 1);
    CHECK(window.posts == 4 && window.since == 1 && !window.changed);
    buffers.mark();
    CHECK(draw(buffers, 1, 3) && draw(buffers, 1, 2));  // another one, then back: it changed after the mark
    window = since(buffers, 1);
    CHECK(window.posts == 6 && window.since == 2 && window.changed);
    buffers.mark();
    CHECK(!since(buffers, 1).changed);
}

TEST(a_window_whose_buffer_a_lock_holds_at_the_mark_takes_its_next_post_as_the_frame_before) {
    headless::Buffers buffers;
    CHECK(draw(buffers, 1, 1));
    auto drawing = buffer(100, 50);
    CHECK(buffers.lock(1, drawing) == 0);  // a frame begun before the mark
    buffers.mark();
    std::memset(drawing.bits, 7, 100 * 50 * 4);  // and drawn after it
    bool first = false;
    CHECK(buffers.post(1, first) == 0);
    CHECK(draw(buffers, 1, 7));
    auto window = since(buffers, 1);
    CHECK(window.since == 2 && !window.changed);  // the frame before the touch, then the same again
    CHECK(buffers.lock(1, drawing) == 0);
    buffers.mark();
    CHECK(buffers.post(1, first) == 0 && draw(buffers, 1, 8));
    CHECK(since(buffers, 1).changed);
    auto fresh = buffer(100, 50);  // a window that never posted takes its first post as the frame before
    CHECK(buffers.lock(3, fresh) == 0);
    buffers.mark();
    CHECK(buffers.post(3, first) == 0);
    window = since(buffers, 3);
    CHECK(window.id == 3 && window.since == 1 && !window.changed && !window.made);
}

TEST(a_window_made_after_the_mark_is_new_and_one_removed_after_it_says_so_until_the_next_mark) {
    headless::Buffers buffers;
    CHECK(draw(buffers, 1, 1) && draw(buffers, 2, 1));
    buffers.removed(2);
    buffers.mark();
    CHECK(draw(buffers, 1, 2) && draw(buffers, 3, 1) && draw(buffers, 3, 1));
    buffers.removed(1);
    const auto windows = buffers.since_mark();
    CHECK(windows.size() == 2);  // window 2 went before the mark
    auto window = since(buffers, 1);
    CHECK(window.posts == 2 && window.since == 1 && window.changed && window.removed && !window.made);
    window = since(buffers, 3);
    CHECK(window.posts == 2 && window.since == 2 && window.made && !window.removed);
    buffers.mark();
    CHECK(since(buffers, 1).id == 0 && since(buffers, 3).id == 3 && !since(buffers, 3).made);
}

}  // namespace

int main() {
    return apk::testing::run_all();
}
