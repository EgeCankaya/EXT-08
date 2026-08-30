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

}  // namespace

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

CaptureWriter::CaptureWriter(std::string outDir, std::string runLabel,
                             capture::HeaderInfo header, n8ro::sim::MessageSchema stateSchema,
                             std::size_t maxSamples, std::function<bool()> attachedMidRun,
                             std::function<std::string()> lastKnownScenario)
    : outDir_(std::move(outDir)),
      runLabel_(std::move(runLabel)),
      header_(std::move(header)),
      stateSchema_(std::move(stateSchema)),
      maxSamples_(maxSamples),
      attachedMidRun_(std::move(attachedMidRun)),
      lastKnownScenario_(std::move(lastKnownScenario)),
      presence_(stateSchema_) {}

void CaptureWriter::emit(const std::string& line) {
    if (failed_) {
        return;
    }
    file_ << line << '\n';
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

    const std::string slug = scenarioSlug(scenarioForName);
    if (runLabel_.empty()) {
        runLabel_ = nextRunLabel(outDir_, slug);
    }

    const std::filesystem::path target =
        std::filesystem::path(outDir_) / ("capture-" + slug + "-" + runLabel_ + ".n8rocap.jsonl");
    path_ = target.string();

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

    opened_ = true;
    emit(capture::writeHeader(header_));

    N8RO_LOG_INFO(std::string("capture opened: ") + path_ + " format " + capture::kFormatVersion +
                      " producer " + capture::kProducerVersion + " attached_mid_run=" +
                      (header_.attachedMidRun ? "true" : "false"),
                  kCategory);
    return !failed_;
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
            ++counts_.entitiesAdded;
            emit(capture::writeEntityAdd(record.simTimeS, segmentOrdinal_, record.subject,
                                         record.occupancy));
        } else if (record.kind == RecordKind::EntityRemove) {
            ++counts_.entitiesRemoved;
            emit(capture::writeEntityRemove(record.simTimeS, segmentOrdinal_, record.subject,
                                            record.occupancy, record.reason));
        }
        lastRecordSimTimeS_ = record.simTimeS;
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
                ++counts_.entitiesAdded;
                emit(capture::writeEntityAdd(record.simTimeS, segmentOrdinal_, record.subject,
                                             record.occupancy));
            } else {
                ++counts_.entitiesRemoved;
                emit(capture::writeEntityRemove(record.simTimeS, segmentOrdinal_, record.subject,
                                                record.occupancy, record.reason));
            }
            lastRecordSimTimeS_ = record.simTimeS;
            return;
        }

        case RecordKind::Sample: {
            if (maxSamples_ != 0 && counts_.samples >= maxSamples_) {
                // Past the budget. Not a drop - the budget is what ends recording, and
                // `end_reason: size_limit` says so. Counted for the log only, because it is
                // scheduler-dependent (the same reasoning M4 applied to this number).
                samplesAfterBudget_.fetch_add(1);
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
            presence_.note(record.values);
            ++counts_.samples;
            samplesWritten_.fetch_add(1);
            if (!haveFirstSample_) {
                firstSampleSimTimeS_ = record.simTimeS;
                haveFirstSample_ = true;
            }
            lastSampleSimTimeS_ = record.simTimeS;
            emit(capture::writeSample(record, segmentOrdinal_, stateSchema_));
            lastRecordSimTimeS_ = record.simTimeS;
            if (maxSamples_ != 0 && counts_.samples >= maxSamples_) {
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
    if (!ok) {
        N8RO_LOG_ERROR(std::string("capture file was opened but writing failed: ") + path_,
                       kCategory);
    }
    return ok;
}

}  // namespace n8ro::bridge
