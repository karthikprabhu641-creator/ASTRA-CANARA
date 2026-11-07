#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "kvdb/status.hpp"
#include "kvdb/types.hpp"
#include "kvdb/storage/file.hpp"
#include "kvdb/storage/index.hpp"

namespace kvdb::recovery {

struct RecoveryStats {
    Status status{Status::Ok};
    std::uint64_t valid_bytes{0};
    std::uint64_t truncated_bytes{0};
    std::uint64_t recovered_keys{0};
    std::uint64_t committed_transactions{0};
    std::uint64_t uncommitted_transactions{0};
    bool clean_shutdown{false};
    double recovery_time_ms{0.0};
};

class RecoveryEngine {
public:
    // Core recovery execution on database open
    static RecoveryStats recover(storage::File& file, storage::Index& index);

    // Fast integrity verification (non-mutating check)
    static Status verify_integrity(storage::File& file);
};

} // namespace kvdb::recovery
