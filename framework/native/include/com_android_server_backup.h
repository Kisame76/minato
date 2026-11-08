// The backup service's aconfig flags as the C++ header aconfig would generate them (services/backup/flags.aconfig,
// which the fetched sources do not hold), with the values of Android 15's release build (ap3a): all enabled.
// libandroidfw's BackupHelpers.cpp asks enable_max_size_writes_to_pipes.
#pragma once

namespace com::android::server::backup {

inline bool enable_clear_pipe_after_restore_file() { return true; }
inline bool enable_increase_datatypes_for_agent_logging() { return true; }
inline bool enable_increased_bmm_logging_for_restore_at_install() { return true; }
inline bool enable_max_size_writes_to_pipes() { return true; }
inline bool enable_metrics_system_backup_agents() { return true; }

}  // namespace com::android::server::backup
