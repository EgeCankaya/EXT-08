#include "CaptureWriter.h"

#include <core/logging/GlobalLogger.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <utility>

namespace n8ro::bridge {
namespace {

constexpr const char* kCategory = "n8ro-bridge";

// Every close reason the format's closed set allows, as spelled in the file.
constexpr const char* kReasonScenarioUnloaded = "scenario_unloaded";
// A segment cut by the byte bound rather than ended by the scenario. Already in the format's
// closed set (spec 7) - CAP-6 adds no vocabulary, which is why it is not a version bump.
constexpr const char* kReasonSizeLimit = "size_limit";

}  // namespace

const char* sizeLimitActionName(SizeLimitAction action) {
    switch (action) {
        case SizeLimitAction::Stop:   return "stop";
        case SizeLimitAction::Rotate: return "rotate";
    }
    return "stop";
}

bool parseSizeLimitAction(const std::string& text, SizeLimitAction& out, std::string& error) {
    if (text == "stop") {
        out = SizeLimitAction::Stop;
        return true;
    }
    if (text == "rotate") {
        out = SizeLimitAction::Rotate;
        return true;
    }
    error = "--on-size-limit takes stop or rotate, got " + text;
    return false;
}

const char* endReasonName(EndReason reason) {
    switch (reason) {
        case EndReason::Shutdown:  return "shutdown";
        case EndReason::HostLost:  return "host_lost";
        case EndReason::SizeLimit: return "size_limit";
    }
    return "shutdown";
}

std::string CaptureWriter::scenarioSlug(const std::string& scenario) {
    std::string slug;
    bool pendingHyphen = false;
    for (const char raw : scenario) {
        const unsigned char c = static_cast<unsigned char>(raw);
        if (std::isalnum(c) != 0) {
            if (pendingHyphen && !slug.empty()) {
                slug.push_back('-');
            }
            pendingHyphen = false;
            slug.push_back(static_cast<char>(std::tolower(c)));
        } else {
            // Runs of anything else collapse to one hyphen, and a trailing run produces
            // none - so the transform is total and cannot emit a name ending in punctuation.
            pendingHyphen = true;
        }
    }
    // A scenario name with no alphanumeric characters at all, or none reported yet.
    return slug.empty() ? std::string("unknown") : slug;
}

std::string CaptureWriter::nextRunLabel(const std::string& outDir, const std::string& slug) {
    // Zero-padded ordinal derived from what already exists in the directory. Never a
    // timestamp: campaign tooling addresses runs by path, and a wall-clock name makes two
    // identical runs unaddressable as a pair (ADR-3, and the PRD's file conventions).
    const std::string prefix = "capture-" + slug + "-";
    const std::string suffix = ".n8rocap.jsonl";

    std::error_code ec;
    std::uint64_t highest = 0;
    bool any = false;
    std::filesystem::directory_iterator it(outDir, ec);
    if (!ec) {
        for (const std::filesystem::directory_entry& entry : it) {
            const std::string name = entry.path().filename().string();
            if (name.size() <= prefix.size() + suffix.size()) {
                continue;
            }
            if (name.compare(0, prefix.size(), prefix) != 0) {
                continue;
            }
            if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
                continue;
            }
            const std::string middle =
                name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
            if (middle.empty() ||
                !std::all_of(middle.begin(), middle.end(),
                             [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
                continue;
            }
            const std::uint64_t ordinal = std::strtoull(middle.c_str(), nullptr, 10);
            if (!any || ordinal > highest) {
                highest = ordinal;
                any = true;
            }
        }
    }

    const std::uint64_t next = any ? highest + 1 : 0;
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%03llu", static_cast<unsigned long long>(next));
    return std::string(buffer);
}

std::string CaptureWriter::partFileName(const std::string& slug, const std::string& runLabel,
                                        std::uint64_t part) {
    // Part 0 is the name this producer has always written. Anything else would rename every
    // capture in the repository and every path in the documentation to buy nothing, since a
    // run that never reaches its bound has exactly one part.
    if (part == 0) {
        return "capture-" + slug + "-" + runLabel + ".n8rocap.jsonl";
    }
    char ordinal[32];
    std::snprintf(ordinal, sizeof(ordinal), "%03llu", static_cast<unsigned long long>(part));
    return "capture-" + slug + "-" + runLabel + ".part" + ordinal + ".n8rocap.jsonl";
}

CaptureWriter::CaptureWriter(std::string outDir, std::string runLabel,
                             capture::HeaderInfo header, n8ro::sim::MessageSchema stateSchema,
                             std::size_t maxSamples, std::uint64_t maxBytes,
                             SizeLimitAction action, std::function<bool()> attachedMidRun,
                             std::function<std::string()> lastKnownScenario,
                             std::function<TrailerState()> liveTrailerState)
    : outDir_(std::move(outDir)),
      runLabel_(std::move(runLabel)),
      header_(std::move(header)),
      stateSchema_(std::move(stateSchema)),
      maxSamples_(maxSamples),
      maxBytes_(maxBytes),
      sizeLimitAction_(action),
      attachedMidRun_(std::move(attachedMidRun)),
      lastKnownScenario_(std::move(lastKnownScenario)),
      liveTrailerState_(std::move(liveTrailerState)),
      presence_(stateSchema_) {
    // The header states the bound it was written under (BTB-CAP-6). Set here rather than by
    // the caller so the file can never disagree with the writer that enforced it.
    header_.limits.maxBytes = maxBytes_;
    header_.limits.maxSamples = static_cast<std::uint64_t>(maxSamples_);
    header_.limits.onSizeLimit = sizeLimitActionName(sizeLimitAction_);
}

void CaptureWriter::setReferee(std::unique_ptr<Referee> referee) {
    referee_ = std::move(referee);
}

void CaptureWriter::drainVerdicts() {
    if (!referee_) {
        return;
    }
    for (const Verdict& verdict : referee_->drainVerdicts()) {
        // Through the bound like any other data record. If it does not fit and the action
        // is stop, it goes into neither file - keeping the capture and the verdict stream
        // saying the same thing, which is what BTB-REF-4's comparison rests on.
        // The segment is restamped at render time rather than taken from the Verdict as
        // decided. A rotation resets the ordinal, and a `verdict` record - like every other
        // record - names a segment in the file that contains it (spec 5.2, 7). Without this a
        // verdict crossing a rotation would point at a segment its own file does not have.
        if (!admitData([&] {
                Verdict placed = verdict;
                placed.segment = segmentOrdinal_;
                return writeVerdict(placed);
            }, lastRecordSimTimeS_)) {
            return;
        }
        ++counts_.verdicts;
        ++runCounts_.verdicts;
        if (verdictFile_) {
            Verdict placed = verdict;
            placed.segment = segmentOrdinal_;
            verdictFile_ << writeVerdict(placed) << '\n';
        }
    }
}

void CaptureWriter::emit(const std::string& line) {
    if (failed_) {
        return;
    }
    file_ << line << '\n';
    // Exact, not estimated. The file is opened in binary mode, so no translation happens
    // between this count and the bytes on disk, and this is the only place a record reaches
    // the file - which is what lets the bound be enforced before a line rather than after it.
    bytesWritten_ += static_cast<std::uint64_t>(line.size()) + 1;
    recordsWritten_.fetch_add(1);
    if (!file_) {
        // One diagnostic, then stop writing. A half-written line is worse than a short file,
        // and the trailer path checks failed_ before it tries to add one.
        N8RO_LOG_ERROR(std::string("write failed on capture file ") + path_ +
                           "; no further records will be written",
                       kCategory);
        failed_ = true;
    }
}

bool CaptureWriter::ensureOpen(const std::string& scenarioForName) {
    if (opened_ || failed_) {
        return opened_;
    }

    if (slug_.empty()) {
        slug_ = scenarioSlug(scenarioForName);
    }
    const std::string& slug = slug_;
    if (runLabel_.empty()) {
        runLabel_ = nextRunLabel(outDir_, slug);
    }

    const std::filesystem::path target =
        std::filesystem::path(outDir_) / partFileName(slug, runLabel_, part_);
    path_ = target.string();
    bytesWritten_ = 0;

    // Binary mode on purpose. The format is LF-terminated, and Windows' text mode would
    // translate every one to CRLF - which would make a capture written here differ
    // byte-for-byte from the same capture written anywhere else, defeating BTB-CAP-3.
    file_.open(path_, std::ios::binary | std::ios::trunc);
    if (!file_) {
        N8RO_LOG_ERROR(std::string("could not open capture file for writing: ") + path_,
                       kCategory);
        failed_ = true;
        return false;
    }

    // Evaluated exactly here, once. Before this point nothing has been written, so there is
    // no clock and no race in the answer - it is a function of what the message stream
    // contained (BTB-CAP-3, and M4's fix to this same field).
    header_.attachedMidRun = attachedMidRun_ ? attachedMidRun_() : false;

    // Rotation linkage, written into every part's own header. `continues_from` is a bare
    // filename rather than a path: the parts of a set live in one directory by construction,
    // and an absolute path would leak this host's layout into a cross-repo artifact.
    header_.part = part_;
    header_.continuesFrom =
        part_ == 0 ? std::string() : partFileName(slug, runLabel_, part_ - 1);

    opened_ = true;
    partPaths_.push_back(path_);
    emit(capture::writeHeader(header_));

    if (referee_ && part_ == 0) {
        // One verdict file per RUN, not per part. It is opened with the first part and stays
        // open across rotations: a verdict is a statement about the run, and splitting the
        // stream that BTB-REF-4 compares against a replay would make that comparison a
        // multi-file join for no gain.
        const std::filesystem::path verdicts =
            std::filesystem::path(outDir_) /
            ("verdicts-" + slug + "-" + runLabel_ + ".jsonl");
        verdictPath_ = verdicts.string();
        verdictFile_.open(verdictPath_, std::ios::binary | std::ios::trunc);
        if (!verdictFile_) {
            // Not fatal to the capture. The verdicts are still written into the capture
            // itself as `verdict` records, which is what the format specifies; the separate
            // file is the convenience that makes a live-versus-replay comparison a diff.
            N8RO_LOG_ERROR(std::string("could not open verdict file for writing: ") +
                               verdictPath_ + "; verdicts will appear in the capture only",
                           kCategory);
            verdictPath_.clear();
        }
    }

    N8RO_LOG_INFO(std::string("capture opened: ") + path_ + " format " + capture::kFormatVersion +
                      " producer " + capture::kProducerVersion + " attached_mid_run=" +
                      (header_.attachedMidRun ? "true" : "false"),
                  kCategory);
    return !failed_;
}

bool CaptureWriter::wouldBreachBound(std::size_t lineBytes) const {
    if (maxBytes_ == 0) {
        return false;
    }
    // The record's own length plus its LF, plus the space held back to close the file. All
    // three are known before anything is written, which is the whole point: BTB-CAP-6 says
    // "never silently truncate", and a check made after the write could only report one.
    const std::uint64_t after = bytesWritten_ + static_cast<std::uint64_t>(lineBytes) + 1;
    return after + kCloseReserveBytes > maxBytes_;
}

bool CaptureWriter::rotate(double simTimeS) {
    if (rotating_) {
        // Re-entered from the staging flush inside our own openSegment(). One rotation
        // cannot fix what a second would be asked to fix - the record does not fit in a fresh
        // part - so the run ends here rather than opening parts forever.
        N8RO_LOG_ERROR(std::string("a record does not fit in a freshly-opened capture part at "
                                   "--capture-max-bytes ") +
                           std::to_string(maxBytes_) +
                           "; stopping rather than rotating without making progress",
                       kCategory);
        budgetReached_.store(true);
        return false;
    }
    rotating_ = true;
    // Cleared on every exit below; rotate() has several, and none of them may leave the flag
    // set - a stuck flag would silently turn rotation off for the rest of the run.
    const struct Guard {
        bool& flag;
        ~Guard() { flag = false; }
    } guard{rotating_};

    const bool hadSegment = segmentOpen_;
    const std::string scenario = currentScenario_;
    const std::string nextName = partFileName(slug_, runLabel_, part_ + 1);

    // Close this part exactly as a terminal capture is closed - segment first, then a trailer
    // whose end_reason says why. The only difference is `continued_in`, which is what tells a
    // reader this file ends but the run does not.
    closeSegment(simTimeS, kReasonSizeLimit);

    capture::TrailerCounts counts;
    counts.segments = counts_.segments;
    counts.samples = counts_.samples;
    counts.entitiesAdded = counts_.entitiesAdded;
    counts.entitiesRemoved = counts_.entitiesRemoved;
    counts.verdicts = counts_.verdicts;

    // Producer-side drops and the platform's counters as they stand now. The caller is not
    // here to supply them - it is blocked on nothing and will not run again until teardown -
    // so the callback set at construction is what fills them in.
    const TrailerState state = liveTrailerState_ ? liveTrailerState_() : TrailerState{};
    emit(capture::writeTrailer(simTimeS, endReasonName(EndReason::SizeLimit), counts,
                               state.drops, state.busMetrics, nextName));
    file_.flush();
    const bool closedCleanly = !failed_ && static_cast<bool>(file_);
    file_.close();
    if (!closedCleanly) {
        N8RO_LOG_ERROR(std::string("could not close capture part cleanly: ") + path_ +
                           "; stopping rather than rotating onto a broken file",
                       kCategory);
        failed_ = true;
        budgetReached_.store(true);
        return false;
    }

    N8RO_LOG_INFO(std::string("size limit reached at ") + std::to_string(bytesWritten_) +
                      " bytes; part " + std::to_string(part_) + " closed as " + path_ +
                      ", continuing into " + nextName,
                  kCategory);

    // A new part is a new file in every sense the format cares about: its own header with its
    // own schemas, its own segment ordinals from 0, and its own counts. That is what keeps
    // each part independently readable by a reader that knows nothing about rotation.
    ++part_;
    opened_ = false;
    counts_ = WriterCounts{};
    segmentOpen_ = false;
    anySegmentOpened_ = false;
    segmentOrdinal_ = 0;

    if (!ensureOpen(scenario.empty() ? lastScenarioSeen_ : scenario)) {
        budgetReached_.store(true);
        return false;
    }

    // The guard against rotating forever. If a part cannot hold one more record after its own
    // header, rotating again would produce an endless run of header-and-trailer files; saying
    // so once and stopping is the only honest end.
    if (maxBytes_ != 0 && bytesWritten_ + kCloseReserveBytes >= maxBytes_) {
        N8RO_LOG_ERROR(std::string("--capture-max-bytes ") + std::to_string(maxBytes_) +
                           " is too small to hold a header (" + std::to_string(bytesWritten_) +
                           " bytes) plus the " + std::to_string(kCloseReserveBytes) +
                           " bytes reserved to close a file. Stopping rather than rotating "
                           "into empty parts",
                       kCategory);
        budgetReached_.store(true);
        return false;
    }

    if (hadSegment) {
        // The run did not stop, so the segment it was in did not either. It reopens in the new
        // part at ordinal 0, and the `size_limit` close in the previous part is what says the
        // two are one segment cut in half rather than two segments.
        openSegment(scenario, simTimeS);
    }
    return true;
}

void CaptureWriter::noteStoppedAtBound() {
    // The record that did not fit is not written - writing a partial one is exactly what
    // BTB-CAP-6 forbids - and the run ends at the next turn of the main loop with
    // end_reason=size_limit. The space to close the file properly was reserved before the
    // first record went in, so the trailer is never the thing that does not fit.
    if (!budgetReached_.exchange(true)) {
        N8RO_LOG_INFO(std::string("size limit reached at ") + std::to_string(bytesWritten_) +
                          " bytes of " + std::to_string(maxBytes_) +
                          "; closing the capture with end_reason=size_limit",
                      kCategory);
    }
    recordsPastBound_.fetch_add(1);
}

void CaptureWriter::openSegment(const std::string& scenario, double simTimeS) {
    // Ordinals start at 0 and strictly increase; the first segment is 0 and every later one
    // is the previous plus one, never reused within a file (format spec section 7).
    if (anySegmentOpened_) {
        ++segmentOrdinal_;
    }
    anySegmentOpened_ = true;
    segmentOpen_ = true;
    currentScenario_ = scenario;
    ++counts_.segments;
    ++runCounts_.segments;
    // One span per segment, opened empty. It stays empty if the segment carries no samples,
    // which the summary reports as such rather than as a 0.0 -> 0.0 range it never observed.
    segmentSpans_.push_back(SegmentSpan{segmentOrdinal_, 0, 0.0, 0.0, part_});
    emit(capture::writeSegmentOpen(simTimeS, segmentOrdinal_, currentScenario_));
    flushStaging();
}

void CaptureWriter::closeSegment(double simTimeS, const char* reason) {
    if (!segmentOpen_) {
        return;
    }
    emit(capture::writeSegmentClose(simTimeS, segmentOrdinal_, currentScenario_, reason));
    segmentOpen_ = false;
}

void CaptureWriter::flushStaging() {
    // In arrival order, into the segment that just opened. These are the roster records that
    // preceded their own scenario_loaded - see the header comment and D-1.
    while (!staging_.empty()) {
        const CaptureRecord record = std::move(staging_.front());
        staging_.pop_front();
        if (record.kind == RecordKind::EntityAdd) {
            if (!admitData([&] {
                    return capture::writeEntityAdd(record.simTimeS, segmentOrdinal_,
                                                   record.subject, record.occupancy);
                }, record.simTimeS)) {
                return;
            }
            ++counts_.entitiesAdded;
            ++runCounts_.entitiesAdded;
            if (referee_) {
                referee_->onEntityAdd(record.subject, record.occupancy, record.simTimeS,
                                      segmentOrdinal_);
            }
        } else if (record.kind == RecordKind::EntityRemove) {
            if (!admitData([&] {
                    return capture::writeEntityRemove(record.simTimeS, segmentOrdinal_,
                                                      record.subject, record.occupancy,
                                                      record.reason);
                }, record.simTimeS)) {
                return;
            }
            ++counts_.entitiesRemoved;
            ++runCounts_.entitiesRemoved;
            if (referee_) {
                referee_->onEntityRemove(record.subject, record.occupancy, record.simTimeS,
                                         segmentOrdinal_, record.reason);
            }
        }
        lastRecordSimTimeS_ = record.simTimeS;
        lastDataSimTimeS_ = record.simTimeS;
        lastDataSegment_ = segmentOrdinal_;
        drainVerdicts();
    }
}

void CaptureWriter::apply(const CaptureRecord& record) {
    switch (record.kind) {
        case RecordKind::ScenarioUnloaded: {
            if (record.subject.empty()) {
                // Bring-up noise. The engine publishes one unload with an empty scenario name
                // during initialisation, when nothing has ever been loaded; treating it as a
                // boundary would emit an unnamed segment (notes.md, M1).
                ++counts_.unloadNoiseIgnored;
                return;
            }
            lastScenarioSeen_ = record.subject;
            if (segmentOpen_) {
                closeSegment(record.simTimeS, kReasonScenarioUnloaded);
            }
            lastRecordSimTimeS_ = record.simTimeS;
            return;
        }

        case RecordKind::ScenarioLoaded: {
            lastScenarioSeen_ = record.subject;
            if (!ensureOpen(record.subject)) {
                return;
            }
            if (segmentOpen_) {
                // A load with no unload before it. The previous scenario is gone either way,
                // and scenario_unloaded is the closed-set value that describes it (D-10).
                closeSegment(record.simTimeS, kReasonScenarioUnloaded);
            }
            openSegment(record.subject, record.simTimeS);
            lastRecordSimTimeS_ = record.simTimeS;
            return;
        }

        case RecordKind::EntityAdd:
        case RecordKind::EntityRemove: {
            if (!segmentOpen_) {
                // No segment to belong to yet. Wait for the scenario_loaded that this burst
                // is materialising, or for a sample to force the issue (D-1, D-2).
                if (staging_.size() >= kStagingCapacity) {
                    ++counts_.stagedDropped;
                    return;
                }
                staging_.push_back(record);
                return;
            }
            if (record.kind == RecordKind::EntityAdd) {
                if (!admitData([&] {
                        return capture::writeEntityAdd(record.simTimeS, segmentOrdinal_,
                                                       record.subject, record.occupancy);
                    }, record.simTimeS)) {
                    return;
                }
                ++counts_.entitiesAdded;
                ++runCounts_.entitiesAdded;
                if (referee_) {
                    referee_->onEntityAdd(record.subject, record.occupancy, record.simTimeS,
                                          segmentOrdinal_);
                }
            } else {
                if (!admitData([&] {
                        return capture::writeEntityRemove(record.simTimeS, segmentOrdinal_,
                                                          record.subject, record.occupancy,
                                                          record.reason);
                    }, record.simTimeS)) {
                    return;
                }
                ++counts_.entitiesRemoved;
                ++runCounts_.entitiesRemoved;
                if (referee_) {
                    referee_->onEntityRemove(record.subject, record.occupancy, record.simTimeS,
                                             segmentOrdinal_, record.reason);
                }
            }
            lastRecordSimTimeS_ = record.simTimeS;
            lastDataSimTimeS_ = record.simTimeS;
            lastDataSegment_ = segmentOrdinal_;
            drainVerdicts();
            return;
        }

        case RecordKind::Sample: {
            if (maxSamples_ != 0 && runCounts_.samples >= maxSamples_) {
                // Past the budget. Not a drop - the budget is what ends recording, and
                // `end_reason: size_limit` says so. Counted for the log only, because it is
                // scheduler-dependent (the same reasoning M4 applied to this number).
                recordsPastBound_.fetch_add(1);
                return;
            }
            // Naming the file needs a scenario. Prefer one the bus actually announced; fall
            // back to the client's mirrored value for a bridge that attached mid-run and has
            // therefore never seen a scenario_loaded (D-11).
            std::string nameForFile = lastScenarioSeen_;
            if (nameForFile.empty() && lastKnownScenario_) {
                nameForFile = lastKnownScenario_();
            }
            if (!ensureOpen(nameForFile)) {
                return;
            }
            if (!segmentOpen_) {
                // The state model's attach-mid-run branch: "segment_open on scenario_loaded,
                // or first sample when attached mid-run". The scenario name may be empty, and
                // the format allows that (section 7) rather than inventing one.
                openSegment(lastScenarioSeen_, record.simTimeS);
            }
            // Rendered and admitted first, so that everything counted below is counted only
            // if it actually reached a file. A rotation happens inside here, which is why the
            // span bookkeeping that follows reads segmentSpans_.back() rather than a span
            // captured before the call - after a rotation the back() is the new part's.
            if (!admitData([&] { return capture::writeSample(record, segmentOrdinal_,
                                                             stateSchema_); },
                           record.simTimeS)) {
                return;
            }
            presence_.note(record.values);
            ++counts_.samples;
            ++runCounts_.samples;
            samplesWritten_.fetch_add(1);
            if (!segmentSpans_.empty()) {
                SegmentSpan& span = segmentSpans_.back();
                if (span.samples == 0) {
                    span.minSimTimeS = record.simTimeS;
                    span.maxSimTimeS = record.simTimeS;
                } else {
                    span.minSimTimeS = (std::min)(span.minSimTimeS, record.simTimeS);
                    span.maxSimTimeS = (std::max)(span.maxSimTimeS, record.simTimeS);
                }
                ++span.samples;
            }
            lastRecordSimTimeS_ = record.simTimeS;
            lastDataSimTimeS_ = record.simTimeS;
            lastDataSegment_ = segmentOrdinal_;
            if (referee_) {
                // Driven from the record that was just written, so a verdict can never refer
                // to a sample the capture does not contain (BTB-REF-2).
                const StreamValueSource source(record.values);
                referee_->onSample(record.subject, record.occupancy, record.simTimeS,
                                   segmentOrdinal_, source);
                drainVerdicts();
            }
            // Run-wide, not per part. counts_.samples resets on rotation, and a record
            // budget that reset with it would silently become a per-file quota (D-13, D-16).
            if (maxSamples_ != 0 && runCounts_.samples >= maxSamples_) {
                budgetReached_.store(true);
            }
            return;
        }
    }
}

void CaptureWriter::run(RecordQueue& queue) {
    std::vector<CaptureRecord> batch;
    for (;;) {
        const bool closedAndDrained =
            queue.waitAndDrain(batch, std::chrono::milliseconds(100)) && batch.empty();
        for (const CaptureRecord& record : batch) {
            apply(record);
        }
        if (!batch.empty() && opened_ && !failed_) {
            // Batched, never per record. Buffering improves throughput and widens the
            // lost-tail window on an abnormal termination; flushing once per drained batch is
            // the compromise the PRD's optimisation notes ask for - bounded interval, not
            // bounded by record count.
            file_.flush();
        }
        if (closedAndDrained) {
            return;
        }
    }
}

std::vector<std::string> CaptureWriter::neverPublishedFields() const {
    return presence_.neverPublished();
}

bool CaptureWriter::finish(EndReason reason, const capture::TrailerDrops& drops,
                           const capture::TrailerBusMetrics& busMetrics) {
    // A run that recorded nothing still leaves a valid capture. The format explicitly permits
    // a file that is header then trailer with zero segments (section 7), and it is a far more
    // useful artifact than no file at all: it carries the resolved topic, the schemas, the
    // subscription policy and every drop counter, which between them say why it is empty.
    if (!opened_) {
        const std::string name =
            !lastScenarioSeen_.empty()
                ? lastScenarioSeen_
                : (lastKnownScenario_ ? lastKnownScenario_() : std::string());
        if (!ensureOpen(name)) {
            return false;
        }
    }
    if (failed_) {
        return false;
    }

    // Anything still staged never found a segment - the host died between a creation burst
    // and its scenario_loaded. Open one for it rather than discard the records: they are real
    // observations, and dropping them silently is what tenet 3 forbids.
    if (!staging_.empty() && !segmentOpen_) {
        openSegment(lastScenarioSeen_, staging_.front().simTimeS);
    }

    if (referee_) {
        // Explicit not-met verdicts for everything never satisfied (BTB-REF-2). Emitted
        // before the segment closes, so every verdict record sits inside a segment as the
        // format requires, and anchored on the last *data* record so replay reaches the same
        // stamp from the same file.
        for (const Verdict& verdict : referee_->finalVerdicts(lastDataSegment_,
                                                              lastDataSimTimeS_)) {
            const std::string line = writeVerdict(verdict);
            ++counts_.verdicts;
            ++runCounts_.verdicts;
            // Emitted directly rather than through the bound. These are the last records of
            // the run and they are what kCloseReserveBytes is held back for - a final not-met
            // verdict suppressed by the byte bound would mean a capture that ends without
            // saying what it decided, which is a worse failure than a file a few hundred
            // bytes over its limit. The reserve is sized so that it is not one.
            emit(line);
            if (verdictFile_) {
                verdictFile_ << line << '\n';
            }
        }
    }

    closeSegment(lastRecordSimTimeS_, endReasonName(reason));

    capture::TrailerCounts counts;
    counts.segments = counts_.segments;
    counts.samples = counts_.samples;
    counts.entitiesAdded = counts_.entitiesAdded;
    counts.entitiesRemoved = counts_.entitiesRemoved;
    counts.verdicts = counts_.verdicts;

    emit(capture::writeTrailer(lastRecordSimTimeS_, endReasonName(reason), counts, drops,
                               busMetrics));
    file_.flush();
    const bool ok = !failed_ && static_cast<bool>(file_);
    file_.close();
    if (verdictFile_) {
        verdictFile_.flush();
        verdictFile_.close();
    }
    if (!ok) {
        N8RO_LOG_ERROR(std::string("capture file was opened but writing failed: ") + path_,
                       kCategory);
    }
    return ok;
}

}  // namespace n8ro::bridge
