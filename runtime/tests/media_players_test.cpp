// Checks the guest's media players (ADR-0060 e2e1): media_player's operations through the syscall dispatcher, as the
// framework's MediaPlayer natives make them, against a fake table of host players: the IDs the runtime keeps, the
// limits and checks that keep the host sound against hostile values, what reaches the host and what comes back, the
// records the host posts as the guest reads them from the event descriptor (their order, a frame refused rather than
// put before a waiting record, a full pipe), a callback source's READ and DATA, the frames' tokens, and a DESTROY
// that waits for a call still in the host. graphics/host's player on AVPlayer is checked by the fixture
// media-player-test against Android 15.
#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"
#include "descriptors.h"
#include "sockets.h"
#include "syscall_frame.h"
#include "syscalls.h"

#include "check.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef APKRT_RUNTIME_DIR
#error "APKRT_RUNTIME_DIR names runtime/, for the NR numbers' check"
#endif

namespace {

constexpr std::int64_t eagain = 11, ebadf = 9, ebusy = 16, efault = 14, eio = 5, einval = 22, enodev = 19, enospc = 28,
                       enosys = 38;
constexpr std::uint64_t linux_read = 63, linux_close = 57;

static_assert(sizeof(apkrt_media_player) == 96);
static_assert(offsetof(apkrt_media_player, kind) == 8 && offsetof(apkrt_media_player, flags) == 12);
static_assert(offsetof(apkrt_media_player, fd) == 16 && offsetof(apkrt_media_player, events) == 20);
static_assert(offsetof(apkrt_media_player, width) == 24 && offsetof(apkrt_media_player, height) == 28);
static_assert(offsetof(apkrt_media_player, data) == 32 && offsetof(apkrt_media_player, size) == 40);
static_assert(offsetof(apkrt_media_player, offset) == 48 && offsetof(apkrt_media_player, length) == 56);
static_assert(offsetof(apkrt_media_player, time_us) == 64 && offsetof(apkrt_media_player, duration_us) == 72);
static_assert(offsetof(apkrt_media_player, token) == 80 && offsetof(apkrt_media_player, value) == 88);
static_assert(sizeof(apkrt_media_player_event) == 32 && offsetof(apkrt_media_player_event, value) == 16);
static_assert(offsetof(apkrt_media_player_event, token) == 24);
static_assert(sizeof(apkrt_media_player_track) == 64);

std::int64_t syscall(std::uint64_t number, std::uint64_t a = 0, std::uint64_t b = 0, std::uint64_t c = 0) {
    apkrt_syscall_frame frame{};
    frame.x[0] = a;
    frame.x[1] = b;
    frame.x[2] = c;
    frame.x[8] = number;
    apkrt_syscall_dispatch(&frame);
    return static_cast<std::int64_t>(frame.x[0]);
}

std::int64_t call(std::uint64_t op, std::uint64_t argument) {
    return syscall(APKRT_NR_MEDIA_PLAYER, op, argument);
}

std::int64_t call(std::uint64_t op, apkrt_media_player &player) {
    return call(op, reinterpret_cast<std::uint64_t>(&player));
}

/// A fake table of host players, with a log of the calls, for any thread.
struct Fake {
    struct Player {
        std::uint64_t key = 0;  // create's `player`
        apkrt_media_player_source_info source{};
        std::string url;
        std::string fd_bytes;  // what the source's descriptor reads, from its offset, for its length
        int prepares = 0, starts = 0, pauses = 0;
        std::vector<std::pair<std::int64_t, std::uint32_t>> seeks;
        std::vector<std::pair<std::uint32_t, std::uint64_t>> sets;
        struct Data {
            std::uint64_t token;
            std::vector<unsigned char> bytes;
            std::uint32_t flags;
        };
        std::vector<Data> data;
        std::vector<std::uint64_t> rendered, discarded;
        std::vector<std::uint64_t> window_renders;
        bool destroyed = false;
        int calls_running = 0;
    };
    std::mutex mutex;
    std::map<std::uint64_t, Player> players;
    std::uint64_t next = 700;
    int destroys = 0, overlaps = 0, sources_answer = 0, render_answer = 0;
    apkrt_media_player_state state{};
    std::vector<apkrt_media_player_track> tracks;
    bool hold_start = false, start_entered = false;
    std::condition_variable start_changed;

    void reset() {
        const std::lock_guard hold(mutex);
        players.clear();
        destroys = overlaps = sources_answer = render_answer = 0;
        state = {};
        tracks.clear();
        hold_start = start_entered = false;
    }

    Player &newest() {
        const std::lock_guard hold(mutex);
        return players.rbegin()->second;
    }
};

Fake fake;

/// Counts a call into the fake for the destroy's overlap check.
struct Running {
    explicit Running(std::uint64_t handle) : handle(handle) {
        const std::lock_guard hold(fake.mutex);
        fake.players.at(handle).calls_running++;
    }
    ~Running() {
        const std::lock_guard hold(fake.mutex);
        fake.players.at(handle).calls_running--;
    }
    std::uint64_t handle;
};

int fake_create(void *, std::uint64_t player, std::uint64_t *handle) {
    const std::lock_guard hold(fake.mutex);
    *handle = fake.next++;
    fake.players[*handle].key = player;
    return 0;
}

int fake_source(void *, std::uint64_t handle, const apkrt_media_player_source_info *source) {
    const std::lock_guard hold(fake.mutex);
    Fake::Player &player = fake.players.at(handle);
    player.source = *source;
    if (source->kind == APKRT_MEDIA_PLAYER_URL) player.url.assign(source->url, source->url_size);
    if (source->kind == APKRT_MEDIA_PLAYER_FD && source->length > 0) {
        player.fd_bytes.resize(static_cast<std::size_t>(source->length));
        const ssize_t got = pread(source->fd, player.fd_bytes.data(), player.fd_bytes.size(), source->offset);
        player.fd_bytes.resize(got < 0 ? 0 : static_cast<std::size_t>(got));
    }
    return fake.sources_answer;
}

int fake_prepare(void *, std::uint64_t handle) {
    const std::lock_guard hold(fake.mutex);
    fake.players.at(handle).prepares++;
    return 0;
}

int fake_start(void *, std::uint64_t handle) {
    Running running(handle);
    std::unique_lock hold(fake.mutex);
    fake.players.at(handle).starts++;
    fake.start_entered = true;
    fake.start_changed.notify_all();
    fake.start_changed.wait(hold, [] { return !fake.hold_start; });
    return 0;
}

int fake_pause(void *, std::uint64_t handle) {
    const std::lock_guard hold(fake.mutex);
    fake.players.at(handle).pauses++;
    return 0;
}

int fake_seek(void *, std::uint64_t handle, std::int64_t time_us, std::uint32_t mode) {
    const std::lock_guard hold(fake.mutex);
    fake.players.at(handle).seeks.emplace_back(time_us, mode);
    return 0;
}

int fake_status(void *, std::uint64_t, apkrt_media_player_state *state) {
    const std::lock_guard hold(fake.mutex);
    *state = fake.state;
    return 0;
}

int fake_set(void *, std::uint64_t handle, std::uint32_t key, std::uint64_t value) {
    const std::lock_guard hold(fake.mutex);
    fake.players.at(handle).sets.emplace_back(key, value);
    return 0;
}

int fake_tracks(void *, std::uint64_t, apkrt_media_player_track *tracks, std::uint32_t capacity,
                std::uint32_t *count) {
    const std::lock_guard hold(fake.mutex);
    for (std::uint32_t at = 0; at < capacity && at < fake.tracks.size(); at++) tracks[at] = fake.tracks[at];
    *count = static_cast<std::uint32_t>(fake.tracks.size());
    return 0;
}

void fake_data(void *, std::uint64_t handle, std::uint64_t token, const void *bytes, std::size_t size,
               std::uint32_t flags) {
    const std::lock_guard hold(fake.mutex);
    const auto *first = static_cast<const unsigned char *>(bytes);
    fake.players.at(handle).data.push_back({token, std::vector<unsigned char>(first, first + size), flags});
}

int fake_render(void *, std::uint64_t handle, std::uint64_t token, const apkrt_video_target *) {
    const std::lock_guard hold(fake.mutex);
    fake.players.at(handle).rendered.push_back(token);
    return fake.render_answer;
}

int fake_render_window(void *, std::uint64_t handle, std::uint64_t token, std::uint64_t window, std::int64_t,
                       std::uint32_t) {
    const std::lock_guard hold(fake.mutex);
    fake.players.at(handle).window_renders.push_back(token);
    return window == 999 ? -static_cast<int>(enodev) : 0;
}

void fake_discard(void *, std::uint64_t handle, std::uint64_t token) {
    const std::lock_guard hold(fake.mutex);
    fake.players.at(handle).discarded.push_back(token);
}

void fake_destroy(void *, std::uint64_t handle) {
    const std::lock_guard hold(fake.mutex);
    Fake::Player &player = fake.players.at(handle);
    if (player.calls_running != 0) fake.overlaps++;
    player.destroyed = true;
    fake.destroys++;
}

const apkrt_media_players table{fake_create, fake_source, fake_prepare,  fake_start,         fake_pause,
                                fake_seek,   fake_status, fake_set,      fake_tracks,        fake_data,
                                fake_render, fake_render_window, fake_discard, fake_destroy, nullptr};

void use_fake() {
    fake.reset();
    CHECK(apkrt_set_media_players(&table) == 0);
}

/// A player made through CREATE, with its event descriptor.
apkrt_media_player create() {
    apkrt_media_player player{};
    CHECK(call(APKRT_MEDIA_PLAYER_CREATE, player) == 0);
    CHECK(player.id != 0 && player.events >= 0);
    return player;
}

void destroy(const apkrt_media_player &player) {
    CHECK(call(APKRT_MEDIA_PLAYER_DESTROY, reinterpret_cast<std::uint64_t>(&player)) == 0);
    CHECK(syscall(linux_close, static_cast<std::uint64_t>(player.events)) == 0);
}

/// The records waiting on the guest's event descriptor, read through the guest's read.
std::vector<apkrt_media_player_event> records(int events) {
    std::vector<apkrt_media_player_event> got;
    for (;;) {
        apkrt_media_player_event event{};
        const std::int64_t read = syscall(linux_read, static_cast<std::uint64_t>(events),
                                          reinterpret_cast<std::uint64_t>(&event), sizeof event);
        if (read == -eagain) return got;
        CHECK(read == sizeof event);
        got.push_back(event);
    }
}

apkrt_media_player_event record(std::uint32_t type, std::int64_t value = 0, std::uint64_t token = 0,
                                std::int32_t ext1 = 0, std::int32_t ext2 = 0) {
    apkrt_media_player_event event{};
    event.type = type, event.value = value, event.token = token, event.ext1 = ext1, event.ext2 = ext2;
    return event;
}

/// One record of the newest player's, as the host posts it.
int post(const apkrt_media_player &, const apkrt_media_player_event &event) {
    return apkrt_media_player_post(fake.newest().key, &event);
}

/// A page the guest cannot read or write.
std::uint64_t no_access_page() {
    static void *page = mmap(nullptr, 16384, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return reinterpret_cast<std::uint64_t>(page);
}

/// A guest descriptor of a file of `bytes`.
int guest_file(const std::string &bytes) {
    char name[] = "/tmp/media-players-test-XXXXXX";
    const int host = mkstemp(name);
    CHECK(host >= 0);
    unlink(name);
    CHECK(write(host, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
    const long guest = apkrt::descriptors::add(host, /*close_on_exec=*/true);
    CHECK(guest >= 0);
    return static_cast<int>(guest);
}

TEST(the_nr_number_is_the_only_one_of_its_value) {
    std::map<long, std::set<std::string>> numbers;
    const std::regex define(R"(#define\s+(APKRT_NR_\w+)\s+(\d+))");
    for (const char *name : {"include/APKRuntime/guest_interface.h", "src/loader_syscalls.h"}) {
        std::ifstream file(std::string(APKRT_RUNTIME_DIR) + "/" + name);
        CHECK(file.good());
        std::stringstream text;
        text << file.rdbuf();
        const std::string all = text.str();
        for (std::sregex_iterator it(all.begin(), all.end(), define), end; it != end; ++it) {
            numbers[std::stol((*it)[2].str())].insert((*it)[1].str());
        }
    }
    CHECK(numbers.count(APKRT_NR_MEDIA_PLAYER) == 1 && numbers[APKRT_NR_MEDIA_PLAYER].size() == 1);
    for (const auto &[number, names] : numbers) CHECK(names.size() == 1);
}

TEST(without_host_players_nothing_is_made) {
    CHECK(apkrt_set_media_players(nullptr) == 0);
    apkrt_media_player player{};
    player.events = -1;
    CHECK(call(APKRT_MEDIA_PLAYER_CREATE, player) == -enosys);
    CHECK(player.id == 0 && player.events == -1);
}

TEST(a_table_with_a_function_missing_is_refused) {
    for (std::size_t at = 0; at < 14; at++) {
        apkrt_media_players partial = table;
        reinterpret_cast<void **>(&partial)[at] = nullptr;
        CHECK(apkrt_set_media_players(&partial) == -1);
    }
}

TEST(create_gives_ids_never_0_nor_reused_and_a_non_blocking_event_descriptor) {
    use_fake();
    const apkrt_media_player first = create();
    CHECK(syscall(25, static_cast<std::uint64_t>(first.events), 1) == 1);         // F_GETFD: FD_CLOEXEC
    CHECK((syscall(25, static_cast<std::uint64_t>(first.events), 3) & 04000) != 0);  // F_GETFL: O_NONBLOCK
    CHECK(records(first.events).empty());
    destroy(first);
    const apkrt_media_player second = create();
    CHECK(second.id != first.id);
    apkrt_media_player again = first;
    CHECK(call(APKRT_MEDIA_PLAYER_START, again) == -ebadf);
    CHECK(call(APKRT_MEDIA_PLAYER_DESTROY, again) == -ebadf);
    destroy(second);
    CHECK(fake.destroys == 2);
    CHECK(call(99, 0) == -einval);
    CHECK(call(APKRT_MEDIA_PLAYER_CREATE, 0) == -efault);
    CHECK(call(APKRT_MEDIA_PLAYER_CREATE, no_access_page()) == -efault);
}

TEST(records_reach_the_guest_in_order_and_hostile_ones_never) {
    use_fake();
    const apkrt_media_player player = create();
    CHECK(post(player, record(APKRT_MEDIA_PLAYER_VIDEO_SIZE, 0, 0, 320, 180)) == 0);
    CHECK(post(player, record(APKRT_MEDIA_PLAYER_PREPARED, 1068000, 0, 320, 180)) == 0);
    CHECK(post(player, record(APKRT_MEDIA_PLAYER_ERROR, 0, 0, 1, -2147483647 - 1)) == 0);
    const auto got = records(player.events);
    CHECK(got.size() == 3);
    CHECK(got[0].type == APKRT_MEDIA_PLAYER_VIDEO_SIZE && got[0].ext1 == 320 && got[0].ext2 == 180);
    CHECK(got[1].type == APKRT_MEDIA_PLAYER_PREPARED && got[1].value == 1068000);
    CHECK(got[2].type == APKRT_MEDIA_PLAYER_ERROR && got[2].ext1 == 1 && got[2].ext2 == -2147483647 - 1);
    CHECK(post(player, record(0)) == -einval);
    CHECK(post(player, record(8)) == -einval);
    apkrt_media_player_event reserved = record(APKRT_MEDIA_PLAYER_COMPLETED);
    reserved.reserved = 1;
    CHECK(post(player, reserved) == -einval);
    CHECK(post(player, record(APKRT_MEDIA_PLAYER_FRAME, 0, 0)) == -einval);  // a frame's token is never 0
    CHECK(apkrt_media_player_post(fake.newest().key, nullptr) == -einval);
    CHECK(records(player.events).empty());
    const std::uint64_t key = fake.newest().key;
    destroy(player);
    reserved.reserved = 0;
    CHECK(apkrt_media_player_post(key, &reserved) == -ebadf);
    CHECK(apkrt_media_player_post(0, &reserved) == -ebadf);
}

TEST(a_full_pipe_keeps_lifecycle_records_in_order_and_refuses_frames_before_them) {
    use_fake();
    const apkrt_media_player player = create();
    int posted = 0;
    // Seek records until the pipe is full and the backlog too.
    for (;;) {
        const int answer = post(player, record(APKRT_MEDIA_PLAYER_SEEK_DONE, posted));
        if (answer != 0) {
            CHECK(answer == -enospc);
            break;
        }
        posted++;
        CHECK(posted < 100000);
    }
    CHECK(posted > static_cast<int>(APKRT_MEDIA_PLAYER_MAX_BACKLOG));
    CHECK(post(player, record(APKRT_MEDIA_PLAYER_FRAME, 0, 41)) == -eagain);  // never before the waiting records
    auto got = records(player.events);
    CHECK(got.size() < static_cast<std::size_t>(posted));
    // The guest drained the pipe; the backlog waits for the next post or call, here a call's.
    apkrt_media_player status = player;
    CHECK(call(APKRT_MEDIA_PLAYER_STATUS, status) == 0);
    const auto rest = records(player.events);
    got.insert(got.end(), rest.begin(), rest.end());
    CHECK(got.size() == static_cast<std::size_t>(posted));
    for (std::size_t at = 0; at < got.size(); at++) {
        CHECK(got[at].type == APKRT_MEDIA_PLAYER_SEEK_DONE && got[at].value == static_cast<std::int64_t>(at));
    }
    // Filled again, a post writes the backlog first, then its frame.
    for (int at = 0; post(player, record(APKRT_MEDIA_PLAYER_SEEK_DONE, at)) == 0; at++) CHECK(at < 100000);
    const std::size_t drained = records(player.events).size();
    CHECK(post(player, record(APKRT_MEDIA_PLAYER_FRAME, 0, 42)) == 0);
    const auto last = records(player.events);
    CHECK(last.size() == APKRT_MEDIA_PLAYER_MAX_BACKLOG + 1 && drained > 0);
    CHECK(last.back().type == APKRT_MEDIA_PLAYER_FRAME && last.back().token == 42);
    CHECK(last.front().value == static_cast<std::int64_t>(drained));
    destroy(player);
}

TEST(frames_wait_for_their_render_or_discard_up_to_the_limit) {
    use_fake();
    const apkrt_media_player player = create();
    for (std::uint64_t token = 1; token <= APKRT_MEDIA_PLAYER_MAX_WAITING; token++) {
        CHECK(post(player, record(APKRT_MEDIA_PLAYER_FRAME, 1000 + static_cast<std::int64_t>(token), token)) == 0);
    }
    CHECK(post(player, record(APKRT_MEDIA_PLAYER_FRAME, 0, 99)) == -eagain);
    CHECK(post(player, record(APKRT_MEDIA_PLAYER_FRAME, 0, 1)) == -eagain);  // one that waits
    const auto got = records(player.events);
    CHECK(got.size() == APKRT_MEDIA_PLAYER_MAX_WAITING);
    CHECK(got[0].type == APKRT_MEDIA_PLAYER_FRAME && got[0].token == 1 && got[0].value == 1001);
    apkrt_media_player asked = player;
    asked.token = 1;
    CHECK(call(APKRT_MEDIA_PLAYER_DISCARD, asked) == 0);
    CHECK(call(APKRT_MEDIA_PLAYER_DISCARD, asked) == -einval);  // ended
    asked.token = 77;
    CHECK(call(APKRT_MEDIA_PLAYER_DISCARD, asked) == -einval);
    asked.token = 2;
    asked.size = 0;  // no buffer: the token ends undrawn
    CHECK(call(APKRT_MEDIA_PLAYER_RENDER, asked) == -einval);
    asked.token = 3;
    asked.size = 0;
    CHECK(call(APKRT_MEDIA_PLAYER_RENDER_WINDOW, asked) == -enodev);
    asked.token = 4;
    asked.size = 5;
    asked.flags = APKRT_VIDEO_DECODER_CROP;
    CHECK(call(APKRT_MEDIA_PLAYER_RENDER_WINDOW, asked) == 0);
    {
        const std::lock_guard hold(fake.mutex);
        const Fake::Player &host = fake.players.rbegin()->second;
        CHECK((host.discarded == std::vector<std::uint64_t>{1, 2, 3}));
        CHECK((host.window_renders == std::vector<std::uint64_t>{4}));
    }
    CHECK(post(player, record(APKRT_MEDIA_PLAYER_FRAME, 0, 5)) == 0);
    CHECK(post(player, record(APKRT_MEDIA_PLAYER_FRAME, 0, 5)) == -eagain);  // a token that waits, below the limit
    destroy(player);
    {
        const std::lock_guard hold(fake.mutex);
        CHECK(fake.players.rbegin()->second.destroyed);
    }
}

TEST(a_source_by_descriptor_is_the_runtimes_duplicate_within_the_file) {
    use_fake();
    const int fd = guest_file("0123456789");
    apkrt_media_player player = create();
    player.kind = APKRT_MEDIA_PLAYER_FD;
    player.fd = fd;
    player.offset = 11, player.length = -1;
    CHECK(call(APKRT_MEDIA_PLAYER_SOURCE, player) == -einval);
    player.offset = 10;  // the file's end: no byte to play, as MediaPlayerService refuses it
    CHECK(call(APKRT_MEDIA_PLAYER_SOURCE, player) == -einval);
    player.offset = 4, player.length = 7;
    CHECK(call(APKRT_MEDIA_PLAYER_SOURCE, player) == -einval);
    player.offset = -1, player.length = 2;
    CHECK(call(APKRT_MEDIA_PLAYER_SOURCE, player) == -einval);
    player.offset = 2, player.length = -2;
    CHECK(call(APKRT_MEDIA_PLAYER_SOURCE, player) == -einval);
    player.fd = 31000, player.offset = 0, player.length = -1;
    CHECK(call(APKRT_MEDIA_PLAYER_SOURCE, player) == -ebadf);
    CHECK(post(player, record(APKRT_MEDIA_PLAYER_COMPLETED)) == 0);  // a pipe whose size is a record's
    player.fd = player.events;  // a pipe's, not a file
    CHECK(call(APKRT_MEDIA_PLAYER_SOURCE, player) == -einval);
    player.fd = fd, player.offset = 3, player.length = -1;
    CHECK(call(APKRT_MEDIA_PLAYER_SOURCE, player) == 0);
    CHECK(call(APKRT_MEDIA_PLAYER_SOURCE, player) == -einval);  // once
    {
        const std::lock_guard hold(fake.mutex);
        const Fake::Player &host = fake.players.rbegin()->second;
        CHECK(host.source.offset == 3 && host.source.length == 7 && host.fd_bytes == "3456789");
        CHECK(host.source.fd != apkrt::descriptors::get(fd)->host());
    }
    CHECK(syscall(linux_close, static_cast<std::uint64_t>(fd)) == 0);  // the duplicate stays
    CHECK(call(APKRT_MEDIA_PLAYER_PREPARE, player) == 0);
    CHECK(call(APKRT_MEDIA_PLAYER_PREPARE, player) == -einval);
    const int host_fd = fake.newest().source.fd;
    CHECK(fcntl(host_fd, F_GETFD) >= 0);
    destroy(player);
    CHECK(fcntl(host_fd, F_GETFD) == -1);  // closed once the host's player has gone
}

TEST(a_url_source_is_http_or_https_within_the_limit) {
    use_fake();
    apkrt_media_player player = create();
    CHECK(call(APKRT_MEDIA_PLAYER_PREPARE, player) == -einval);  // no source
    player.kind = APKRT_MEDIA_PLAYER_URL;
    const std::vector<std::string> refused = {"", "ftp://example.org/a.mp4", "file:///sdcard/a.mp4", "http:",
                                              "httpx://a", std::string("http://a\0b", 10),
                                              "http://" + std::string(APKRT_MEDIA_PLAYER_MAX_URL, 'a')};
    for (const std::string &url : refused) {
        player.data = reinterpret_cast<std::uint64_t>(url.data());
        player.size = url.size();
        CHECK(call(APKRT_MEDIA_PLAYER_SOURCE, player) == -einval);
    }
    player.data = no_access_page(), player.size = 20;
    CHECK(call(APKRT_MEDIA_PLAYER_SOURCE, player) == -efault);
    const std::string url = "HTTPS://127.0.0.1:1/baseline.mp4";
    player.data = reinterpret_cast<std::uint64_t>(url.data()), player.size = url.size();
    apkrt::sockets::set_internet(false);  // the host never loads what the app's sockets could not
    CHECK(call(APKRT_MEDIA_PLAYER_SOURCE, player) == -13);
    CHECK(fake.newest().url.empty());
    apkrt::sockets::set_internet(true);
    CHECK(call(APKRT_MEDIA_PLAYER_SOURCE, player) == 0);
    CHECK(fake.newest().url == url);
    player.kind = 9;
    CHECK(call(APKRT_MEDIA_PLAYER_SOURCE, player) == -einval);
    destroy(player);
}

TEST(a_callback_source_reads_one_request_at_a_time) {
    use_fake();
    apkrt_media_player player = create();
    player.kind = APKRT_MEDIA_PLAYER_CALLBACK, player.length = -3;
    CHECK(call(APKRT_MEDIA_PLAYER_SOURCE, player) == -einval);
    player.length = 10;
    CHECK(call(APKRT_MEDIA_PLAYER_SOURCE, player) == 0);
    CHECK(fake.newest().source.kind == APKRT_MEDIA_PLAYER_CALLBACK && fake.newest().source.length == 10);
    CHECK(post(player, record(APKRT_MEDIA_PLAYER_READ, -1, 7, 4)) == -einval);  // offset below 0
    CHECK(post(player, record(APKRT_MEDIA_PLAYER_READ, 2, 7, -1)) == -einval);  // size below 0
    CHECK(post(player, record(APKRT_MEDIA_PLAYER_READ, 2, 0, 4)) == -einval);   // token 0
    CHECK(post(player, record(APKRT_MEDIA_PLAYER_READ, 2, 7, 4)) == 0);
    CHECK(post(player, record(APKRT_MEDIA_PLAYER_READ, 6, 8, 4)) == -ebusy);
    const auto got = records(player.events);
    CHECK(got.size() == 1 && got[0].token == 7 && got[0].value == 2 && got[0].ext1 == 4);
    const unsigned char bytes[5] = {'2', '3', '4', '5', '6'};
    player.data = reinterpret_cast<std::uint64_t>(bytes);
    player.token = 8, player.size = 4;
    CHECK(call(APKRT_MEDIA_PLAYER_DATA, player) == -einval);  // not the READ that waits
    player.token = 7, player.size = 5;
    CHECK(call(APKRT_MEDIA_PLAYER_DATA, player) == -einval);  // above the READ's size
    player.size = 4, player.data = no_access_page();
    CHECK(call(APKRT_MEDIA_PLAYER_DATA, player) == -efault);
    player.data = reinterpret_cast<std::uint64_t>(bytes);
    player.flags = 2;
    CHECK(call(APKRT_MEDIA_PLAYER_DATA, player) == -einval);
    player.flags = 0;
    CHECK(call(APKRT_MEDIA_PLAYER_DATA, player) == 0);
    CHECK(call(APKRT_MEDIA_PLAYER_DATA, player) == -einval);  // answered
    CHECK(post(player, record(APKRT_MEDIA_PLAYER_READ, 6, 8, 4)) == 0);
    player.token = 8, player.size = 0, player.flags = APKRT_MEDIA_PLAYER_READ_FAILED;
    CHECK(call(APKRT_MEDIA_PLAYER_DATA, player) == 0);
    {
        const std::lock_guard hold(fake.mutex);
        const auto &data = fake.players.rbegin()->second.data;
        CHECK(data.size() == 2);
        CHECK(data[0].token == 7 && (data[0].bytes == std::vector<unsigned char>{'2', '3', '4', '5'}));
        CHECK(data[1].token == 8 && data[1].bytes.empty() && data[1].flags == APKRT_MEDIA_PLAYER_READ_FAILED);
    }
    CHECK(post(player, record(APKRT_MEDIA_PLAYER_READ, 0, 9, 4)) == 0);
    const std::uint64_t key = fake.newest().key;
    destroy(player);
    player.token = 9, player.size = 0, player.flags = 0;
    CHECK(call(APKRT_MEDIA_PLAYER_DATA, player) == -ebadf);
    CHECK(apkrt_media_player_post(key, nullptr) == -einval);
}

TEST(seeks_settings_status_and_tracks_are_checked_both_ways) {
    use_fake();
    apkrt_media_player player = create();
    player.time_us = -1;
    CHECK(call(APKRT_MEDIA_PLAYER_SEEK, player) == -einval);
    player.time_us = 433000, player.value = 4;
    CHECK(call(APKRT_MEDIA_PLAYER_SEEK, player) == -einval);
    player.value = 3;
    CHECK(call(APKRT_MEDIA_PLAYER_SEEK, player) == 0);
    const auto bits = [](float low, float high) {
        std::uint32_t a = 0, b = 0;
        std::memcpy(&a, &low, 4);
        std::memcpy(&b, &high, 4);
        return std::uint64_t{a} | std::uint64_t{b} << 32;
    };
    const std::pair<std::uint32_t, std::uint64_t> refused[] = {
        {APKRT_MEDIA_PLAYER_VOLUME, bits(1.5f, 0.5f)}, {APKRT_MEDIA_PLAYER_VOLUME, bits(0.5f, -0.1f)},
        {APKRT_MEDIA_PLAYER_VOLUME, bits(NAN, 0.5f)},  {APKRT_MEDIA_PLAYER_LOOPING, 2},
        {APKRT_MEDIA_PLAYER_SPEED, bits(0.1f, 0)},     {APKRT_MEDIA_PLAYER_SPEED, bits(8.5f, 0)},
        {APKRT_MEDIA_PLAYER_SPEED, bits(2.0f, 1.0f)},  {APKRT_MEDIA_PLAYER_SPEED, bits(NAN, 0)},
        {APKRT_MEDIA_PLAYER_SESSION, std::uint64_t{1} << 32}, {APKRT_MEDIA_PLAYER_SESSION, 0x80000000u},
        {0, 0},                                        {5, 0}};
    for (const auto &[key, value] : refused) {
        player.kind = key, player.value = value;
        CHECK(call(APKRT_MEDIA_PLAYER_SET, player) == -einval);
    }
    player.kind = APKRT_MEDIA_PLAYER_VOLUME, player.value = bits(0.25f, 1.0f);
    CHECK(call(APKRT_MEDIA_PLAYER_SET, player) == 0);
    player.kind = APKRT_MEDIA_PLAYER_LOOPING, player.value = 1;
    CHECK(call(APKRT_MEDIA_PLAYER_SET, player) == 0);
    player.kind = APKRT_MEDIA_PLAYER_SPEED, player.value = bits(0.125f, 0);
    CHECK(call(APKRT_MEDIA_PLAYER_SET, player) == 0);
    player.kind = APKRT_MEDIA_PLAYER_SESSION, player.value = 41;  // the app's audio session of the player
    CHECK(call(APKRT_MEDIA_PLAYER_SET, player) == 0);
    {
        const std::lock_guard hold(fake.mutex);
        const Fake::Player &host = fake.players.rbegin()->second;
        CHECK((host.seeks == std::vector<std::pair<std::int64_t, std::uint32_t>>{{433000, 3}}));
        CHECK(host.sets.size() == 4 && host.sets[0].second == bits(0.25f, 1.0f) && host.sets[1].second == 1);
        CHECK(host.sets[3].first == APKRT_MEDIA_PLAYER_SESSION && host.sets[3].second == 41);
        fake.state = {500000, 1068000, 320, 180, APKRT_MEDIA_PLAYER_PLAYING};
        apkrt_media_player_track video{1, "und", "video/avc"}, audio{2, "eng", "audio/mp4a-latm"};
        fake.tracks = {video, audio};
    }
    CHECK(call(APKRT_MEDIA_PLAYER_STATUS, player) == 0);
    CHECK(player.time_us == 500000 && player.duration_us == 1068000 && player.width == 320 && player.height == 180);
    CHECK(player.flags == APKRT_MEDIA_PLAYER_PLAYING);
    for (const apkrt_media_player_state hostile : {apkrt_media_player_state{-1, 1068000, 320, 180, 0},
                                                   apkrt_media_player_state{0, -2, 320, 180, 0},
                                                   apkrt_media_player_state{0, 1068000, 8193, 180, 0},
                                                   apkrt_media_player_state{0, 1068000, 320, 180, 2}}) {
        {
            const std::lock_guard hold(fake.mutex);
            fake.state = hostile;
        }
        apkrt_media_player asked = player;
        CHECK(call(APKRT_MEDIA_PLAYER_STATUS, asked) == -eio);
    }
    apkrt_media_player_track tracks[3]{};
    player.data = reinterpret_cast<std::uint64_t>(tracks), player.size = sizeof tracks[0];
    CHECK(call(APKRT_MEDIA_PLAYER_TRACKS, player) == -enospc);
    CHECK(player.size == 2 * sizeof tracks[0]);
    player.size = sizeof tracks;
    CHECK(call(APKRT_MEDIA_PLAYER_TRACKS, player) == 0);
    CHECK(player.size == 2 * sizeof tracks[0]);
    CHECK(tracks[0].type == 1 && std::strcmp(tracks[1].language, "eng") == 0);
    CHECK(std::strcmp(tracks[1].mime, "audio/mp4a-latm") == 0);
    player.data = no_access_page();
    CHECK(call(APKRT_MEDIA_PLAYER_TRACKS, player) == -efault);
    {
        const std::lock_guard hold(fake.mutex);
        std::memset(fake.tracks[1].mime, 'x', sizeof fake.tracks[1].mime);  // no NUL
    }
    player.data = reinterpret_cast<std::uint64_t>(tracks);
    CHECK(call(APKRT_MEDIA_PLAYER_TRACKS, player) == -eio);
    {
        const std::lock_guard hold(fake.mutex);
        fake.tracks[1].mime[55] = 0;  // ended again
        fake.tracks[1].type = 6;
    }
    CHECK(call(APKRT_MEDIA_PLAYER_TRACKS, player) == -eio);
    {
        const std::lock_guard hold(fake.mutex);
        fake.tracks.assign(APKRT_MEDIA_PLAYER_MAX_TRACKS + 1, apkrt_media_player_track{1, "und", "video/avc"});
    }
    CHECK(call(APKRT_MEDIA_PLAYER_TRACKS, player) == -eio);
    destroy(player);
}

TEST(a_destroy_waits_for_the_call_still_in_the_host_and_ends_the_pipe) {
    use_fake();
    const apkrt_media_player player = create();
    {
        const std::lock_guard hold(fake.mutex);
        fake.hold_start = true;
    }
    std::thread starting([&] {
        apkrt_media_player asked = player;
        CHECK(call(APKRT_MEDIA_PLAYER_START, asked) == 0);
    });
    {
        std::unique_lock hold(fake.mutex);
        fake.start_changed.wait(hold, [] { return fake.start_entered; });
    }
    std::thread destroying([&] {
        apkrt_media_player asked = player;
        CHECK(call(APKRT_MEDIA_PLAYER_DESTROY, asked) == 0);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    {
        const std::lock_guard hold(fake.mutex);
        CHECK(fake.destroys == 0);
        fake.hold_start = false;
        fake.start_changed.notify_all();
    }
    starting.join();
    destroying.join();
    CHECK(fake.destroys == 1 && fake.overlaps == 0);
    char byte = 0;
    CHECK(syscall(linux_read, static_cast<std::uint64_t>(player.events), reinterpret_cast<std::uint64_t>(&byte), 1) ==
          0);  // the end of the pipe
    CHECK(syscall(linux_close, static_cast<std::uint64_t>(player.events)) == 0);
}

TEST(the_players_of_a_process_are_limited) {
    use_fake();
    std::vector<apkrt_media_player> made;
    for (std::uint32_t at = 0; at < APKRT_MEDIA_PLAYER_MAX_PLAYERS; at++) made.push_back(create());
    apkrt_media_player one_more{};
    CHECK(call(APKRT_MEDIA_PLAYER_CREATE, one_more) == -ebusy);
    for (const auto &player : made) destroy(player);
    destroy(create());
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // each test's line before a crash in the next
    return apk::testing::run_all();
}
