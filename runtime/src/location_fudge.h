// The runtime's coarse-location fudger (ADR-0063 decision 3). When a guest holds only a coarse location grant, the
// runtime hands it a position snapped to a ~2 km grid with a per-app offset that redraws periodically, never the
// precise fix, and an accuracy of 2000 m, so averaging many fixes cannot recover the precise position. This runs on
// the trusted side (the runtime), never in the guest. Written to Formate's AVD measurement (apk-runner-35,
// Android 15): coarse accuracy 2000 m, latitude cell 0.018018 deg (2000/111000), longitude cell 2000/(111000*cos lat),
// a random shift before snapping, redrawn about every 10 minutes. The exact offset size and redraw interval are
// measured-approximate until AOSP's LocationFudger.java is read (the user has not approved that download).
#pragma once

#include <cmath>
#include <cstdint>
#include <random>

namespace apkrt::location {

constexpr double fudge_grid_meters = 2000.0;
constexpr double meters_per_degree_latitude = 111000.0;  // matches the AVD's 0.018018 deg latitude cell
constexpr float coarse_accuracy_m = 2000.0f;
constexpr std::int64_t fudge_redraw_ns = 600LL * 1000 * 1000 * 1000;  // ~10 min, measured-approximate

struct FudgedFix {
    double latitude;
    double longitude;
    float accuracy_m;
};

/// Snaps (`lat`, `lon`) to the coarse grid after a per-app offset in metres (`off_lat_m`, `off_lon_m`, both >= 0):
/// the result is a multiple of the cell, within about a cell of the true position, reported at coarse_accuracy_m.
inline FudgedFix snap(double lat, double lon, double off_lat_m, double off_lon_m) {
    constexpr double pi = 3.14159265358979323846;
    const double cell_lat = fudge_grid_meters / meters_per_degree_latitude;
    const double off_lat_deg = off_lat_m / meters_per_degree_latitude;
    double lat_s = std::round((lat + off_lat_deg) / cell_lat) * cell_lat;
    if (lat_s > 90.0) lat_s = 90.0;
    if (lat_s < -90.0) lat_s = -90.0;
    // The longitude cell is taken from the SNAPPED latitude, so every point in a latitude cell shares one longitude
    // grid and nearby points snap to the same cell (a cell width from the raw latitude would differ point to point).
    double mpd_lon = meters_per_degree_latitude * std::cos(lat_s * (pi / 180.0));
    if (mpd_lon < 1.0) mpd_lon = 1.0;  // near the poles the longitude cell would blow up; floor the metres/degree
    const double cell_lon = fudge_grid_meters / mpd_lon;
    const double off_lon_deg = off_lon_m / mpd_lon;
    double lon_s = std::round((lon + off_lon_deg) / cell_lon) * cell_lon;
    lon_s = std::fmod(lon_s + 180.0, 360.0);  // wrap into [-180, 180)
    if (lon_s < 0.0) lon_s += 360.0;
    lon_s -= 180.0;
    return {lat_s, lon_s, coarse_accuracy_m};
}

/// A per-session coarse fudger: it holds the per-app offset and redraws it every fudge_redraw_ns, so a guest sees a
/// stable coarse cell within a window and a different one across windows.
class Fudger {
public:
    void reseed(std::uint64_t seed) {
        prng_.seed(seed);
        drawn_ns_ = unset;
    }
    FudgedFix fudge(double lat, double lon, std::int64_t now_ns) {
        maybe_redraw(now_ns);
        return snap(lat, lon, off_lat_m_, off_lon_m_);
    }
    double offset_latitude_meters() const { return off_lat_m_; }    // for tests
    double offset_longitude_meters() const { return off_lon_m_; }   // for tests

private:
    static constexpr std::int64_t unset = INT64_MIN;
    void maybe_redraw(std::int64_t now_ns) {
        if (drawn_ns_ != unset && now_ns - drawn_ns_ < fudge_redraw_ns) return;
        std::uniform_real_distribution<double> cell(0.0, fudge_grid_meters);
        off_lat_m_ = cell(prng_);
        off_lon_m_ = cell(prng_);
        drawn_ns_ = now_ns;
    }
    std::mt19937_64 prng_{0x9e3779b97f4a7c15ULL};
    double off_lat_m_ = 0.0;
    double off_lon_m_ = 0.0;
    std::int64_t drawn_ns_ = unset;
};

}  // namespace apkrt::location
