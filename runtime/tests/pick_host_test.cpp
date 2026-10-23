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
#include <memory>
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

TEST(a_name_freed_by_the_app_never_hands_back_another_document) {
    Layout layout;
    make_dir(layout.root + "/one");
    make_dir(layout.root + "/two");
    write_file(layout.root + "/one/photo.txt", "from one\n");
    write_file(layout.root + "/two/photo.txt", "from two\n");
    ph::Host host(layout.config());
    std::uint64_t count = 0;
    host.set_files({layout.root + "/one/photo.txt"});
    CHECK(host.documents(1, false, count) && layout.manifest(1) == "photo.txt\n");
    ::unlink((layout.download + "/photo.txt").c_str());                          // the app deletes its clone
    host.set_files({layout.root + "/two/photo.txt"});
    CHECK(host.documents(2, false, count) && layout.manifest(2) == "photo.txt\n");   // the other document takes the name
    host.set_files({layout.root + "/one/photo.txt"});
    CHECK(host.documents(3, false, count) && layout.manifest(3) == "photo (1).txt\n");
    CHECK(read_text(layout.download + "/photo.txt") == "from two\n");
    CHECK(read_text(layout.download + "/photo (1).txt") == "from one\n");
}

TEST(a_file_picked_again_after_the_user_changed_it_gets_a_new_clone_unless_the_app_has_edits) {
    Layout layout;
    ph::Host host(layout.config());
    std::uint64_t count = 0;
    CHECK(host.documents(1, false, count) && layout.manifest(1) == "known.txt\n");
    write_file(layout.source + "/known.txt", "changed elsewhere\n");                  // the user edits the original
    CHECK(host.documents(2, false, count) && layout.manifest(2) == "known (1).txt\n");
    CHECK(read_text(layout.download + "/known (1).txt") == "changed elsewhere\n");
    CHECK(read_text(layout.download + "/known.txt") == "hello\n");                    // the old clone stays as it was
    CHECK(host.documents(3, false, count) && layout.manifest(3) == "known (1).txt\n");  // now the same again
    // An app with unsent edits keeps its clone: the write-back decides what becomes of the original.
    write_file(layout.download + "/known (1).txt", "app edit\n");
    host.write_back().closed("known (1).txt", 1000);
    write_file(layout.source + "/known.txt", "changed again\n");
    CHECK(host.documents(4, false, count) && layout.manifest(4) == "known (1).txt\n");
    CHECK(read_text(layout.download + "/known (1).txt") == "app edit\n");
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

// ---- Writing an edited clone back to its original (ADR-0057, the addendum accepted 2026-10-23) --------------------

/// An original that counts what the host does to it: the Mac's stand-in for the iOS host's bookmark and coordination.
class CountingOriginal : public ph::PathOriginal {
public:
    using ph::PathOriginal::PathOriginal;
    int writes = 0;
    bool fail = false;
    Result replace_if(const ph::Version &expected, const std::string &content, ph::Version &after) override {
        if (fail) return FAILED;
        const Result result = ph::PathOriginal::replace_if(expected, content, after);
        if (result == WRITTEN) ++writes;
        return result;
    }
};

struct Tracked {
    Layout layout;
    std::string state = layout.root + "/host-state";
    ph::WriteBack write_back{layout.download, state};
    CountingOriginal *original = nullptr;
    Tracked() {
        make_dir(state);
        write_file(layout.download + "/known.txt", "hello\n");
        auto owned = std::make_unique<CountingOriginal>(layout.source + "/known.txt");
        original = owned.get();
        ph::Version at_clone;
        CHECK(original->read(cloned_text, at_clone));
        write_back.track("known.txt", std::move(owned), at_clone, at_clone);
    }
    std::string cloned_text;
    std::string known() const { return read_text(layout.source + "/known.txt"); }
};

TEST(an_edited_clone_reaches_its_original_one_second_after_the_last_close) {
    Tracked t;
    write_file(t.layout.download + "/known.txt", "edited\n");
    t.write_back.closed("known.txt", 10000);
    t.write_back.flush(10500, false);
    CHECK(t.known() == "hello\n" && !t.write_back.removable("known.txt"));
    t.write_back.closed("known.txt", 10800);                                 // the app closes again: the clock restarts
    t.write_back.flush(11200, false);
    CHECK(t.known() == "hello\n");
    t.write_back.flush(11800, false);
    CHECK(t.known() == "edited\n" && t.original->writes == 1 && t.write_back.removable("known.txt"));
    t.write_back.flush(20000, true);
    CHECK(t.original->writes == 1);                                          // nothing left to write
}

TEST(a_close_without_a_change_leaves_the_original_untouched) {
    Tracked t;
    struct stat before {};
    ::stat((t.layout.source + "/known.txt").c_str(), &before);
    t.write_back.closed("known.txt", 0);
    t.write_back.flush(5000, false);
    struct stat after {};
    ::stat((t.layout.source + "/known.txt").c_str(), &after);
    CHECK(t.original->writes == 0 && before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec);
    CHECK(t.write_back.removable("known.txt"));
}

TEST(a_write_through_a_mapping_is_found_at_the_end_of_the_session) {
    Tracked t;
    write_file(t.layout.download + "/known.txt", "mapped!\n");                // no close was reported
    t.write_back.flush(5000, false);
    CHECK(t.known() == "hello\n");
    t.write_back.end_session(6000);
    CHECK(t.known() == "mapped!\n" && t.write_back.removable("known.txt"));
}

TEST(an_original_that_changed_meanwhile_is_not_overwritten_and_the_app_version_is_kept) {
    Tracked t;
    write_file(t.layout.source + "/known.txt", "somebody else\n");
    write_file(t.layout.download + "/known.txt", "edited\n");
    t.write_back.closed("known.txt", 0);
    t.write_back.flush(5000, false);
    CHECK(t.known() == "somebody else\n" && t.original->writes == 0);
    CHECK(!t.write_back.removable("known.txt"));
    CHECK(t.write_back.conflicts() == std::vector<std::string>{"known.txt"});
    ::unlink((t.layout.download + "/known.txt").c_str());                    // the guest's cleaning deletes the clone
    CHECK(!names(t.state).empty());                                          // the host holds the app's version
    CHECK(t.write_back.resolve("known.txt", true));
    CHECK(t.known() == "edited\n" && t.write_back.removable("known.txt") && t.write_back.conflicts().empty());
    CHECK(names(t.state) == std::vector<std::string>{"records"} || names(t.state).empty());
}

TEST(a_conflict_the_user_discards_keeps_the_original_and_frees_the_clone) {
    Tracked t;
    write_file(t.layout.source + "/known.txt", "somebody else\n");
    write_file(t.layout.download + "/known.txt", "edited\n");
    t.write_back.closed("known.txt", 0);
    t.write_back.flush(5000, false);
    CHECK(t.write_back.resolve("known.txt", false));
    CHECK(t.known() == "somebody else\n" && t.write_back.removable("known.txt"));
    CHECK(names(t.state) == std::vector<std::string>{"records"} || names(t.state).empty());
}

TEST(an_original_that_is_gone_or_became_a_link_is_a_conflict_and_nothing_is_written_through) {
    Tracked t;
    write_file(t.layout.outside + "/target", "outside\n");
    ::unlink((t.layout.source + "/known.txt").c_str());
    ::symlink((t.layout.outside + "/target").c_str(), (t.layout.source + "/known.txt").c_str());
    write_file(t.layout.download + "/known.txt", "edited\n");
    t.write_back.closed("known.txt", 0);
    t.write_back.flush(5000, false);
    CHECK(read_text(t.layout.outside + "/target") == "outside\n" && t.original->writes == 0);
    CHECK(t.write_back.conflicts() == std::vector<std::string>{"known.txt"});
    ::unlink((t.layout.source + "/known.txt").c_str());
    Tracked gone;
    ::unlink((gone.layout.source + "/known.txt").c_str());
    write_file(gone.layout.download + "/known.txt", "edited\n");
    gone.write_back.closed("known.txt", 0);
    gone.write_back.flush(5000, false);
    CHECK(!exists_nofollow(gone.layout.source + "/known.txt") && !gone.write_back.removable("known.txt"));
}

TEST(a_clone_the_app_replaced_by_a_link_is_never_read_through) {
    Tracked t;
    write_file(t.layout.outside + "/secret", "secret\n");
    ::unlink((t.layout.download + "/known.txt").c_str());
    ::symlink((t.layout.outside + "/secret").c_str(), (t.layout.download + "/known.txt").c_str());
    t.write_back.closed("known.txt", 0);
    t.write_back.end_session(5000);
    CHECK(t.known() == "hello\n" && t.original->writes == 0 && !t.write_back.removable("known.txt"));
}

TEST(an_app_that_saves_by_writing_a_temporary_file_and_renaming_it_over_the_clone_is_found) {
    Tracked t;
    write_file(t.layout.download + "/known.tmp", "renamed over\n");
    CHECK(::rename((t.layout.download + "/known.tmp").c_str(), (t.layout.download + "/known.txt").c_str()) == 0);
    t.write_back.end_session(5000);                                          // no close of the clone's identity came
    CHECK(t.known() == "renamed over\n" && t.write_back.removable("known.txt"));
}

TEST(a_write_that_fails_is_tried_again_and_the_clone_stays) {
    Tracked t;
    write_file(t.layout.download + "/known.txt", "edited\n");
    t.original->fail = true;
    t.write_back.closed("known.txt", 0);
    t.write_back.flush(5000, false);
    CHECK(t.known() == "hello\n" && !t.write_back.removable("known.txt") && t.write_back.conflicts().empty());
    t.original->fail = false;
    t.write_back.flush(6000, false);
    CHECK(t.known() == "edited\n" && t.write_back.removable("known.txt"));
}

TEST(records_survive_the_hosts_restart_and_are_not_taken_from_the_apps_folders) {
    Tracked t;
    write_file(t.layout.download + "/known.txt", "edited\n");
    t.write_back.closed("known.txt", 0);                                     // the host is killed before any flush
    ph::WriteBack again(t.layout.download, t.state);
    CHECK(again.load([&](const std::string &token) { return std::make_unique<CountingOriginal>(token); }));
    again.end_session(5000);
    CHECK(t.known() == "edited\n" && again.removable("known.txt"));
    // A records file planted in the app's folders is not read.
    write_file(t.layout.download + "/records", "known.txt\tD\t" + t.layout.outside + "/x\n");
    ph::WriteBack other(t.layout.download, t.layout.root + "/empty-state");
    CHECK(!other.load([&](const std::string &token) { return std::make_unique<CountingOriginal>(token); }) ||
          !other.tracked("known.txt"));
}

TEST(the_host_tracks_the_clones_it_makes_and_writes_them_back_at_the_session_end) {
    Layout layout;
    ph::Config config = layout.config();
    config.state = layout.root + "/host-state";
    make_dir(config.state);
    ph::Host host(config);
    std::uint64_t count = 0;
    CHECK(host.documents(1, false, count) && host.write_back().tracked("known.txt"));
    write_file(layout.download + "/known.txt", "edited\n");
    host.write_back().closed("known.txt", 0);
    host.write_back().end_session(100);
    CHECK(read_text(layout.source + "/known.txt") == "edited\n");
    CHECK(host.documents(2, false, count) && read_text(layout.download + "/known.txt") == "edited\n");  // same clone
}

// ---- The export of a finished image or video to Photos (ADR-0057 step e), here a copy into a folder ----------------

TEST(a_finished_image_below_the_data_folder_is_exported_by_its_path) {
    Layout layout;
    const std::string target = layout.root + "/photos";
    make_dir(target);
    make_dir(layout.data + "/.minato");
    make_dir(layout.data + "/.minato/library");
    make_dir(layout.data + "/.minato/library/files");
    write_file(layout.data + "/.minato/library/files/shot.jpg", "pixels");
    ph::Config config = layout.config();
    ph::Host host(config);
    CHECK(host.export_media("1\t.minato/library/files/shot.jpg", target));
    CHECK(read_text(target + "/shot.jpg") == "pixels");
    CHECK(host.export_media("2\t.minato/library/files/shot.jpg", target));        // a second one gets another name
    CHECK(names(target) == (std::vector<std::string>{"shot (1).jpg", "shot.jpg"}));
    CHECK(!host.export_media("1\t.minato/library/files/missing.jpg", target));
    CHECK(!host.export_media("1\t.minato/library", target));                         // a folder
    CHECK(!host.export_media("3\t.minato/library/files/shot.jpg", target));          // not a kind
    write_file(layout.outside + "/secret.jpg", "secret");                           // root/outside, beside the data folder
    CHECK(!host.export_media("1\t../outside/secret.jpg", target));                  // only the ".." check stops this
    CHECK(names(target).size() == 2);
}

TEST(an_export_does_not_follow_a_link_the_app_made_in_the_path) {
    Layout layout;
    const std::string target = layout.root + "/photos";
    make_dir(target);
    write_file(layout.outside + "/secret.jpg", "secret");
    make_dir(layout.data + "/.minato");
    ::symlink(layout.outside.c_str(), (layout.data + "/.minato/library").c_str());          // a folder that is a link
    ph::Host host(layout.config());
    CHECK(!host.export_media("1\t.minato/library/secret.jpg", target));
    ::unlink((layout.data + "/.minato/library").c_str());
    make_dir(layout.data + "/.minato/library");
    ::symlink((layout.outside + "/secret.jpg").c_str(), (layout.data + "/.minato/library/shot.jpg").c_str());
    CHECK(!host.export_media("1\t.minato/library/shot.jpg", target));                        // a file that is a link
    CHECK(names(target).empty());
}

TEST(an_export_refuses_a_file_with_more_than_one_link) {
    Layout layout;
    const std::string target = layout.root + "/photos";
    make_dir(target);
    make_dir(layout.data + "/.minato");
    write_file(layout.data + "/.minato/a.jpg", "picture");
    ph::Host host(layout.config());
    CHECK(host.export_media("1\t.minato/a.jpg", target));                                    // one link: taken
    ::link((layout.data + "/.minato/a.jpg").c_str(), (layout.data + "/.minato/b.jpg").c_str());
    CHECK(!host.export_media("1\t.minato/a.jpg", target));                                   // two links: refused
    CHECK(!host.export_media("1\t.minato/b.jpg", target));
    CHECK(names(target).size() == 1);
}

TEST(an_export_never_blocks_on_a_fifo_nor_copies_a_huge_file_nor_leaves_a_partial_one) {
    Layout layout;
    const std::string target = layout.root + "/photos";
    make_dir(target);
    make_dir(layout.data + "/.minato");
    CHECK(::mkfifo((layout.data + "/.minato/pipe.jpg").c_str(), 0600) == 0);
    const int big = ::open((layout.data + "/.minato/big.mp4").c_str(), O_CREAT | O_WRONLY, 0600);
    CHECK(big >= 0 && ::ftruncate(big, std::int64_t{2} << 30) == 0);                          // 2 GiB, sparse
    ::close(big);
    write_file(layout.data + "/.minato/small.jpg", "ok");
    ph::Host host(layout.config());
    ::alarm(20);                                                                              // a hang fails the test
    CHECK(!host.export_media("1\t.minato/pipe.jpg", target));
    CHECK(!host.export_media("2\t.minato/big.mp4", target));
    ::alarm(0);
    CHECK(names(target).empty());
    CHECK(host.export_media("1\t.minato/small.jpg", target) && names(target) == std::vector<std::string>{"small.jpg"});
}

TEST(the_host_takes_the_file_at_once_and_copies_it_after_the_handler_returned) {
    Layout layout;
    const std::string target = layout.root + "/photos";
    make_dir(target);
    make_dir(layout.data + "/.minato");
    write_file(layout.data + "/.minato/a.jpg", "abcdef");
    ph::Host host(layout.config());
    auto job = host.export_begin("1\t.minato/a.jpg");
    CHECK(job.ok() && names(target).empty());                                     // nothing is copied yet
    ::unlink((layout.data + "/.minato/a.jpg").c_str());                           // the app swaps the file meanwhile
    write_file(layout.data + "/.minato/a.jpg", "other!");
    CHECK(job.run(target) && read_text(target + "/a.jpg") == "abcdef");           // the copy is of the file taken
}

TEST(a_copy_that_fails_halfway_leaves_nothing_behind) {
    Layout layout;
    const std::string target = layout.root + "/photos";
    make_dir(target);
    make_dir(layout.data + "/.minato");
    write_file(layout.data + "/.minato/a.jpg", std::string(100000, 'x'));
    ph::Host host(layout.config());
    auto job = host.export_begin("1\t.minato/a.jpg");
    CHECK(job.ok());
    CHECK(::truncate((layout.data + "/.minato/a.jpg").c_str(), 70000) == 0);       // the file shrinks under the copy
    CHECK(!job.run(target));
    CHECK(names(target).empty());                                                  // no half a file
}

TEST(a_session_exports_at_most_its_budget_of_bytes) {
    Layout layout;
    const std::string target = layout.root + "/photos";
    make_dir(target);
    make_dir(layout.data + "/.minato");
    write_file(layout.data + "/.minato/a.jpg", std::string(60, 'a'));
    ph::Config config = layout.config();
    config.export_bytes = 100;
    ph::Host host(config);
    CHECK(host.export_media("1\t.minato/a.jpg", target));                          // 60 of 100
    CHECK(!host.export_media("1\t.minato/a.jpg", target));                         // 120 would be over
    write_file(layout.data + "/.minato/b.jpg", std::string(40, 'b'));
    CHECK(host.export_media("1\t.minato/b.jpg", target));                          // exactly 100
    CHECK(names(target).size() == 2);
}

TEST(a_session_exports_at_most_a_thousand_files) {
    Layout layout;
    const std::string target = layout.root + "/photos";
    make_dir(target);
    make_dir(layout.data + "/.minato");
    write_file(layout.data + "/.minato/a.jpg", "a");
    ph::Host host(layout.config());
    int copied = 0;
    for (int i = 0; i < 1100; ++i) copied += host.export_media("1\t.minato/a.jpg", target) ? 1 : 0;
    CHECK(copied == 1000);
}

TEST(a_clone_the_app_replaced_by_a_fifo_is_a_conflict_not_a_hang) {
    Tracked t;
    ::unlink((t.layout.download + "/known.txt").c_str());
    CHECK(::mkfifo((t.layout.download + "/known.txt").c_str(), 0600) == 0);
    t.write_back.closed("known.txt", 0);
    ::alarm(20);
    t.write_back.end_session(5000);
    ::alarm(0);
    CHECK(t.known() == "hello\n" && !t.write_back.removable("known.txt"));
}

}  // namespace

int main() { return apk::testing::run_all(); }
