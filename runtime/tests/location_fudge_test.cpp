// The coarse-location fudger (ADR-0063 decision 3): a coarse grant gets a position snapped to a ~2 km grid with a
// per-app offset, at 2000 m accuracy, never the precise fix. Checks the accuracy, the grid snap and its cell
// periodicity, that nearby positions share a cell, the pole/dateline bounds, and the Fudger's per-window stable /
// across-window redrawn offset. Written to Formate's AVD measurement; ranges, not exact values, are asserted.
#include "location_fudge.h"

#include "check.h"

#include <cmath>

namespace {

using namespace apkrt::location;
constexpr double cell_lat = fudge_grid_meters / meters_per_degree_latitude;

bool near(double a, double b, double eps) { return std::fabs(a - b) < eps; }

}  // namespace

TEST(a_coarse_fix_always_reports_the_coarse_accuracy) {
    CHECK(snap(52.52, 13.405, 0.0, 0.0).accuracy_m == coarse_accuracy_m);
    CHECK(snap(-33.87, 151.21, 500.0, 900.0).accuracy_m == coarse_accuracy_m);
}

TEST(a_position_snaps_to_a_multiple_of_the_latitude_cell_near_the_truth) {
    const FudgedFix f = snap(52.52, 13.405, 0.0, 0.0);
    const double k = f.latitude / cell_lat;
    CHECK(near(k, std::round(k), 1e-6));                      // the latitude is a whole multiple of the cell
    CHECK(near(f.latitude, 52.52, 3000.0 / meters_per_degree_latitude));  // within about 3 km of the truth
}

TEST(positions_within_one_cell_snap_together) {
    const FudgedFix a = snap(52.5200, 13.4050, 0.0, 0.0);
    const FudgedFix b = snap(52.5205, 13.4055, 0.0, 0.0);  // a few hundred metres away, the same cell
    CHECK(a.latitude == b.latitude && a.longitude == b.longitude);
}

TEST(a_whole_cell_of_offset_shifts_the_grid_by_one_cell) {
    const FudgedFix a = snap(52.52, 13.405, 0.0, 0.0);
    const FudgedFix b = snap(52.52, 13.405, fudge_grid_meters, 0.0);  // one cell of latitude offset
    CHECK(near(b.latitude, a.latitude + cell_lat, 1e-6));
}

TEST(latitude_clamps_and_longitude_wraps) {
    const FudgedFix pole = snap(89.999, 179.999, 5000.0, 5000.0);
    CHECK(pole.latitude <= 90.0 && pole.latitude >= -90.0);
    CHECK(pole.longitude >= -180.0 && pole.longitude < 180.0);
}

TEST(the_offset_is_stable_within_a_window_and_redraws_after) {
    Fudger fudger;
    fudger.reseed(12345);
    const FudgedFix first = fudger.fudge(52.52, 13.405, 1'000'000'000);
    const double offset = fudger.offset_latitude_meters();
    const FudgedFix mid = fudger.fudge(52.52, 13.405, 1'000'000'000 + fudge_redraw_ns / 2);
    CHECK(first.latitude == mid.latitude && first.longitude == mid.longitude);  // same window, same cell
    CHECK(fudger.offset_latitude_meters() == offset);
    fudger.fudge(52.52, 13.405, 1'000'000'000 + fudge_redraw_ns + 1);  // past the window
    CHECK(fudger.offset_latitude_meters() != offset);                  // the offset redrew
}

TEST(a_fudger_is_deterministic_for_a_seed_and_differs_across_seeds) {
    Fudger same_a, same_b, other;
    same_a.reseed(7);
    same_b.reseed(7);
    other.reseed(8);
    same_a.fudge(52.52, 13.405, 0);
    same_b.fudge(52.52, 13.405, 0);
    other.fudge(52.52, 13.405, 0);
    CHECK(same_a.offset_latitude_meters() == same_b.offset_latitude_meters());   // same seed, same offset
    CHECK(same_a.offset_latitude_meters() != other.offset_latitude_meters());    // different seed, different offset
}

int main() { return apk::testing::run_all(); }
