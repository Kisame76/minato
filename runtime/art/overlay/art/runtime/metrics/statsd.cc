// Minato (ADR-0033 a): ART's statsd backend, in place of AOSP's metrics/statsd.cc. The guest has no statsd, so the
// runtime gets no backend and registers no pull callback, as in ART's host build (statsd.h). With it, nothing of
// ART links libstatspull, libstatssocket or the generated statslog_art.
#include "metrics/statsd.h"

#include "base/metrics/metrics.h"

namespace art HIDDEN {
namespace metrics {

std::unique_ptr<MetricsBackend> CreateStatsdBackend() {
    return nullptr;
}

void SetupCallbackForDeviceStatus() {}

}  // namespace metrics
}  // namespace art
