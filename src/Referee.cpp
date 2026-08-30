#include "Referee.h"

#include "Json.h"

#include <utility>

namespace n8ro::bridge {
namespace {

// A value that decided a condition, rendered through the same round-trip-exact writer the
// capture's own doubles go through. A verdict must be as reproducible as the samples it was
// derived from, and locale-dependent formatting here would break that as surely as it would
// in a sample record (BTB-CAP-3, OQ-5).
[[nodiscard]] std::string renderNumber(double value) {
    std::string out;
    json::appendDouble(out, value);
    return out;
}

}  // namespace

bool StreamValueSource::tryString(const std::string& field, std::string& out) const {
    const auto it = values_.find(field);   // a lookup, never an iteration
    if (it == values_.end()) {
        return false;
    }
    const std::string* text = it->second.tryGet<std::string>();
    if (text == nullptr) {
        return false;
    }
    out = *text;
    return true;
}

bool StreamValueSource::tryGeodetic(const std::string& field, geo::Geodetic& out) const {
    const auto it = values_.find(field);
    if (it == values_.end()) {
        return false;
    }
    const std::vector<double>* vector = it->second.tryGet<std::vector<double>>();
    if (vector == nullptr || vector->size() < 3) {
        return false;
    }
    out = geo::Geodetic{(*vector)[0], (*vector)[1], (*vector)[2]};
    return true;
}

bool JsonFieldSource::tryString(const std::string& field, std::string& out) const {
    return fields_.getString(field, out);
}

bool JsonFieldSource::tryGeodetic(const std::string& field, geo::Geodetic& out) const {
    const json::Value* array = fields_.findOfKind(field, json::Value::Kind::Array);
    if (array == nullptr || array->elements.size() < 3) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        const json::Value& element = *array->elements[static_cast<std::size_t>(i)];
        // A double may be written without a fractional part - `400`, not `400.0` - so a
        // reader that types values from the JSON token instead of from the schema silently
        // gets an integer for every round altitude in the file. Section 8.3 of the format
        // spec says so in bold, and this is what heeding it looks like: the token's shape is
        // not consulted, only that it is a number.
        if (element.kind != json::Value::Kind::Number) {
            return false;
        }
        out[static_cast<std::size_t>(i)] = element.number;
    }
    return true;
}

Referee::Referee(std::vector<Condition> conditions) : conditions_(std::move(conditions)) {
    decided_.assign(conditions_.size(), false);
}

void Referee::emit(Verdict verdict, std::size_t index) {
    decided_[index] = true;
    ++emitted_;
    pending_.push_back(std::move(verdict));
}

void Referee::evaluateProximity(const Condition& condition, std::size_t index, double simTimeS,
                                std::uint64_t segment) {
    const auto a = live_.find(condition.entityA);
    const auto b = live_.find(condition.entityB);
    if (a == live_.end() || b == live_.end() || !a->second.hasPosition ||
        !b->second.hasPosition) {
        // Both have to have published a position for the question to have an answer. Not a
        // failure - just not yet decidable.
        return;
    }

    const double distance = geo::distanceM(a->second.position, b->second.position);
    if (distance > condition.withinM) {
        return;
    }

    Verdict verdict;
    verdict.conditionId = condition.id;
    verdict.met = true;
    verdict.segment = segment;
    verdict.simTimeS = simTimeS;
    verdict.entities = {condition.entityA, condition.entityB};
    verdict.numberValues["distance_m"] = renderNumber(distance);
    verdict.numberValues["within_m"] = renderNumber(condition.withinM);
    // Enough to locate the causing samples in the capture: the two entities, their
    // occupancies, the simulation times of the samples that were current, and the distance
    // (BTB-REF-2 - "every verdict is reproducible from the capture alone").
    verdict.numberValues["occupancy_a"] = std::to_string(a->second.occupancy);
    verdict.numberValues["occupancy_b"] = std::to_string(b->second.occupancy);
    verdict.numberValues["sample_sim_time_a_s"] = renderNumber(a->second.simTimeS);
    verdict.numberValues["sample_sim_time_b_s"] = renderNumber(b->second.simTimeS);
    emit(std::move(verdict), index);
}

void Referee::evaluateArea(const Condition& condition, std::size_t index,
                           const std::string& entity, double simTimeS, std::uint64_t segment,
                           const geo::Geodetic& position) {
    const bool inside = condition.region.shape == RegionShape::Circle
                            ? geo::insideCircle(position, condition.region.centre,
                                                condition.region.radiusM)
                            : geo::insidePolygon(position, condition.region.polygon);
    const bool satisfied = condition.test == AreaTest::Inside ? inside : !inside;
    if (!satisfied) {
        return;
    }

    Verdict verdict;
    verdict.conditionId = condition.id;
    verdict.met = true;
    verdict.segment = segment;
    verdict.simTimeS = simTimeS;
    verdict.entities = {entity};
    verdict.stringValues["test"] = condition.test == AreaTest::Inside ? "inside" : "outside";
    verdict.stringValues["shape"] =
        condition.region.shape == RegionShape::Circle ? "circle" : "polygon";
    verdict.numberValues["latitude_deg"] = renderNumber(position[0]);
    verdict.numberValues["longitude_deg"] = renderNumber(position[1]);
    verdict.numberValues["altitude_m"] = renderNumber(position[2]);
    if (condition.region.shape == RegionShape::Circle) {
        verdict.numberValues["distance_from_centre_m"] =
            renderNumber(geo::distanceM(geo::Geodetic{position[0], position[1],
                                                      condition.region.centre[2]},
                                        condition.region.centre));
        verdict.numberValues["radius_m"] = renderNumber(condition.region.radiusM);
    }
    emit(std::move(verdict), index);
}

void Referee::onEntityAdd(const std::string& entity, std::uint64_t occupancy, double simTimeS,
                          std::uint64_t) {
    // A new tenure starts with no known position. Without this a proximity condition could be
    // decided against the previous occupant's last place under a re-used name, which ADR-6
    // exists to make impossible.
    Known& known = live_[entity];
    known.occupancy = occupancy;
    known.simTimeS = simTimeS;
    known.hasPosition = false;
    known.position = geo::Geodetic{};
}

void Referee::onSample(const std::string& entity, std::uint64_t occupancy, double simTimeS,
                       std::uint64_t segment, const FieldSource& fields) {
    Known& known = live_[entity];
    known.occupancy = occupancy;
    known.simTimeS = simTimeS;
    geo::Geodetic position{};
    const bool hasPosition = fields.tryGeodetic(kPositionField, position);
    if (hasPosition) {
        known.hasPosition = true;
        known.position = position;
    }

    for (std::size_t i = 0; i < conditions_.size(); ++i) {
        if (decided_[i]) {
            continue;
        }
        const Condition& condition = conditions_[i];
        switch (condition.kind) {
            case ConditionKind::Proximity:
                if (entity == condition.entityA || entity == condition.entityB) {
                    evaluateProximity(condition, i, simTimeS, segment);
                }
                break;

            case ConditionKind::Area:
                if (entity == condition.entity && hasPosition) {
                    evaluateArea(condition, i, entity, simTimeS, segment, position);
                }
                break;

            case ConditionKind::TerminalState: {
                if (entity != condition.entity || condition.fieldName.empty()) {
                    break;
                }
                std::string value;
                if (!fields.tryString(condition.fieldName, value) ||
                    value != condition.fieldEquals) {
                    break;
                }
                Verdict verdict;
                verdict.conditionId = condition.id;
                verdict.met = true;
                verdict.segment = segment;
                verdict.simTimeS = simTimeS;
                verdict.entities = {entity};
                verdict.stringValues["field"] = condition.fieldName;
                verdict.stringValues["value"] = value;
                verdict.numberValues["occupancy"] = std::to_string(occupancy);
                emit(std::move(verdict), i);
                break;
            }
        }
    }
}

void Referee::onEntityRemove(const std::string& entity, std::uint64_t occupancy, double simTimeS,
                             std::uint64_t segment, const std::string& reason) {
    for (std::size_t i = 0; i < conditions_.size(); ++i) {
        if (decided_[i]) {
            continue;
        }
        const Condition& condition = conditions_[i];
        if (condition.kind != ConditionKind::TerminalState || condition.removalReason.empty() ||
            condition.entity != entity) {
            continue;
        }
        // Verbatim comparison against whatever the platform sent. The removal-reason
        // vocabulary is deliberately open (format spec section 9), so a condition may name a
        // supplier-specific reason this build has never heard of and it will still match.
        if (reason != condition.removalReason) {
            continue;
        }
        Verdict verdict;
        verdict.conditionId = condition.id;
        verdict.met = true;
        verdict.segment = segment;
        verdict.simTimeS = simTimeS;
        verdict.entities = {entity};
        verdict.stringValues["removal_reason"] = reason;
        verdict.numberValues["occupancy"] = std::to_string(occupancy);
        emit(std::move(verdict), i);
    }

    // The body is gone; its position is no longer a fact about anything live.
    const auto known = live_.find(entity);
    if (known != live_.end()) {
        known->second.hasPosition = false;
    }
}

std::vector<Verdict> Referee::drainVerdicts() {
    std::vector<Verdict> out;
    out.swap(pending_);
    return out;
}

std::vector<Verdict> Referee::finalVerdicts(std::uint64_t segment, double simTimeS) {
    std::vector<Verdict> out;
    for (std::size_t i = 0; i < conditions_.size(); ++i) {
        if (decided_[i]) {
            continue;
        }
        // Explicit, never silence. A condition that was never met and says nothing is
        // indistinguishable from one nobody evaluated (BTB-REF-2).
        const Condition& condition = conditions_[i];
        Verdict verdict;
        verdict.conditionId = condition.id;
        verdict.met = false;
        verdict.segment = segment;
        verdict.simTimeS = simTimeS;
        if (condition.kind == ConditionKind::Proximity) {
            verdict.entities = {condition.entityA, condition.entityB};
            verdict.numberValues["within_m"] = renderNumber(condition.withinM);
        } else {
            verdict.entities = {condition.entity};
        }
        verdict.stringValues["kind"] = conditionKindName(condition.kind);
        decided_[i] = true;
        ++emitted_;
        out.push_back(std::move(verdict));
    }
    return out;
}

std::string writeVerdict(const Verdict& verdict) {
    std::string out;
    out.push_back('{');
    out += json::quoted("type");
    out.push_back(':');
    out += json::quoted("verdict");

    out.push_back(',');
    out += json::quoted("sim_time_s");
    out.push_back(':');
    json::appendDouble(out, verdict.simTimeS);

    out.push_back(',');
    out += json::quoted("segment");
    out.push_back(':');
    json::appendInt(out, static_cast<std::int64_t>(verdict.segment));

    out.push_back(',');
    out += json::quoted("condition_id");
    out.push_back(':');
    out += json::quoted(verdict.conditionId);

    out.push_back(',');
    out += json::quoted("met");
    out.push_back(':');
    json::appendBool(out, verdict.met);

    out.push_back(',');
    out += json::quoted("entities");
    out.push_back(':');
    out.push_back('[');
    for (std::size_t i = 0; i < verdict.entities.size(); ++i) {
        if (i != 0) {
            out.push_back(',');
        }
        out += json::quoted(verdict.entities[i]);
    }
    out.push_back(']');

    out.push_back(',');
    out += json::quoted("values");
    out.push_back(':');
    out.push_back('{');
    bool first = true;
    // Both maps are ordered, and strings are written after numbers, so a verdict's bytes are
    // a function of its content and nothing else.
    for (const auto& entry : verdict.numberValues) {
        if (!first) {
            out.push_back(',');
        }
        first = false;
        out += json::quoted(entry.first);
        out.push_back(':');
        out += entry.second;   // already rendered through the round-trip-exact writer
    }
    for (const auto& entry : verdict.stringValues) {
        if (!first) {
            out.push_back(',');
        }
        first = false;
        out += json::quoted(entry.first);
        out.push_back(':');
        out += json::quoted(entry.second);
    }
    out.push_back('}');

    out.push_back('}');
    return out;
}

}  // namespace n8ro::bridge
