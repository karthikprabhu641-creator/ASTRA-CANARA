#pragma once

#include <shared_mutex>
#include <mutex>

namespace kvdb::concurrency {

class LockManager {
public:
    using SharedLock = std::shared_lock<std::shared_mutex>;
    using ExclusiveLock = std::unique_lock<std::shared_mutex>;

    LockManager() = default;

    // Legacy manual lock operations remain available to existing callers.
    void lock_shared() { mutex_.lock_shared(); }
    void unlock_shared() { mutex_.unlock_shared(); }

    void lock_exclusive() { mutex_.lock(); }
    void unlock_exclusive() { mutex_.unlock(); }

    std::shared_mutex& mutex() { return mutex_; }
    SharedLock acquire_shared() const { return SharedLock(mutex_); }
    ExclusiveLock acquire_exclusive() const { return ExclusiveLock(mutex_); }

private:
    mutable std::shared_mutex mutex_;
};

} // namespace kvdb::concurrency
