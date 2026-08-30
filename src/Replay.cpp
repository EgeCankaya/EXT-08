#include "Replay.h"

#include "CaptureFormat.h"
#include "JsonParse.h"
#include "Referee.h"

#include <fstream>

namespace n8ro::bridge {
namespace {

[[nodiscard]] std::string atLine(std::uint64_t line, const std::string& why) {
    return "line " + std::to_string(line) + ": " + why;
}

}  // namespace

bool replay(const std::string& capturePath, const std::string& verdictPath,
            const std::vector<Condition>& conditions, ReplayResult& result,
            std::string& error) {
    std::ifstream file(capturePath, std::ios::binary);
    if (!file) {
        error = "could not open capture: " + capturePath;
        return false;
    }

    std::ofstream verdicts(verdictPath, std::ios::binary | std::ios::trunc);
    if (!verdicts) {
        error = "could not open verdict file for writing: " + verdictPath;
        return false;
    }
    result.verdictPath = verdictPath;

    Referee referee(conditions);

    bool segmentOpen = false;
    std::uint64_t segment = 0;
    double lastDataSimTimeS = 0.0;
    std::uint64_t lastDataSegment = 0;
    bool afterTrailer = false;

    auto writeAll = [&verdicts, &result](const std::vector<Verdict>& batch) {
        for (const Verdict& verdict : batch) {
            verdicts << writeVerdict(verdict) << '\n';
            ++result.verdictsEmitted;
            if (verdict.met) {
                ++result.met;
            }
        }
    };

    std::string line;
    while (std::getline(file, line)) {
        ++result.linesRead;
        // A capture is LF-terminated. Tolerate a stray CR so a file that has been through a
        // text-mode copy still replays, and say nothing - the conformance reader is what
        // judges the file, and this is a consumer.
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        if (afterTrailer) {
            error = atLine(result.linesRead, "a record after the trailer; the file is malformed");
            return false;
        }

        json::ValuePtr record;
        std::string parseError;
        if (!json::parse(line, record, parseError)) {
            error = atLine(result.linesRead, "not valid JSON - " + parseError);
            return false;
        }

        std::string type;
        if (!record->getString("type", type)) {
            error = atLine(result.linesRead, "record has no \"type\"");
            return false;
        }

        if (result.linesRead == 1) {
            if (type != "header") {
                error = "the first line is a \"" + type +
                        "\" record, not a header; this is not a capture";
                return false;
            }
            if (!record->getString("format_version", result.formatVersion)) {
                error = "the header carries no \"format_version\"";
                return false;
            }
            if (result.formatVersion != capture::kFormatVersion) {
                // Blunt on purpose (BTB-CAP-5). Naming both versions is what turns this from
                // a refusal into a diagnosis.
                error = "capture declares format_version \"" + result.formatVersion +
                        "\", and this reader implements \"" + capture::kFormatVersion +
                        "\" only. Refusing to parse it rather than guessing: a partial parse "
                        "of an unknown format is how silently-wrong analysis happens";
                return false;
            }
            continue;
        }

        if (type == "segment_open") {
            if (!record->getUint("segment", segment)) {
                error = atLine(result.linesRead, "segment_open has no integer \"segment\"");
                return false;
            }
            segmentOpen = true;
            ++result.segments;
            continue;
        }
        if (type == "segment_close") {
            segmentOpen = false;
            continue;
        }
        if (type == "verdict") {
            // The capture may already carry verdicts from the live run that produced it.
            // They are the answer to a possibly different condition file and are not this
            // run's business; counted so the summary can say they were there, then ignored.
            ++result.verdictsInInput;
            continue;
        }
        if (type == "trailer") {
            afterTrailer = true;
            result.sawTrailer = true;
            static_cast<void>(record->getString("end_reason", result.endReason));
            // End-of-run verdicts, anchored on the last data record - the same anchor the
            // live writer uses, which is what makes the two files identical (BTB-REF-4).
            writeAll(referee.finalVerdicts(lastDataSegment, lastDataSimTimeS));
            continue;
        }

        // The remaining three carry data and must sit inside a segment.
        if (type != "sample" && type != "entity_add" && type != "entity_remove") {
            // An unrecognised type inside a version-matched file is a producer defect
            // (section 3 of the format spec). Report rather than skip.
            error = atLine(result.linesRead, "record type \"" + type +
                                                 "\" is outside the closed vocabulary of "
                                                 "n8ro-capture/1");
            return false;
        }

        std::string entity;
        std::uint64_t occupancy = 0;
        double simTimeS = 0.0;
        if (!record->getString("entity", entity) || !record->getUint("occupancy", occupancy) ||
            !record->getNumber("sim_time_s", simTimeS)) {
            error = atLine(result.linesRead,
                           type + " needs \"entity\", an integer \"occupancy\" and "
                                  "\"sim_time_s\"");
            return false;
        }
        std::uint64_t recordSegment = 0;
        if (!record->getUint("segment", recordSegment)) {
            error = atLine(result.linesRead, type + " has no integer \"segment\"");
            return false;
        }
        if (!segmentOpen) {
            error = atLine(result.linesRead,
                           "a " + type + " record outside any open segment; the file is "
                                         "malformed (format spec section 7)");
            return false;
        }

        if (type == "sample") {
            const json::Value* fields = record->findOfKind("fields", json::Value::Kind::Object);
            if (fields == nullptr) {
                error = atLine(result.linesRead, "sample has no \"fields\" object");
                return false;
            }
            ++result.samples;
            const JsonFieldSource source(*fields);
            referee.onSample(entity, occupancy, simTimeS, recordSegment, source);
        } else if (type == "entity_add") {
            ++result.entityAdds;
            referee.onEntityAdd(entity, occupancy, simTimeS, recordSegment);
        } else {
            std::string reason;
            if (!record->getString("reason", reason)) {
                error = atLine(result.linesRead, "entity_remove has no \"reason\"");
                return false;
            }
            ++result.entityRemoves;
            referee.onEntityRemove(entity, occupancy, simTimeS, recordSegment, reason);
        }

        lastDataSimTimeS = simTimeS;
        lastDataSegment = recordSegment;
        writeAll(referee.drainVerdicts());
    }

    if (!result.sawTrailer) {
        // Everything before the truncation point is still valid and the verdicts derived from
        // it are real, so they are written - but the caller is told, because a truncated
        // capture means the run's tail is missing and a not-met verdict may only be not-met
        // because the evidence was lost with it.
        writeAll(referee.finalVerdicts(lastDataSegment, lastDataSimTimeS));
        verdicts.flush();
        error = "the capture has no trailer - it was truncated, so the producer was killed or "
                "the disk filled. Verdicts were derived from what is present, but a not-met "
                "verdict may be not-met only because the run's tail is missing";
        return false;
    }

    verdicts.flush();
    if (!verdicts) {
        error = "writing the verdict file failed: " + verdictPath;
        return false;
    }
    verdicts.close();
    return true;
}

}  // namespace n8ro::bridge
