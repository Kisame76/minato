// Checks the host side of the system pickers (tools/pick_host.h, ADR-0057): the app can leave links, planted files and
// loops in the folders the host writes to (its data folder, Download), so the host must write nothing through them.
// Each attack here is one the lead's review named, played in a scratch folder: a link planted as the manifest's
// temporary file, a dangling link where a clone should go, a link as .minato or picks, and a name that links to itself.
#include "pick_host.h"

#include "check.h"

#include <dirent.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

namespace ph = pick_host;

std::string scratch() {
    std::string pattern = std::string(::getenv("TMPDIR") != nullptr ? ::getenv("TMPDIR") : "/tmp") + "/pick-host-XXXXXX";
    CHECK(::mkdtemp(pattern.data()) != nullptr);
    return pattern;
}

void make_dir(const std::string &path) { ::mkdir(path.c_str(), 0755); }

void write_file(const std::string &path, const std::string &content) {
    std::ofstream(path, std::ios::binary) << content;
}

std::string read_text(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool exists_nofollow(const std::string &path) {
    struct stat info {};
    return ::lstat(path.c_str(), &info) == 0;
}

std::vector<std::string> names(const std::string &folder) {
    std::vector<std::string> found;
    if (DIR *dir = ::opendir(folder.c_str())) {
        while (const dirent *entry = ::readdir(dir)) {
            const std::string name = entry->d_name;
            if (name != "." && name != "..") found.push_back(name);
        }
        ::closedir(dir);
    }
    std::sort(found.begin(), found.end());
    return found;
}

/// A scratch layout: root/{data,download,outside,source}; the user's files in source.
struct Layout {
    std::string root = scratch(), data = root + "/data", download = root + "/download", outside = root + "/outside",
                source = root + "/source";
    Layout() {
        for (const std::string &folder : {data, download, outside, source}) make_dir(folder);
        write_file(source + "/known.txt", "hello\n");
        write_file(source + "/data.bin", "\x01\x02\x03\x04");
    }
    ph::Config config() const {
        ph::Config config;
        config.files = {source + "/known.txt", source + "/data.bin"};
        config.download = download;
        config.data = data;
        return config;
    }
    std::string manifest(int id) const { return read_text(data + "/.minato/picks/" + std::to_string(id)); }
};

TEST(a_pick_is_cloned_and_its_manifest_written_whole) {
    Layout layout;
    ph::Host host(layout.config());
    std::uint64_t count = 0;
    CHECK(host.documents(1, true, count));
    CHECK(count == 2);
    CHECK(layout.manifest(1) == "known.txt\ndata.bin\n");
    CHECK(read_text(layout.download + "/known.txt") == "hello\n");
    CHECK(names(layout.data + "/.minato/picks") == std::vector<std::string>{"1"});
    CHECK(host.documents(2, false, count) && count == 1 && layout.manifest(2) == "known.txt\n");
    CHECK(names(layout.download) == (std::vector<std::string>{"data.bin", "known.txt"}));
}

TEST(a_dangling_link_where_a_clone_goes_is_not_written_through) {
    Layout layout;
    ::symlink((layout.outside + "/x").c_str(), (layout.download + "/known.txt").c_str());
    ph::Host host(layout.config());
    std::uint64_t count = 0;
    CHECK(host.documents(1, false, count));
    CHECK(!exists_nofollow(layout.outside + "/x"));
    CHECK(layout.manifest(1) == "known (1).txt\n");
    CHECK(read_text(layout.download + "/known (1).txt") == "hello\n");
}

TEST(a_link_planted_as_the_manifests_temporary_file_is_replaced_not_followed) {
    Layout layout;
    write_file(layout.outside + "/victim", "keep me");
    make_dir(layout.data + "/.minato");
    make_dir(layout.data + "/.minato/picks");
    ::symlink((layout.outside + "/victim").c_str(), (layout.data + "/.minato/picks/1.partial").c_str());
    ph::Host host(layout.config());
    std::uint64_t count = 0;
    CHECK(host.documents(1, false, count));
    CHECK(read_text(layout.outside + "/victim") == "keep me");
    CHECK(layout.manifest(1) == "known.txt\n");
    CHECK(!exists_nofollow(layout.data + "/.minato/picks/1.partial"));
}

TEST(a_link_planted_as_the_manifest_itself_is_replaced_not_followed) {
    Layout layout;
    write_file(layout.outside + "/victim", "keep me");
    make_dir(layout.data + "/.minato");
    make_dir(layout.data + "/.minato/picks");
    ::symlink((layout.outside + "/victim").c_str(), (layout.data + "/.minato/picks/1").c_str());
    ph::Host host(layout.config());
    std::uint64_t count = 0;
    CHECK(host.documents(1, false, count));
    CHECK(read_text(layout.outside + "/victim") == "keep me");
    CHECK(layout.manifest(1) == "known.txt\n");
}

TEST(a_link_as_a_folder_of_the_data_folder_makes_the_answer_a_cancel) {
    for (const char *link : {".minato", ".minato/picks"}) {
        Layout layout;
        make_dir(layout.outside + "/elsewhere");
        if (std::string(link) == ".minato/picks") {
            make_dir(layout.data + "/.minato");
            ::symlink((layout.outside + "/elsewhere").c_str(), (layout.data + "/.minato/picks").c_str());
        } else {
            ::symlink((layout.outside + "/elsewhere").c_str(), (layout.data + "/.minato").c_str());
        }
        ph::Host host(layout.config());
        std::uint64_t count = 0;
        CHECK(!host.documents(1, false, count));
        CHECK(names(layout.outside + "/elsewhere").empty());
        ph::Host photos(layout.config());
        CHECK(!photos.location(2, "", count) || names(layout.outside + "/elsewhere").empty());
    }
}

TEST(a_name_that_links_to_itself_does_not_stop_the_host) {
    Layout layout;
    ::symlink("known.txt", (layout.download + "/known.txt").c_str());
    ::symlink("loop", (layout.download + "/loop").c_str());
    ph::Host host(layout.config());
    std::uint64_t count = 0;
    CHECK(host.documents(1, false, count));
    CHECK(layout.manifest(1) == "known (1).txt\n");
}

TEST(a_file_picked_again_is_one_document_unless_the_app_replaced_its_clone) {
    Layout layout;
    ph::Host host(layout.config());
    std::uint64_t count = 0;
    CHECK(host.documents(1, false, count) && host.documents(2, false, count));
    CHECK(layout.manifest(1) == layout.manifest(2));
    CHECK(names(layout.download) == std::vector<std::string>{"known.txt"});
    ::unlink((layout.download + "/known.txt").c_str());
    CHECK(host.documents(3, false, count) && layout.manifest(3) == "known.txt\n");
    CHECK(read_text(layout.download + "/known.txt") == "hello\n");
    ::unlink((layout.download + "/known.txt").c_str());
    ::symlink((layout.outside + "/x").c_str(), (layout.download + "/known.txt").c_str());
    CHECK(host.documents(4, false, count) && layout.manifest(4) == "known (1).txt\n");
    CHECK(!exists_nofollow(layout.outside + "/x"));
}

TEST(photos_are_cloned_below_the_data_folder_with_their_numbers) {
    Layout layout;
    write_file(layout.source + "/p.png", std::string("\x89PNG\r\n\x1a\n\0\0\0\rIHDR\0\0\0\x40\0\0\0\x30", 24));
    ph::Config config = layout.config();
    config.photos = {layout.source + "/p.png", layout.source + "/known.txt"};
    ph::Host host(config);
    std::uint64_t count = 0;
    CHECK(host.photos(1, 1, count) && count == 1);
    const std::string line = layout.manifest(1);
    CHECK(line.rfind("p.png\t64\t48\t0\t", 0) == 0);
    CHECK(line.size() > 15 && line.substr(line.size() - 3) == "\t0\n");
    CHECK(read_text(layout.data + "/.minato/photos/p.png").size() == 24);
}

TEST(a_save_creates_its_file_under_a_plain_unique_name_and_a_folder_is_named) {
    Layout layout;
    ph::Config config = layout.config();
    config.tree = "Download/treetest";
    ph::Host host(config);
    std::uint64_t count = 0;
    CHECK(host.location(1, "note.txt", count) && layout.manifest(1) == "Download/note.txt\n");
    CHECK(host.location(2, "note.txt", count) && layout.manifest(2) == "Download/note (1).txt\n");
    CHECK(host.location(3, "../x\ty.txt", count) && layout.manifest(3) == "Download/.._x_y.txt\n");
    CHECK(host.location(4, "", count) && layout.manifest(4) == "Download/treetest\n");
    CHECK(read_text(layout.download + "/note.txt").empty());
    ::unlink((layout.download + "/note (1).txt").c_str());
    ::symlink((layout.outside + "/x").c_str(), (layout.download + "/note (1).txt").c_str());
    CHECK(host.location(5, "note.txt", count) && layout.manifest(5) == "Download/note (2).txt\n");
    CHECK(!exists_nofollow(layout.outside + "/x"));
}

TEST(a_users_file_name_becomes_a_name_the_guest_accepts) {
    CHECK(ph::plain_name("known.txt") == "known.txt");
    CHECK(ph::plain_name("note \xF0\x9F\x98\x80 \xC3\xA4.txt") == "note \xF0\x9F\x98\x80 \xC3\xA4.txt");
    CHECK(ph::plain_name("a/b") == "a_b");
    CHECK(ph::plain_name("a\tb\nc\rd") == "a_b_c_d");
    CHECK(ph::plain_name("a\xE2\x80\x8D" "b") == "a_b");                   // zero-width joiner
    CHECK(ph::plain_name("a\xE2\x80\xAE" "b") == "a_b");                   // right-to-left override
    CHECK(ph::plain_name("a\xE2\x80\xA8" "b") == "a_b");                   // line separator
    CHECK(ph::plain_name("a\xEF\xBB\xBF" "b") == "a_b");                   // byte order mark
    CHECK(ph::plain_name("a\xC2\x85" "b") == "a_b");                       // C1 control
    CHECK(ph::plain_name("a\x01" "b\x7F") == "a_b_");
    CHECK(ph::plain_name("a\xFF" "b") == "a_b");                           // not UTF-8
    CHECK(ph::plain_name("a\xC0\x80" "b") == "a__b");                      // overlong NUL
    CHECK(ph::plain_name("a\xED\xA0\x80" "b") == "a___b");                 // a surrogate
    CHECK(ph::plain_name("") == "Untitled");
    CHECK(ph::plain_name(".") == "Untitled");
    CHECK(ph::plain_name("..") == "Untitled");
    const std::string longest = ph::plain_name(std::string(400, 'x') + ".jpg");
    CHECK(longest.size() == 255 && longest.substr(251) == ".jpg");
    std::string emoji;
    for (int i = 0; i < 100; ++i) emoji += "\xF0\x9F\x98\x80";
    const std::string cut = ph::plain_name(emoji + ".txt");
    CHECK(cut.size() <= 255 && cut.substr(cut.size() - 4) == ".txt");
    CHECK((cut.size() - 4) % 4 == 0);                                       // cut between code points
    const std::string numbered = ph::numbered(std::string(255, 'y') + "", 3);
    CHECK(numbered.size() <= 255 && numbered.substr(numbered.size() - 4) == " (3)");
}

}  // namespace

int main() { return apk::testing::run_all(); }
