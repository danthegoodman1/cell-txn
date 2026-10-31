// Replay checks a recorded history for serializability.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "db.h"
#include "record.h"

namespace celltnx::check {

// Replay reruns the logs serially: writers at their commitTs, then readers
// and constraint rejections at the timestamp they saw. Every logged read
// and write outcome must match the model, every committed row must pass
// its CHECKs, every rejection must be justified, and the final model must
// equal dump(tbl) for every base table. Failures are Code::kInternal with
// the divergence in detail.
Status Replay(const std::vector<Schema> &schemas, const std::vector<Log> &logs, uint64_t last,
              const std::function<std::vector<OwnedRec>(int tbl)> &dump);

}  // namespace celltnx::check
