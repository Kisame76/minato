// odrefresh's metrics upload for ART (ADR-0033 a): ART calls it at start to hand odrefresh's saved metrics to
// statsd. The guest has neither odrefresh nor statsd, so there are never metrics to upload.
#include "odr_statslog/odr_statslog.h"

namespace art {
namespace odrefresh {

bool UploadStatsIfAvailable(std::string*) {
    return true;
}

}  // namespace odrefresh
}  // namespace art
