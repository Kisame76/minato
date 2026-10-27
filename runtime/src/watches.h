// The host's watches on files the guest writes (ADR-0057, the write-back of an edited document): apkrt_watch_writes
// asks to be told when the guest has finished writing a file. A watch is on the file's identity (device, inode and
// birth time, found once at the call), so a rename does not move it, a new file under the old name is not watched, and
// a reused inode number is not taken for it. Each guest open of a watched file for writing counts one writer, the
// description it makes (descriptors.h) carries the watch and tells when its last guest number closes, and when no
// writer is left the watch's callback runs once, on the guest thread that closed. Not seen: a write through a mapping,
// a truncate by path, a file saved by writing another and renaming it over the name, and a writer that was open
// before the watch began; the host compares the file at its own flush points.
#pragma once

#include <sys/stat.h>
#include <sys/types.h>

#include <string_view>

namespace apkrt::watches {

using Callback = void (*)(void *context, const char *guest_path);

/// The most watches at once.
constexpr int limit = 1024;

/// Watches the regular file at the guest path `guest_path` (absolute): its watch id > 0, or a negative errno:
/// -EINVAL (not absolute, too long, not a regular file or no callback), -ENOENT (no mount covers it, or it is not
/// there), -ELOOP (a link), -EEXIST (the file is watched already), -EMFILE (`limit` watches).
int watch(std::string_view guest_path, Callback callback, void *context);

/// Ends watch `id`; 0, or -EINVAL for an id that is not a watch. When the callback of this watch runs on another
/// thread, it waits until that has returned, so that the host may free the context at once; called from inside the
/// callback it does not wait.
int unwatch(int id);

/// The guest opened the file `info` (an fstat of the open file) for writing: the id of the watch on it, which counts
/// one more writer, or 0 when it is not watched.
int writer_opened(const struct stat &info);

/// The writers of watch `id` that are open now (each description that was opened for writing and has a guest number
/// left), or -1 when `id` is not a watch or has ended. Takes the watch table's lock for the read only, so it is safe
/// beside an unwatch on another thread and inside the watch's own callback.
int writers(int id);

/// A writer of watch `id` is gone. The callback runs when it was the last, unless the watch has ended meanwhile; it
/// runs without a lock of this file held, and an exception out of it is dropped.
void writer_closed(int id);

/// Ends all watches, for tests.
void reset();

}  // namespace apkrt::watches
