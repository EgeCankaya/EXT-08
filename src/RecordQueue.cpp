#include "RecordQueue.h"

#include <algorithm>
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
            // DropOldest. Evict the OLDEST SAMPLE, never a structural record.
            //
            // The two thresholds above are only half of the reserve; this is the other half.
            // Popping records_.front() unconditionally would let an arriving sample - whose
            // threshold is sampleCapacity_ - evict an entity_add sitting at the front of a
            // creation burst, which is exactly the trade D-8 was written to forbid and which
            // docs/capture-format-v1.md section 16 tells a reader it can lean on: "overload
            // costs data and never structure". So the scan skips structural records and takes
            // the first sample behind them. In practice that is the front, because samples
            // outnumber events by three orders of magnitude on this platform.
            //
            // If the queue holds nothing but structural records there is no sample to give
            // up, and the arriving record is refused instead - counted under its own kind, so
            // the loss is still exactly one record and still exactly one count.
            const auto oldestSample =
                std::find_if(records_.begin(), records_.end(), [](const CaptureRecord& queued) {
                    return !isStructuralRecord(queued.kind);
                });
            if (oldestSample == records_.end()) {
                if (structural) {
                    ++counters_.structuralDropped;
                } else {
                    ++counters_.samplesDropped;
                }
                return;
            }
            // Counted under the kind of the record that LEAVES, which is always a sample here.
            ++counters_.samplesDropped;
            records_.erase(oldestSample);
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
