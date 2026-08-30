#include "SampleBuffer.h"

#include <utility>

namespace n8ro::bridge {

SampleBuffer::SampleBuffer(std::size_t capacity) : capacity_(capacity) {
    // Reserved up front so the handler never pays for a reallocation mid-run. A growing
    // vector would copy every held StreamValueMap on the pump thread, which is exactly the
    // work a courier must not do.
    records_.reserve(capacity_);
}

void SampleBuffer::offer(CapturedSample sample) {
    bool becameFull = false;
    {
        const std::lock_guard<std::mutex> guard(mutex_);
        if (records_.size() >= capacity_) {
            ++notRecorded_;
            return;
        }
        records_.push_back(std::move(sample));
        becameFull = records_.size() >= capacity_;
    }
    if (becameFull) {
        // Once, on the transition - not on every sample. Notifying outside the lock so the
        // waiter does not wake straight into a contended mutex.
        full_.notify_all();
    }
}

bool SampleBuffer::waitUntilFull(std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> guard(mutex_);
    // The predicate form: it re-checks on wake, so a spurious wake or a notify that arrived
    // before the wait began cannot lose the transition.
    return full_.wait_for(guard, timeout, [this] { return records_.size() >= capacity_; });
}

bool SampleBuffer::atCapacity() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return records_.size() >= capacity_;
}

std::size_t SampleBuffer::size() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return records_.size();
}

std::uint64_t SampleBuffer::notRecorded() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return notRecorded_;
}

const std::vector<CapturedSample>& SampleBuffer::records() const {
    // No lock. Phase 2 runs after the subscriptions are cancelled and the pump is stopped,
    // so there is no writer left to race with; taking the lock here would only make the
    // returned reference's safety look like it came from the lock, which it does not.
    return records_;
}

}  // namespace n8ro::bridge
