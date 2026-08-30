#include "RecordQueue.h"

#include <utility>

namespace n8ro::bridge {

bool parseOverflowPolicy(const std::string& text, OverflowPolicy& out, std::string& error) {
    if (text == "drop_newest") {
        out = OverflowPolicy::DropNewest;
        return true;
    }
    if (text == "drop_oldest") {
        out = OverflowPolicy::DropOldest;
        return true;
    }
    if (text == "block") {
        // BTB-BP-4 lists block as one of three permitted policies, so a caller asking for it
        // has read the requirement rather than guessed. Say why it is not offered.
        error = "--overflow-policy block is not offered by this producer. Blocking this queue "
                "blocks the subscription handler, which stalls the bus delivery thread and "
                "perturbs the run being recorded - the same objection ADR-4 raises to BLOCK at "
                "the bus boundary, one thread later. docs/capture-format-v1.md section 14 "
                "states to consumers that this producer never blocks the bus. Use drop_newest "
                "or drop_oldest";
        return false;
    }
    error = "--overflow-policy must be drop_newest or drop_oldest, got " + text;
    return false;
}

const char* overflowPolicyName(OverflowPolicy policy) {
    switch (policy) {
        case OverflowPolicy::DropNewest: return "drop_newest";
        case OverflowPolicy::DropOldest: return "drop_oldest";
    }
    return "unknown";
}

RecordQueue::RecordQueue(std::size_t sampleCapacity, std::size_t structuralReserve,
                         OverflowPolicy policy)
    : sampleCapacity_(sampleCapacity), structuralReserve_(structuralReserve), policy_(policy) {}

void RecordQueue::offer(CaptureRecord record) {
    const bool structural = isStructuralRecord(record.kind);
    {
        const std::lock_guard<std::mutex> guard(mutex_);
        if (closed_) {
            // Past the end of recording, not a loss. Counted apart from overflow because it
            // is scheduler-dependent and must not reach the file (see QueueCounters).
            if (structural) {
                ++counters_.structuralAfterClose;
            } else {
                ++counters_.samplesAfterClose;
            }
            return;
        }

        if (structural) {
            ++counters_.structuralOffered;
        } else {
            ++counters_.samplesOffered;
        }

        // Two thresholds. A sample yields to a structural record's reserved headroom; a
        // structural record may use the whole queue (D-8).
        const std::size_t limit = structural ? sampleCapacity_ + structuralReserve_
                                             : sampleCapacity_;
        if (records_.size() >= limit) {
            if (policy_ == OverflowPolicy::DropNewest) {
                if (structural) {
                    ++counters_.structuralDropped;
                } else {
                    ++counters_.samplesDropped;
                }
                return;
            }
            // DropOldest. Evict from the front, and count the evicted record under its own
            // kind rather than under the arriving one - the loss is the record that leaves.
            if (!records_.empty()) {
                if (isStructuralRecord(records_.front().kind)) {
                    ++counters_.structuralDropped;
                } else {
                    ++counters_.samplesDropped;
                }
                records_.pop_front();
            }
        }

        records_.push_back(std::move(record));
        if (records_.size() > counters_.highWater) {
            counters_.highWater = records_.size();
        }
    }
    // Outside the lock, so the writer does not wake straight into a contended mutex.
    ready_.notify_one();
}

bool RecordQueue::waitAndDrain(std::vector<CaptureRecord>& out,
                               std::chrono::milliseconds timeout) {
    out.clear();
    std::unique_lock<std::mutex> guard(mutex_);
    // The predicate form re-checks on wake, so a spurious wake or a notify that arrived
    // before the wait began cannot lose a record.
    ready_.wait_for(guard, timeout, [this] { return !records_.empty() || closed_; });

    if (records_.empty()) {
        // Either the timeout elapsed with nothing queued, or the queue is closed and drained.
        // The second is the writer's signal to finish, and it is what `true` means here.
        return closed_;
    }

    out.reserve(records_.size());
    for (CaptureRecord& record : records_) {
        out.push_back(std::move(record));
    }
    records_.clear();
    return true;
}

void RecordQueue::close() {
    {
        const std::lock_guard<std::mutex> guard(mutex_);
        closed_ = true;
    }
    ready_.notify_all();
}

bool RecordQueue::isClosed() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return closed_;
}

QueueCounters RecordQueue::counters() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return counters_;
}

std::size_t RecordQueue::depth() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return records_.size();
}

}  // namespace n8ro::bridge
