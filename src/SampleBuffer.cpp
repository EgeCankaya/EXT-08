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
    const std::lock_guard<std::mutex> guard(mutex_);
    if (records_.size() >= capacity_) {
        ++notRecorded_;
        return;
    }
    records_.push_back(std::move(sample));
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
