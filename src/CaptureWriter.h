// EXT-08 Bus Telemetry Bridge - M5: the writer thread and the capture session's state
// machine (BTB-CX-4, BTB-EP-3, BTB-BP-1, BTB-CAP-2).
//
// Everything expensive happens here and only here: float conversion, JSON encoding, and all
// file IO. The bus handlers copy into a RecordQueue and return (CLAUDE.md hard rule 2).
//
// This replaces M4's buffer-then-dump entirely. What survives unchanged is CaptureFormat's
// serialiser, which is pure functions over their arguments and never knew where its records
// came from.
//
// ---------------------------------------------------------------------------------------
// The segment state machine
//
// The PRD's state model reads
//
//     waiting -> attached -> segment_open <=> segment_closed -> closing -> closed
//
// and M5 implements all of it. The one thing the model did not anticipate is what M5
// measured on the live bus (docs/decisions-m5-m7.md, D-1): **the entity_created burst that
// materialises a scenario arrives before the scenario_loaded that announces it**, at
// bring-up and at every reload.
//
//     seq 6   scenario_unloaded  ""                <- bring-up noise, empty scenario name
//     seq 7..48   entity_created x 42              <- the burst, BEFORE the load
//     seq 49  scenario_loaded    "Atacama Air Defense"
//
// So a `scenario_loaded` is a completion announcement, not a start. Closing on the unload
// and opening on the load, with nothing in between, would leave 42 entity_add records
// outside any segment - which docs/capture-format-v1.md section 7 calls malformed.
//
// The staging area is the answer. Roster records that arrive while no segment is open wait
// there and flush into the segment that opens next, so each creation burst lands in the
// segment it belongs to. A `sample` never waits: if one arrives while no segment is open the
// segment opens immediately (the state model's attach-mid-run branch), which is also what
// bounds the staging area in practice - it can only grow with a creation burst, which is
// entity-count sized.
//
// ---------------------------------------------------------------------------------------
// Threading
//
// run() is the writer thread's whole body. open-on-demand, finish() and every getter that
// is not atomic are called from our own thread *after* the writer thread has been joined.
// The queue is the only thing the two threads share.

#pragma once

#include "CaptureFormat.h"
#include "CaptureRecord.h"
#include "RecordQueue.h"
#include "Referee.h"

#include <messaging/packed/MessageSchema.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace n8ro::bridge {

// Why a capture stopped. These map one-to-one onto the format's closed `end_reason` set.
enum class EndReason {
    Shutdown,     // the operator stopped the producer (M7 - signal handling)
    HostLost,     // the heartbeat went silent (BTB-CX-3)
    SizeLimit,    // a configured bound was reached - bytes (BTB-CAP-6) or records (D-13)
};

[[nodiscard]] const char* endReasonName(EndReason reason);

// What BTB-CAP-6 calls "one documented action". The FR offers two and requires the producer
// to take one of them and to say which in the header; this is the operator's choice, not a
// compiled-in decision, because which one is right depends on whether the run's tail or the
// host's free space matters more - and nobody but the operator knows that.
enum class SizeLimitAction {
    Stop,      // close the capture with a terminal trailer and end the run
    Rotate,    // close this part and continue into a numbered continuation file
};

[[nodiscard]] const char* sizeLimitActionName(SizeLimitAction action);
[[nodiscard]] bool parseSizeLimitAction(const std::string& text, SizeLimitAction& out,
                                        std::string& error);

// The counters a trailer needs that the writer does not itself own. The final part's trailer
// gets these from the caller at teardown, exactly as before; an intermediate part's trailer -
// which is written by the writer thread mid-run, when the caller is not there to be asked -
// gets them through this callback instead.
//
// The callback runs ON THE WRITER THREAD, so whatever it reads must be safe to read there.
// It is deliberately not given the bus objects: `RecordQueue::counters()` is mutex-guarded
// and safe, but nothing establishes that for the SDK's own metric accessors, and reaching
// into them off the main thread to fill in a counter would be trading a real invariant for a
// cosmetic one. main() caches its own bus snapshot instead - see kBusMetricsAreAsOfLastPoll.
struct TrailerState {
    capture::TrailerDrops drops;
    capture::TrailerBusMetrics busMetrics;
};

struct WriterCounts {
    std::uint64_t segments = 0;
    std::uint64_t samples = 0;
    std::uint64_t entitiesAdded = 0;
    std::uint64_t entitiesRemoved = 0;
    std::uint64_t verdicts = 0;
    std::uint64_t stagedDropped = 0;     // staging area overflow - see kStagingCapacity
    std::uint64_t unloadNoiseIgnored = 0; // scenario_unloaded with an empty scenario name
};

class CaptureWriter {
public:
    // The staging area's bound. It only ever holds one creation burst - 42 records on the
    // reference scenario, 126 on the overload one - because a sample forces the segment open
    // and flushes it. Three orders of magnitude of headroom, and overflow is counted.
    static constexpr std::size_t kStagingCapacity = 8192;

    // Room held back from the byte bound so that closing a part is always possible. A
    // trailer measures ~520 bytes with every counter present, a segment_close ~110, and a
    // run's final not-met verdicts a few hundred each; 8 KiB covers all of it with two
    // orders of magnitude of headroom on the trailer itself.
    //
    // This is what makes BTB-CAP-6's "never silently truncate" true rather than hoped for:
    // the bound is checked BEFORE a line is written and against the line's real length, so a
    // record is either written whole or not written at all, and the space to close the file
    // properly was reserved before the first sample went in.
    static constexpr std::uint64_t kCloseReserveBytes = 8192;

    // `attachedMidRun` is evaluated once, when the file is opened, and answers the causal
    // question M4 settled: did samples arrive for entities whose creation we never saw?
    // Never a clock and never a status tick (BTB-CAP-3).
    //
    // `maxBytes` is BTB-CAP-6's bound and `maxSamples` the older record-count safety bound
    // (D-13); either may be 0, meaning unbounded, and they are independent. `action` applies
    // to the byte bound only - a record budget always stops, because rotating on it would
    // make `--capture-max-samples` a per-file quota rather than the run bound it has always
    // been documented as.
    CaptureWriter(std::string outDir, std::string runLabel, capture::HeaderInfo header,
                  n8ro::sim::MessageSchema stateSchema, std::size_t maxSamples,
                  std::uint64_t maxBytes, SizeLimitAction action,
                  std::function<bool()> attachedMidRun,
                  std::function<std::string()> lastKnownScenario,
                  std::function<TrailerState()> liveTrailerState);

    // Optional. When present, the referee is driven from the same record stream the writer is
    // already draining, in the same order, so a verdict lands in the capture at the position
    // and in the segment where it was decided. Set before run(); owned from here on.
    //
    // Verdicts go to two places: into the capture as `verdict` records, and into a separate
    // `verdicts-<scenario>-<run-label>.jsonl` beside it. The first is what the format
    // specifies; the second is what replay can also produce, which is how "live verdicts
    // equal replay verdicts" becomes a file comparison (BTB-REF-4).
    void setReferee(std::unique_ptr<Referee> referee);

    [[nodiscard]] const std::string& verdictPath() const { return verdictPath_; }

    // The writer thread's body. Drains until the queue is closed and empty, then returns.
    void run(RecordQueue& queue);

    // Called from our own thread after the writer thread is joined. Closes any open segment,
    // writes the trailer, flushes and closes the file. If nothing ever opened the file, it is
    // opened here so that a run which recorded nothing still leaves a valid, complete, empty
    // capture - header then trailer, which the format explicitly permits (section 7).
    [[nodiscard]] bool finish(EndReason reason, const capture::TrailerDrops& drops,
                              const capture::TrailerBusMetrics& busMetrics);

    // Set once the sample budget is reached, so the main loop can tear down promptly rather
    // than a status tick later. Read from our own thread.
    [[nodiscard]] bool budgetReached() const { return budgetReached_.load(); }

    // Live counters for the status line. Approximate by design - they are read without a
    // lock while the writer runs, and are exact once it is joined.
    [[nodiscard]] std::uint64_t samplesWritten() const { return samplesWritten_.load(); }
    [[nodiscard]] std::uint64_t recordsWritten() const { return recordsWritten_.load(); }
    // Records the producer received but did not write because a bound had been reached.
    // Not a loss and not in the file: a bound is what ends recording, and `end_reason:
    // size_limit` says so. Any record type can land here now that the bound is in bytes,
    // which is why this is not called "samples".
    [[nodiscard]] std::uint64_t recordsPastBound() const { return recordsPastBound_.load(); }

    // Valid after finish(). Empty until the file is opened. On a rotated run this is the
    // LAST part; parts() is the whole set in order.
    [[nodiscard]] const std::string& path() const { return path_; }

    // Every part written, in order, oldest first. One entry for a capture that never rotated.
    [[nodiscard]] const std::vector<std::string>& parts() const { return partPaths_; }

    // Counts for the CURRENT part only - which is what the format specifies a trailer's
    // `counts` to mean, "what is in this file" (spec 11). runCounts() is the whole run, and
    // is what the summary reports, because an operator asking how much a run recorded does
    // not mean how much landed in its last part.
    [[nodiscard]] const WriterCounts& counts() const { return counts_; }
    [[nodiscard]] const WriterCounts& runCounts() const { return runCounts_; }
    [[nodiscard]] std::uint64_t rotations() const { return part_; }
    [[nodiscard]] bool failed() const { return failed_; }
    [[nodiscard]] std::vector<std::string> neverPublishedFields() const;
    [[nodiscard]] double lastRecordSimTimeS() const { return lastRecordSimTimeS_; }

    // The simulation-time span of the samples in one segment, min to max.
    //
    // Reported per segment rather than as a single file-wide first-to-last pair. A complete
    // live run always ends with a teardown reload whose clock has been reset to 0 (spec 5.1),
    // so the last sample written is 0.0 on every such run and a "first written -> last
    // written" line reads 0.0 -> 0.0 however long the run was - hiding the entire run behind
    // its own teardown. min/max per segment says what actually happened and makes the reset
    // visible as its own segment.
    struct SegmentSpan {
        std::uint64_t ordinal = 0;
        std::uint64_t samples = 0;
        double minSimTimeS = 0.0;
        double maxSimTimeS = 0.0;
        std::uint64_t part = 0;   // which part of a rotated set it landed in
    };
    [[nodiscard]] const std::vector<SegmentSpan>& segmentSpans() const { return segmentSpans_; }

    // Turns a scenario name into the filename component: lowercase, runs of anything outside
    // [a-z0-9] collapsed to one hyphen, ends trimmed. Total, deterministic, and derived from
    // the scenario name alone (D-12). Exposed for the tests.
    [[nodiscard]] static std::string scenarioSlug(const std::string& scenario);

    // Picks `<run-label>` when the caller supplied none: the lowest zero-padded ordinal not
    // already present in `outDir` for this scenario slug. Never a timestamp (ADR-3).
    [[nodiscard]] static std::string nextRunLabel(const std::string& outDir,
                                                  const std::string& slug);

    // The filename of one part of a rotated set. Part 0 is the plain, unchanged name, so a
    // capture that never rotates is byte-for-byte and name-for-name what it was before
    // CAP-6 existed; parts 1 and up interpose `.partNNN` before the extension.
    //
    // Part 0 keeping the ordinary name is deliberate. nextRunLabel() scans the directory for
    // `capture-<slug>-<digits>.n8rocap.jsonl` and requires the middle to be all digits, so
    // `capture-atacama-000.part001.n8rocap.jsonl` is skipped by it - a rotated set therefore
    // consumes exactly one run-label ordinal, not one per part.
    [[nodiscard]] static std::string partFileName(const std::string& slug,
                                                  const std::string& runLabel,
                                                  std::uint64_t part);

private:
    void apply(const CaptureRecord& record);
    bool ensureOpen(const std::string& scenarioForName);
    void openSegment(const std::string& scenario, double simTimeS);
    void closeSegment(double simTimeS, const char* reason);
    void flushStaging();
    void emit(const std::string& line);
    void drainVerdicts();

    // True when `lineBytes` more would leave less than kCloseReserveBytes of the byte bound.
    [[nodiscard]] bool wouldBreachBound(std::size_t lineBytes) const;

    // The one place a data record meets the byte bound. Writes the record and returns true,
    // or - when writing it would breach the bound - either rotates into a new part and writes
    // it there, or stops recording and returns false without writing anything.
    //
    // Structural records (the header, a segment_open or segment_close, the trailer) never go
    // through here: they are what kCloseReserveBytes is reserved for, and a bound that could
    // suppress the trailer would defeat the requirement it is implementing.
    //
    // `render` produces the record's line, and is called AGAIN after a rotation. It has to be:
    // a rotation resets the segment ordinal to 0, and every record names the segment in the
    // file that actually contains it (spec 5.2). Rendering once and writing that same line
    // into the next part would put a record carrying `"segment":3` into a file whose only
    // segment is 0 - a malformed capture, and one no reader could repair.
    //
    // A template rather than a std::function so that the common path - no bound configured,
    // or nowhere near it - is a direct call with nothing allocated per record.
    template <typename Render>
    [[nodiscard]] bool admitData(Render&& render, double simTimeS) {
        std::string line = render();
        if (!wouldBreachBound(line.size())) {
            emit(line);
            return true;
        }
        if (sizeLimitAction_ == SizeLimitAction::Rotate) {
            if (!rotate(simTimeS)) {
                return false;
            }
            // Re-rendered against the new part's segment ordinal. The guard in rotate() has
            // already proved the new part has room for a record.
            emit(render());
            return true;
        }
        noteStoppedAtBound();
        return false;
    }

    // The stop half of admitData, factored out only so the template above stays small.
    void noteStoppedAtBound();

    // Closes the current part with a `size_limit` trailer naming its successor, opens the
    // successor, and re-opens the segment that was open. Returns false if the successor could
    // not be opened or cannot hold a record, in which case the run stops.
    bool rotate(double simTimeS);

    std::string outDir_;
    std::string runLabel_;
    capture::HeaderInfo header_;
    n8ro::sim::MessageSchema stateSchema_;
    std::size_t maxSamples_ = 0;
    std::uint64_t maxBytes_ = 0;
    SizeLimitAction sizeLimitAction_ = SizeLimitAction::Stop;
    std::function<bool()> attachedMidRun_;
    std::function<std::string()> lastKnownScenario_;
    std::function<TrailerState()> liveTrailerState_;

    std::ofstream file_;
    std::string path_;
    bool opened_ = false;
    bool failed_ = false;

    // Rotation state. `slug_` is remembered from the first open so that every later part is
    // named from the same scenario the set started with, even if the scenario changes mid-run
    // - the parts of one set belong to one file name by definition.
    std::string slug_;
    std::uint64_t part_ = 0;
    std::vector<std::string> partPaths_;
    // Bytes written to the CURRENT part, counted at the one place records are emitted. Exact:
    // the file is opened in binary mode and every record is `line` plus one LF, so this is
    // the file's length and not an estimate of it.
    std::uint64_t bytesWritten_ = 0;
    // Re-entrancy guard. rotate() re-opens the segment that was open, and openSegment()
    // flushes the staging area, and a staged record goes through admitData() - which can
    // decide to rotate. Nothing in the arithmetic makes that recursion terminate: it would
    // take a record too large to fit in a fresh part, which needs an unusually large schema
    // table and a bound near its floor, but "needs an unusual combination" is not the same as
    // "cannot happen". The guard makes it structurally impossible instead of arguably rare.
    bool rotating_ = false;

    // Segment state.
    bool segmentOpen_ = false;
    std::uint64_t segmentOrdinal_ = 0;
    bool anySegmentOpened_ = false;
    std::string currentScenario_;
    std::string lastScenarioSeen_;

    // Roster records waiting for a segment to belong to.
    std::deque<CaptureRecord> staging_;

    std::unique_ptr<Referee> referee_;
    std::ofstream verdictFile_;
    std::string verdictPath_;

    capture::FieldPresence presence_;
    // Per part - the trailer's `counts` is "what is in this file" (spec 11), so this resets
    // on rotation. runCounts_ never resets and is what the run summary reports.
    WriterCounts counts_;
    WriterCounts runCounts_;
    std::vector<SegmentSpan> segmentSpans_;
    double lastRecordSimTimeS_ = 0.0;

    // The last record that carried data rather than a boundary. End-of-run verdicts are
    // stamped from these so that live and replay agree exactly: replay reads a file whose
    // boundary records it also sees, and anchoring on the last *data* record is the one
    // point both paths can reach identically (BTB-REF-4).
    double lastDataSimTimeS_ = 0.0;
    std::uint64_t lastDataSegment_ = 0;

    std::atomic<bool> budgetReached_{false};
    std::atomic<std::uint64_t> samplesWritten_{0};
    std::atomic<std::uint64_t> recordsWritten_{0};
    std::atomic<std::uint64_t> recordsPastBound_{0};
};

}  // namespace n8ro::bridge
