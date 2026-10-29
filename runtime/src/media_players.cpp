// The guest's media players (media_players.h, ADR-0060 e2e1), as the video decoders' registry (video_decoders.cpp):
// IDs count up from 1 and are never used again, one mutex for the registry, which no call into the host holds, and
// one of each player's around every call of its host's functions, so that they come one at a time and a DESTROY
// waits for the call still running on another thread. The guest's struct is read once, its bytes copied into host
// memory before the host sees them; an fd source is the runtime's duplicate of the guest's descriptor, checked
// against the file's size, closed after the host's player has gone; a URL source needs the app's INTERNET, as its
// sockets do (ADR-0054 step c2a), so that the host never loads what the app could not.
// Records: each player has a pipe, whose read end is the guest's event descriptor and whose write end only the
// runtime writes, never waiting (non-blocking, and without SIGPIPE once the guest closed its end). A second mutex of
// the player's, which a host's function may take through apkrt_media_player_post while the first is held, but never
// the other way round, guards the write end, the backlog, the waiting frames' tokens and the READ that waits. No
// record is dropped: one that finds the pipe full waits in the backlog, which every post and every guest call on the
// player writes first, in order; a FRAME is refused instead (the host drops it), and counts against
// APKRT_MEDIA_PLAYER_MAX_WAITING only once written, so that the guest never sees a frame before a record that came
// before it. A DESTROY ends every waiting token and the READ, and closes the write end: the guest reads the pipe's end.
#include "media_players.h"

#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"
#include "descriptors.h"
#include "sockets.h"
#include "syscalls.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <vector>

namespace apkrt::media_players {
namespace {

namespace linux_errno = sys::linux_errno;

// EACCES, EMFILE (too many open descriptors) and ENOSPC are not in sys::linux_errno; descriptors.cpp defines its own
// the same way.
constexpr long eacces = 13, emfile = 24, enospc = 28;

static_assert(sizeof(apkrt_media_player) == 96);
static_assert(offsetof(apkrt_media_player, kind) == 8 && offsetof(apkrt_media_player, fd) == 16);
static_assert(offsetof(apkrt_media_player, width) == 24 && offsetof(apkrt_media_player, data) == 32);
static_assert(offsetof(apkrt_media_player, offset) == 48 && offsetof(apkrt_media_player, time_us) == 64);
static_assert(offsetof(apkrt_media_player, token) == 80 && offsetof(apkrt_media_player, value) == 88);
static_assert(sizeof(apkrt_media_player_event) == 32 && offsetof(apkrt_media_player_event, token) == 24);
static_assert(sizeof(apkrt_media_player_track) == 64);
static_assert(sizeof(apkrt_media_player_event) <= PIPE_BUF);  // a record is written whole or not at all

/// An answer of the host's functions as an errno: 0, or -1 to -4095, anything else -EIO.
long host_answer(int answer) {
    return answer == 0 || (answer < 0 && answer >= -4095) ? answer : -linux_errno::eio;
}

struct Player {
    apkrt_media_players table{};
    std::uint64_t handle = 0;
    std::mutex mutex;          // around each call of the host's functions, and for the fields below
    bool gone = false;         // destroyed: no more calls
    bool sourced = false, prepared = false;
    int source_fd = -1;        // an fd source's duplicate, the runtime's

    std::mutex records;        // for the fields below; taken after `mutex`, never before it
    int write_end = -1;        // the pipe's, -1 once destroyed
    std::deque<apkrt_media_player_event> backlog;  // lifecycle records that found the pipe full, in order
    std::set<std::uint64_t> waiting;               // the tokens of FRAMEs written and not rendered or discarded
    std::uint64_t read_token = 0;                  // the READ that waits for its DATA, 0 for none
    std::int64_t read_size = 0;

    ~Player() {
        if (write_end >= 0) ::close(write_end);
        if (source_fd >= 0) ::close(source_fd);
    }
};

struct Registry {
    std::mutex mutex;
    std::map<std::uint64_t, std::shared_ptr<Player>> players;
    std::uint32_t creating = 0;    // CREATEs in the host, counted against the limit
    std::uint32_t destroying = 0;  // DESTROYs whose entry went, until the host's player has gone too
    std::uint64_t next_id = 1;
    apkrt_media_players table{};   // all NULL: none
};

Registry &registry() {
    static auto *one = new Registry;  // as long as the process
    return *one;
}

std::shared_ptr<Player> find(std::uint64_t id) {
    auto &all = registry();
    const std::lock_guard hold(all.mutex);
    const auto found = all.players.find(id);
    return found == all.players.end() ? nullptr : found->second;
}

/// Writes `size` bytes of `value` to the guest's struct at `address` + `offset`; false where the guest cannot write.
bool put(std::uint64_t address, std::size_t offset, const void *value, std::size_t size) {
    return sys::memory::copy_checked(address + offset, reinterpret_cast<std::uint64_t>(value), size);
}

/// Copies `size` of the guest's bytes at `address` into `bytes`; false where the guest does not have them.
bool take(std::uint64_t address, std::uint64_t size, std::vector<unsigned char> &bytes) {
    bytes.resize(size);
    if (size == 0) return true;
    if (address == 0) return false;
    return sys::memory::copy_checked(reinterpret_cast<std::uint64_t>(bytes.data()), address, size);
}

enum class Written { yes, full };

/// One record into the pipe, under `records`. A guest that closed its end gets nothing, as if it had read it.
Written write_record(const Player &player, const apkrt_media_player_event &event) {
    for (;;) {
        const ssize_t wrote = ::write(player.write_end, &event, sizeof event);
        if (wrote == static_cast<ssize_t>(sizeof event)) return Written::yes;
        if (wrote < 0 && errno == EINTR) continue;
        return wrote < 0 && errno == EPIPE ? Written::yes : Written::full;
    }
}

/// Writes the backlog, under `records`, as far as the pipe has room: whether all of it went.
bool write_backlog(Player &player) {
    while (!player.backlog.empty()) {
        if (write_record(player, player.backlog.front()) == Written::full) return false;
        player.backlog.pop_front();
    }
    return true;
}

/// Writes what waits for the guest, at each call of the guest's on the player.
void write_waiting(Player &player) {
    const std::lock_guard hold(player.records);
    if (player.write_end >= 0) write_backlog(player);
}

/// Whether a record from the host holds.
bool record_holds(const apkrt_media_player_event &event) {
    if (event.reserved != 0) return false;
    switch (event.type) {
        case APKRT_MEDIA_PLAYER_PREPARED:
        case APKRT_MEDIA_PLAYER_VIDEO_SIZE: return event.ext1 >= 0 && event.ext2 >= 0;
        case APKRT_MEDIA_PLAYER_ERROR:
        case APKRT_MEDIA_PLAYER_SEEK_DONE:
        case APKRT_MEDIA_PLAYER_COMPLETED: return true;
        case APKRT_MEDIA_PLAYER_FRAME: return event.token != 0;
        case APKRT_MEDIA_PLAYER_READ:
            return event.token != 0 && event.value >= 0 && event.ext1 >= 0 &&
                   static_cast<std::uint64_t>(event.ext1) <= APKRT_MEDIA_PLAYER_MAX_DATA;
        default: return false;
    }
}

long destroy(std::uint64_t id) {
    std::shared_ptr<Player> player;
    {
        auto &all = registry();
        const std::lock_guard hold(all.mutex);
        const auto found = all.players.find(id);
        if (found == all.players.end()) return -linux_errno::ebadf;
        player = std::move(found->second);
        all.players.erase(found);
        all.destroying++;
    }
    {
        const std::lock_guard hold(player->mutex);  // after the call still running on it
        player->gone = true;
        player->table.destroy(player->table.context, player->handle);
        const std::lock_guard records(player->records);
        player->waiting.clear();
        player->read_token = 0;
        player->backlog.clear();
        ::close(player->write_end);  // the guest reads the pipe's end
        player->write_end = -1;
        if (player->source_fd >= 0) ::close(player->source_fd);
        player->source_fd = -1;
    }
    auto &all = registry();
    const std::lock_guard hold(all.mutex);
    all.destroying--;
    return 0;
}

/// A pipe for a player's records: both ends non-blocking and close-on-exec, the write end without SIGPIPE.
bool open_pipe(int (&ends)[2]) {
    if (::pipe(ends) != 0) return false;
    for (const int end : ends) {
        if (::fcntl(end, F_SETFD, FD_CLOEXEC) != 0 || ::fcntl(end, F_SETFL, O_NONBLOCK) != 0) {
            ::close(ends[0]);
            ::close(ends[1]);
            return false;
        }
    }
    if (::fcntl(ends[1], F_SETNOSIGPIPE, 1) != 0) {
        ::close(ends[0]);
        ::close(ends[1]);
        return false;
    }
    return true;
}

long create(std::uint64_t argument) {
    apkrt_media_players table{};
    std::uint64_t id = 0;
    {
        auto &all = registry();
        const std::lock_guard hold(all.mutex);
        if (all.table.create == nullptr) return -linux_errno::enosys;
        if (all.players.size() + all.creating + all.destroying >= APKRT_MEDIA_PLAYER_MAX_PLAYERS) {
            return -linux_errno::ebusy;
        }
        all.creating++;
        table = all.table;
        id = all.next_id++;  // an ID a failed CREATE takes is not used again either
    }
    const auto done_creating = [] {
        auto &all = registry();
        const std::lock_guard hold(all.mutex);
        all.creating--;
    };
    int ends[2] = {-1, -1};
    if (!open_pipe(ends)) {
        done_creating();
        return -emfile;
    }
    std::shared_ptr<Player> player;
    try {
        player = std::make_shared<Player>();
    } catch (const std::bad_alloc &) {
        ::close(ends[0]);
        ::close(ends[1]);
        done_creating();
        return -linux_errno::enomem;
    }
    player->table = table;
    player->write_end = ends[1];
    const long made = host_answer(table.create(table.context, id, &player->handle));
    if (made != 0) {
        ::close(ends[0]);
        done_creating();
        return made;
    }
    long events = -linux_errno::enomem;
    try {
        // The guest's descriptor owns the read end from here, also when the table refuses it.
        events = descriptors::add(std::make_shared<descriptors::Description>(
                                      ends[0], /*can_block=*/true, /*adopted=*/false, /*nonblocking=*/true,
                                      descriptors::Description::Kind::file,
                                      descriptors::Description::PipeEnd::reading),
                                  /*close_on_exec=*/true);
    } catch (const std::bad_alloc &) {
        ::close(ends[0]);
    }
    bool listed = false;
    if (events >= 0) {
        auto &all = registry();
        const std::lock_guard hold(all.mutex);
        try {
            all.players.emplace(id, player);
            listed = true;
        } catch (const std::bad_alloc &) {
            events = -linux_errno::enomem;
        }
    }
    if (!listed) {
        if (events >= 0) descriptors::close(static_cast<int>(events));
        table.destroy(table.context, player->handle);
        done_creating();
        return events < 0 ? events : -linux_errno::enomem;
    }
    done_creating();
    const std::int32_t guest_events = static_cast<std::int32_t>(events);
    if (!put(argument, offsetof(apkrt_media_player, id), &id, sizeof id) ||
        !put(argument, offsetof(apkrt_media_player, events), &guest_events, sizeof guest_events)) {
        destroy(id);  // the guest cannot learn of it
        descriptors::close(guest_events);
        return -linux_errno::efault;
    }
    return 0;
}

/// SOURCE's fd: the runtime's duplicate of the guest's descriptor, the offset and length checked against the file.
long fd_source(const apkrt_media_player &asked, apkrt_media_player_source_info &source) {
    const descriptors::Handle description = descriptors::get(asked.fd);
    if (description == nullptr || description->kind() != descriptors::Description::Kind::file) {
        return -linux_errno::ebadf;
    }
    struct stat status{};
    if (::fstat(description->host(), &status) != 0) return -linux_errno::ebadf;
    if (!S_ISREG(status.st_mode)) return -linux_errno::einval;  // a file, whose size bounds the source
    const std::int64_t size = status.st_size;
    if (asked.offset < 0 || asked.offset >= size || asked.length < -1 ||
        (asked.length != -1 && asked.length > size - asked.offset)) {
        return -linux_errno::einval;
    }
    const int duplicate = ::fcntl(description->host(), F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0) return -emfile;
    source.fd = duplicate;
    source.offset = asked.offset;
    source.length = asked.length == -1 ? size - asked.offset : asked.length;
    return 0;
}

/// Whether `url` is an http or https URL, without a NUL.
bool web_url(const std::vector<unsigned char> &url) {
    if (std::memchr(url.data(), 0, url.size()) != nullptr) return false;
    const std::string text(url.begin(), url.end());
    const auto starts = [&](const char *scheme) {
        const std::size_t length = std::strlen(scheme);
        if (text.size() <= length) return false;
        for (std::size_t at = 0; at < length; at++) {
            const char c = text[at];
            if ((c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c) != scheme[at]) return false;
        }
        return true;
    };
    return starts("http://") || starts("https://");
}

long source(const std::shared_ptr<Player> &player, const apkrt_media_player &asked) {
    apkrt_media_player_source_info source{asked.kind, -1, 0, 0, nullptr, 0};
    std::vector<unsigned char> url;
    switch (asked.kind) {
        case APKRT_MEDIA_PLAYER_FD: break;
        case APKRT_MEDIA_PLAYER_URL:
            if (asked.size == 0 || asked.size > APKRT_MEDIA_PLAYER_MAX_URL) return -linux_errno::einval;
            if (!take(asked.data, asked.size, url)) return -linux_errno::efault;
            if (!web_url(url)) return -linux_errno::einval;
            if (!sockets::internet()) return -eacces;  // the app's sockets would answer EPERM (ADR-0054 step c2a)
            source.url = reinterpret_cast<const char *>(url.data());
            source.url_size = url.size();
            break;
        case APKRT_MEDIA_PLAYER_CALLBACK:
            if (asked.length < -1) return -linux_errno::einval;
            source.length = asked.length;
            break;
        default: return -linux_errno::einval;
    }
    const std::lock_guard hold(player->mutex);
    if (player->gone) return -linux_errno::ebadf;
    if (player->sourced) return -linux_errno::einval;
    if (asked.kind == APKRT_MEDIA_PLAYER_FD) {
        if (const long checked = fd_source(asked, source); checked != 0) return checked;
    }
    const long answer = host_answer(player->table.source(player->table.context, player->handle, &source));
    if (answer != 0) {
        if (source.fd >= 0) ::close(source.fd);
        return answer;
    }
    player->sourced = true;
    player->source_fd = source.fd;
    return 0;
}

/// Whether the float in `bits` is within [low, high]: never a NaN, whose comparisons are false.
bool float_within(std::uint32_t bits, float low, float high) {
    float value = 0;
    std::memcpy(&value, &bits, sizeof value);
    return value >= low && value <= high;
}

bool value_holds(std::uint32_t key, std::uint64_t value) {
    const auto low = static_cast<std::uint32_t>(value), high = static_cast<std::uint32_t>(value >> 32);
    switch (key) {
        case APKRT_MEDIA_PLAYER_VOLUME: return float_within(low, 0, 1) && float_within(high, 0, 1);
        case APKRT_MEDIA_PLAYER_LOOPING: return value <= 1;
        case APKRT_MEDIA_PLAYER_SPEED: return high == 0 && float_within(low, 0.125f, 8);
        default: return false;
    }
}

long status(std::uint64_t argument, Player &player) {
    apkrt_media_player_state state{};
    const long answer = host_answer(player.table.status(player.table.context, player.handle, &state));
    if (answer != 0) return answer;
    if (state.position_us < 0 || state.duration_us < -1 || (state.flags & ~APKRT_MEDIA_PLAYER_PLAYING) != 0 ||
        state.width > APKRT_VIDEO_DECODER_MAX_DIMENSION || state.height > APKRT_VIDEO_DECODER_MAX_DIMENSION) {
        return -linux_errno::eio;
    }
    const std::uint32_t sizes[2] = {state.width, state.height};
    const std::int64_t times[2] = {state.position_us, state.duration_us};
    return put(argument, offsetof(apkrt_media_player, flags), &state.flags, sizeof state.flags) &&
                   put(argument, offsetof(apkrt_media_player, width), sizes, sizeof sizes) &&
                   put(argument, offsetof(apkrt_media_player, time_us), times, sizeof times)
               ? 0
               : -linux_errno::efault;
}

/// Whether a track from the host holds: a known type, and both strings ended within their arrays.
bool track_holds(const apkrt_media_player_track &track) {
    return track.type <= 5 && std::memchr(track.language, 0, sizeof track.language) != nullptr &&
           std::memchr(track.mime, 0, sizeof track.mime) != nullptr;
}

long tracks(std::uint64_t argument, Player &player, const apkrt_media_player &asked) {
    const std::uint64_t fits = asked.size / sizeof(apkrt_media_player_track);
    const auto capacity =
        static_cast<std::uint32_t>(fits < APKRT_MEDIA_PLAYER_MAX_TRACKS ? fits : APKRT_MEDIA_PLAYER_MAX_TRACKS);
    std::vector<apkrt_media_player_track> got(capacity == 0 ? 1 : capacity);
    std::uint32_t count = 0;
    const long answer = host_answer(player.table.tracks(player.table.context, player.handle, got.data(), capacity,
                                                        &count));
    if (answer != 0) return answer;
    if (count > APKRT_MEDIA_PLAYER_MAX_TRACKS) return -linux_errno::eio;
    const std::uint64_t bytes = std::uint64_t{count} * sizeof(apkrt_media_player_track);
    if (count > capacity) {
        return put(argument, offsetof(apkrt_media_player, size), &bytes, sizeof bytes) ? -linux_errno::enospc
                                                                                        : -linux_errno::efault;
    }
    for (std::uint32_t at = 0; at < count; at++) {
        if (!track_holds(got[at])) return -linux_errno::eio;
    }
    if (count != 0 && (asked.data == 0 || !sys::memory::copy_checked(asked.data,
                                                                     reinterpret_cast<std::uint64_t>(got.data()),
                                                                     bytes))) {
        return -linux_errno::efault;
    }
    return put(argument, offsetof(apkrt_media_player, size), &bytes, sizeof bytes) ? 0 : -linux_errno::efault;
}

long data(Player &player, const apkrt_media_player &asked) {
    if (asked.size > APKRT_MEDIA_PLAYER_MAX_DATA || (asked.flags & ~APKRT_MEDIA_PLAYER_READ_FAILED) != 0 ||
        (asked.flags != 0 && asked.size != 0)) {
        return -linux_errno::einval;
    }
    std::vector<unsigned char> bytes;
    if (!take(asked.data, asked.size, bytes)) return -linux_errno::efault;
    {
        const std::lock_guard records(player.records);
        if (player.read_token == 0 || asked.token != player.read_token ||
            asked.size > static_cast<std::uint64_t>(player.read_size)) {
            return -linux_errno::einval;
        }
        player.read_token = 0;
    }
    player.table.data(player.table.context, player.handle, asked.token, bytes.empty() ? nullptr : bytes.data(),
                      bytes.size(), asked.flags);
    return 0;
}

/// The waiting token of a RENDER, RENDER_WINDOW or DISCARD taken: 0, or -EINVAL for one that does not wait.
long take_token(Player &player, std::uint64_t token) {
    const std::lock_guard records(player.records);
    return player.waiting.erase(token) == 0 ? -linux_errno::einval : 0;
}

long render(Player &player, const apkrt_media_player &asked) {
    if (const long taken = take_token(player, asked.token); taken != 0) return taken;
    apkrt_video_target target{};
    if (asked.size == 0 || apkrt_hardware_buffer_acquire_handle(asked.size, &target.handle, &target.buffers,
                                                                 &target.description, &target.stride) != 0) {
        player.table.discard(player.table.context, player.handle, asked.token);
        return -linux_errno::einval;
    }
    long answer = -linux_errno::einval;
    if (target.description.layers == 1) {
        answer = host_answer(player.table.render(player.table.context, player.handle, asked.token, &target));
    } else {
        player.table.discard(player.table.context, player.handle, asked.token);
    }
    apkrt_hardware_buffer_release_handle(asked.size);
    return answer;
}

long render_window(Player &player, const apkrt_media_player &asked) {
    if (const long taken = take_token(player, asked.token); taken != 0) return taken;
    const long refused = asked.size == 0                                                 ? -linux_errno::enodev
                         : (asked.flags & ~std::uint32_t{APKRT_VIDEO_DECODER_CROP}) != 0 ? -linux_errno::einval
                         : asked.time_us < 0 || asked.time_us > INT64_MAX / 1000         ? -linux_errno::einval
                                                                                         : 0;
    if (refused != 0) {
        player.table.discard(player.table.context, player.handle, asked.token);
        return refused;
    }
    return host_answer(player.table.render_window(player.table.context, player.handle, asked.token, asked.size,
                                                  asked.time_us * 1000, asked.flags));
}

/// An operation on a player that exists, under its lock.
long operate(std::uint64_t op, std::uint64_t argument, const apkrt_media_player &asked) {
    const auto player = find(asked.id);
    if (player == nullptr) return -linux_errno::ebadf;
    write_waiting(*player);
    if (op == APKRT_MEDIA_PLAYER_SOURCE) return source(player, asked);
    if (op == APKRT_MEDIA_PLAYER_SEEK && (asked.time_us < 0 || asked.value > 3)) return -linux_errno::einval;
    if (op == APKRT_MEDIA_PLAYER_SET && !value_holds(asked.kind, asked.value)) return -linux_errno::einval;
    const std::lock_guard hold(player->mutex);
    if (player->gone) return -linux_errno::ebadf;
    Player &one = *player;
    const auto &table = one.table;
    switch (op) {
        case APKRT_MEDIA_PLAYER_PREPARE:
            if (!one.sourced || one.prepared) return -linux_errno::einval;
            one.prepared = true;
            return host_answer(table.prepare(table.context, one.handle));
        case APKRT_MEDIA_PLAYER_START: return host_answer(table.start(table.context, one.handle));
        case APKRT_MEDIA_PLAYER_PAUSE: return host_answer(table.pause(table.context, one.handle));
        case APKRT_MEDIA_PLAYER_SEEK:
            return host_answer(table.seek(table.context, one.handle, asked.time_us,
                                          static_cast<std::uint32_t>(asked.value)));
        case APKRT_MEDIA_PLAYER_STATUS: return status(argument, one);
        case APKRT_MEDIA_PLAYER_SET: return host_answer(table.set(table.context, one.handle, asked.kind, asked.value));
        case APKRT_MEDIA_PLAYER_TRACKS: return tracks(argument, one, asked);
        case APKRT_MEDIA_PLAYER_DATA: return data(one, asked);
        case APKRT_MEDIA_PLAYER_RENDER: return render(one, asked);
        case APKRT_MEDIA_PLAYER_RENDER_WINDOW: return render_window(one, asked);
        default:  // DISCARD
            if (const long taken = take_token(one, asked.token); taken != 0) return taken;
            table.discard(table.context, one.handle, asked.token);
            return 0;
    }
}

}  // namespace

long call(std::uint64_t op, std::uint64_t argument) try {
    if (op < APKRT_MEDIA_PLAYER_CREATE || op > APKRT_MEDIA_PLAYER_DESTROY) return -linux_errno::einval;
    apkrt_media_player asked;  // the guest's struct, read once
    if (argument == 0 ||
        !sys::memory::copy_checked(reinterpret_cast<std::uint64_t>(&asked), argument, sizeof asked)) {
        return -linux_errno::efault;
    }
    switch (op) {
        case APKRT_MEDIA_PLAYER_CREATE: return create(argument);
        case APKRT_MEDIA_PLAYER_DESTROY: return destroy(asked.id);
        default: return operate(op, argument, asked);
    }
} catch (const std::bad_alloc &) {
    return -linux_errno::enomem;
}

}  // namespace apkrt::media_players

extern "C" int apkrt_set_media_players(const apkrt_media_players *set) {
    using namespace apkrt::media_players;
    if (set != nullptr &&
        (set->create == nullptr || set->source == nullptr || set->prepare == nullptr || set->start == nullptr ||
         set->pause == nullptr || set->seek == nullptr || set->status == nullptr || set->set == nullptr ||
         set->tracks == nullptr || set->data == nullptr || set->render == nullptr || set->render_window == nullptr ||
         set->discard == nullptr || set->destroy == nullptr)) {
        return -1;
    }
    auto &all = registry();
    const std::lock_guard hold(all.mutex);
    all.table = set != nullptr ? *set : apkrt_media_players{};
    return 0;
}

extern "C" int apkrt_media_player_post(std::uint64_t id, const apkrt_media_player_event *event) try {
    using namespace apkrt::media_players;
    namespace linux_errno = apkrt::sys::linux_errno;
    if (event == nullptr || !record_holds(*event)) return -linux_errno::einval;
    const auto player = find(id);
    if (player == nullptr) return -linux_errno::ebadf;
    const std::lock_guard records(player->records);
    if (player->write_end < 0) return -linux_errno::ebadf;
    const bool written = write_backlog(*player);
    if (event->type == APKRT_MEDIA_PLAYER_FRAME) {
        // Never before a record that waits, and counted only once written.
        if (!written || player->waiting.size() >= APKRT_MEDIA_PLAYER_MAX_WAITING ||
            player->waiting.count(event->token) != 0 || write_record(*player, *event) == Written::full) {
            return -linux_errno::eagain;
        }
        player->waiting.insert(event->token);
        return 0;
    }
    if (event->type == APKRT_MEDIA_PLAYER_READ && player->read_token != 0) return -linux_errno::ebusy;
    if (!written || write_record(*player, *event) == Written::full) {
        if (player->backlog.size() >= APKRT_MEDIA_PLAYER_MAX_BACKLOG) return -enospc;
        player->backlog.push_back(*event);
    }
    if (event->type == APKRT_MEDIA_PLAYER_READ) {
        player->read_token = event->token;
        player->read_size = event->ext1;
    }
    return 0;
} catch (const std::bad_alloc &) {
    return -apkrt::sys::linux_errno::enomem;
}
