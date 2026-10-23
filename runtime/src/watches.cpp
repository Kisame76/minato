#include "watches.h"

#include "vfs.h"

#include <sys/stat.h>

#include <cerrno>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>

namespace apkrt::watches {

namespace {

struct Entry {
    dev_t device;
    ino_t inode;
    timespec birth;
    std::string path;
    Callback callback;
    void *context;
    int writers = 0;
    int running = 0;     // callbacks in progress on guest threads
    bool ended = false;  // unwatched: no new callback
    bool orphan = false;  // unwatched from inside its own callback: the entry goes when that returns
};

std::mutex mutex;
std::condition_variable idle;
std::map<int, Entry> entries;
int next_id = 1;
thread_local int in_callback = 0;  // the watch whose callback this thread runs

bool same_file(const Entry &entry, const struct stat &info) {
    return !entry.ended && entry.device == info.st_dev && entry.inode == info.st_ino &&
           entry.birth.tv_sec == info.st_birthtimespec.tv_sec && entry.birth.tv_nsec == info.st_birthtimespec.tv_nsec;
}

std::size_t live() {
    std::size_t count = 0;
    for (const auto &entry : entries) count += entry.second.ended ? 0 : 1;
    return count;
}

}  // namespace

int watch(std::string_view guest_path, Callback callback, void *context) {
    if (callback == nullptr || guest_path.empty() || guest_path[0] != '/' || guest_path.size() >= vfs::path_max) {
        return -EINVAL;
    }
    const std::string guest = vfs::normalize(guest_path);
    const auto host = vfs::host_path(guest);
    if (!host) return -ENOENT;
    struct stat info {};
    if (::lstat(host->c_str(), &info) != 0) return -errno;
    if (S_ISLNK(info.st_mode)) return -ELOOP;
    if (!S_ISREG(info.st_mode)) return -EINVAL;
    const std::lock_guard<std::mutex> hold(mutex);
    for (const auto &entry : entries) {
        if (same_file(entry.second, info)) return -EEXIST;
    }
    if (live() >= static_cast<std::size_t>(limit)) return -EMFILE;
    const int id = next_id++;
    entries[id] = Entry{info.st_dev, info.st_ino, info.st_birthtimespec, guest, callback, context};
    return id;
}

int unwatch(int id) {
    std::unique_lock<std::mutex> hold(mutex);
    const auto found = entries.find(id);
    if (found == entries.end() || found->second.ended) return -EINVAL;
    found->second.ended = true;
    if (in_callback == id) {
        found->second.orphan = true;  // the callback's own thread: the entry goes when it returns
        return 0;
    }
    idle.wait(hold, [&] {
        const auto it = entries.find(id);
        return it == entries.end() || it->second.running == 0;
    });
    entries.erase(id);
    return 0;
}

int writer_opened(const struct stat &info) {
    const std::lock_guard<std::mutex> hold(mutex);
    for (auto &entry : entries) {
        if (same_file(entry.second, info)) {
            ++entry.second.writers;
            return entry.first;
        }
    }
    return 0;
}

void writer_closed(int id) {
    Callback callback;
    void *context;
    std::string path;
    {
        const std::lock_guard<std::mutex> hold(mutex);
        const auto found = entries.find(id);
        if (found == entries.end() || --found->second.writers > 0 || found->second.ended) return;
        callback = found->second.callback;
        context = found->second.context;
        path = found->second.path;
        ++found->second.running;
    }
    const int outer = in_callback;
    in_callback = id;
    try {
        callback(context, path.c_str());
    } catch (...) {
        // A host callback is C; whatever it throws is dropped rather than unwound through a destructor.
    }
    in_callback = outer;
    const std::lock_guard<std::mutex> hold(mutex);
    const auto found = entries.find(id);
    if (found != entries.end() && --found->second.running == 0 && found->second.orphan) {
        entries.erase(found);  // unwatch ended it from inside the callback: nobody waits
    }
    idle.notify_all();
}

void reset() {
    const std::lock_guard<std::mutex> hold(mutex);
    entries.clear();
}

}  // namespace apkrt::watches
