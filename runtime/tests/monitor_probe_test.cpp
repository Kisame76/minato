// ADR-0020 X7 (c2): the exclusive-monitor probe on this core, with its controls, and the loader's refusal of a library that
// needs the monitor kept, with the probe's answer faked both ways.
#include "monitor_probe.h"

#include "check.h"
#include "loader.h"

namespace {

using apkrt::monitor::Variant;

// The probe's controls on this machine: a plain LL/SC increment succeeds, one with a CLREX between never does. Without them
// the probe could not tell a core that clears the monitor from one that keeps it.
TEST(the_probes_controls_behave_on_this_core) {
    const auto clean = apkrt::monitor::run(Variant::clean, 2000);
    CHECK(clean.passes == 2000 && clean.livelocked == 0);
    CHECK(clean.first_try + clean.retried == 2000);
    const auto clrex = apkrt::monitor::run(Variant::clrex, 50);
    CHECK(clrex.livelocked == 50);  // a known clear is seen: every pass fails every try
}

// What C3b measured on the A17 Pro and the M4: a load or a store to another granule between the exclusives does not clear
// the monitor (no pass livelocks; an interrupt may cost a retry).
TEST(this_core_keeps_the_monitor_for_a_load_and_a_store_to_another_granule) {
    for (const auto variant : {Variant::load, Variant::store}) {
        const auto result = apkrt::monitor::run(variant, 5000);
        CHECK(result.livelocked == 0);
        CHECK(result.first_try + result.retried == 5000);
    }
    apkrt::monitor::force_for_testing(std::nullopt);
    CHECK(apkrt::monitor::kept());
}

// The loader's check with the answer faked: a library without the flag loads on any core; one with it loads where the
// monitor is kept and is refused, with its own reason, where it is not.
TEST(the_loader_refuses_a_library_that_needs_the_monitor_kept_on_a_core_that_clears_it) {
    apkrt::monitor::force_for_testing(false);
    apkrt::loader::require_monitor_kept(0, "plain.dylib");  // no flag: nothing to ask
    apkrt::loader::require_monitor_kept(APK_CONTAINER_STATIC, "static.dylib");
    bool refused = false;
    try {
        apkrt::loader::require_monitor_kept(APK_CONTAINER_MONITOR_INSIDE, "game.dylib");
    } catch (const apkrt::loader::LoadError &error) {
        refused = std::string_view(error.what()).find("clears it") != std::string_view::npos &&
                  std::string_view(error.what()).find("game.dylib") != std::string_view::npos;
    }
    CHECK(refused);
    apkrt::monitor::force_for_testing(true);
    apkrt::loader::require_monitor_kept(APK_CONTAINER_MONITOR_INSIDE | APK_CONTAINER_STATIC, "game.dylib");
    apkrt::monitor::force_for_testing(std::nullopt);
}

}  // namespace

int main() { return apk::testing::run_all(); }
