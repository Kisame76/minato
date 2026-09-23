// apk-run: runs the program in a container dylib on the Mac, one program per process (ADR-0019).
// The tests start it once per fixture; the exit status is the program's own.
//   apk-run [--mount <guest path>=<host path>]... [--libraries <guest directory>=<host directory>]...
//           [--cwd <guest path>] [--env <name>=<value>]... [--argv0 <text>] <container.dylib> [argument...]
// --mount puts a host directory under an Android path (ARCHITECTURE.md 4.5), --libraries makes the
// library containers of a host directory the libraries in a guest directory, e.g. /system/lib64.
// --cwd sets the working directory the program starts in ("/" by default), --env adds to the
// environment an Android shell gives a program, and --argv0 is how the program was started
// (/data/local/tmp/<container name> by default).
#include "APKRuntime/apkrt.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: apk-run [--mount <guest path>=<host path>]... [--libraries <guest directory>=<host directory>]...\n"
                 "               [--cwd <guest path>] [--env <name>=<value>]... [--argv0 <text>] <container.dylib> "
                 "[argument...]\n");
    return 2;
}

/// Splits <left>=<right>; false without an equals sign.
bool split(const std::string &value, std::string &left, std::string &right) {
    const auto equals = value.find('=');
    if (equals == std::string::npos) return false;
    left = value.substr(0, equals);
    right = value.substr(equals + 1);
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    char error[1024] = "";
    // The environment an Android shell gives a program rather than the Mac's.
    std::vector<std::string> environment{
        "PATH=/product/bin:/apex/com.android.runtime/bin:/system/bin:/system/xbin",
        "ANDROID_DATA=/data",
        "ANDROID_ROOT=/system",
        "HOME=/",
    };
    std::string argv0;
    int index = 1;
    for (; index < argc && std::string_view(argv[index]).starts_with("--"); ++index) {
        const std::string_view option = argv[index];
        if (index + 1 >= argc) return usage();
        const std::string value = argv[++index];
        std::string left, right;
        int result = 0;
        if (option == "--mount") {
            if (!split(value, left, right)) return usage();
            result = apkrt_mount(left.c_str(), right.c_str(), error, sizeof error);
        } else if (option == "--libraries") {
            if (!split(value, left, right)) return usage();
            result = apkrt_add_library_directory(left.c_str(), right.c_str(), error, sizeof error);
        } else if (option == "--cwd") {
            result = apkrt_set_working_directory(value.c_str(), error, sizeof error);
        } else if (option == "--env") {
            if (!split(value, left, right)) return usage();
            std::erase_if(environment, [&](const std::string &entry) { return entry.starts_with(left + "="); });
            environment.push_back(value);
        } else if (option == "--argv0") {
            argv0 = value;
        } else {
            return usage();
        }
        if (result != 0) {
            std::fprintf(stderr, "apk-run: %s\n", error);
            return 2;
        }
    }
    if (index >= argc) return usage();

    // Without --argv0 the program sees itself under an Android path, named after its container.
    const char *const container = argv[index];
    if (argv0.empty()) argv0 = "/data/local/tmp/" + std::filesystem::path(container).stem().string();
    std::vector<const char *> arguments{argv0.c_str()};
    for (++index; index < argc; ++index) arguments.push_back(argv[index]);
    arguments.push_back(nullptr);
    std::vector<const char *> variables;
    for (const auto &entry : environment) variables.push_back(entry.c_str());
    variables.push_back(nullptr);

    apkrt_run_program(container, arguments.data(), variables.data(), error, sizeof error);
    std::fprintf(stderr, "apk-run: %s\n", error);
    return 127;
}
