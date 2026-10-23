// The runtime's permission grant store (ADR-0063 location, ADR-0064 microphone): the host's latest answer per group,
// which the location syscalls read to gate access. Checks the default (unknown before any answer), per-group set and
// read-back, that an unknown group is ignored, and that reset forgets everything.
#include "permissions.h"

#include "check.h"

namespace {

constexpr std::int32_t location = APKRT_PERMISSION_LOCATION, microphone = APKRT_PERMISSION_MICROPHONE;
constexpr int unknown = apkrt::permissions::unknown;

}  // namespace

TEST(before_an_answer_a_group_is_unknown) {
    apkrt::permissions::reset();
    CHECK(apkrt::permissions::outcome(location) == unknown);
    CHECK(apkrt::permissions::outcome(microphone) == unknown);
}

TEST(a_set_outcome_is_read_back_per_group) {
    apkrt::permissions::reset();
    apkrt::permissions::set(location, APKRT_PERMISSION_GRANTED_COARSE);
    CHECK(apkrt::permissions::outcome(location) == APKRT_PERMISSION_GRANTED_COARSE);
    CHECK(apkrt::permissions::outcome(microphone) == unknown);  // the other group is untouched
    apkrt::permissions::set(microphone, APKRT_PERMISSION_GRANTED);
    CHECK(apkrt::permissions::outcome(microphone) == APKRT_PERMISSION_GRANTED);
    apkrt::permissions::set(location, APKRT_PERMISSION_DENIED);  // a later answer replaces the earlier
    CHECK(apkrt::permissions::outcome(location) == APKRT_PERMISSION_DENIED);
}

TEST(an_unknown_group_is_ignored) {
    apkrt::permissions::reset();
    apkrt::permissions::set(0, APKRT_PERMISSION_GRANTED);
    apkrt::permissions::set(99, APKRT_PERMISSION_GRANTED);
    CHECK(apkrt::permissions::outcome(0) == unknown);
    CHECK(apkrt::permissions::outcome(99) == unknown);
    CHECK(apkrt::permissions::outcome(location) == unknown);  // nothing leaked into a real group
}

TEST(reset_forgets_every_group) {
    apkrt::permissions::set(location, APKRT_PERMISSION_GRANTED);
    apkrt::permissions::set(microphone, APKRT_PERMISSION_GRANTED);
    apkrt::permissions::reset();
    CHECK(apkrt::permissions::outcome(location) == unknown);
    CHECK(apkrt::permissions::outcome(microphone) == unknown);
}

int main() { return apk::testing::run_all(); }
