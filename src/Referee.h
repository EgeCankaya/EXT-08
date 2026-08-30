// EXT-08 Bus Telemetry Bridge - M6: the referee (BTB-REF-2, BTB-REF-3, BTB-REF-4, ADR-5).
//
// One evaluation engine, two sources. Live, it is driven by the same record stream the
// writer thread is already draining; offline, by a stored capture read back with no
// simulator, no bus and no client. **Verdicts from the two paths must be identical**, and
// that is not merely a nice property: it is the strongest available conformance test for the
// capture format. If the referee can re-derive its own verdicts from the file alone, the file
// demonstrably contains enough (ADR-5, BTB-REF-4).
//
// Single-sourcing the evaluation is what makes that true rather than hoped for, so the two
// sources differ only in how a field is read out of a record - `FieldSource` below - and
// share every line of the deciding logic.
//
// **This is the one place in EXT-08 with an abstract interface**, and it is deliberate.
// ADR-1 declined one for the entity picture on the grounds that a pure-virtual seam buys
// substitutability we have no second implementation for. Here there genuinely are two: a
// decoded `StreamValueMap` off the bus, and a parsed `sample.fields` object out of a file.
//
// Threading: the referee runs on the writer thread in live mode and on the main thread in
// replay. Never in a handler (CLAUDE.md hard rule 2).
//
// ---------------------------------------------------------------------------------------
// Verdict semantics, because "when is a condition met" needs saying once, precisely
//
// A condition produces **exactly one verdict per run**:
//
//   met      at the first moment it is satisfied, carrying the simulation time and the values
//            that decided it. Later satisfactions are not re-emitted - "did the two aircraft
//            come within 5 km" is answered by the first time they did, and re-emitting would
//            flood the capture with thousands of records saying the same thing.
//   not met  at end of run, explicitly. BTB-REF-2 requires this: silence is not an answer,
//            because silence is also what a condition nobody evaluated looks like.

#pragma once

#include "Conditions.h"
#include "Geodesy.h"
#include "JsonParse.h"

#include <messaging/packed/MessageBusPacked.h>
#include <messaging/packed/StreamValue.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace n8ro::bridge {

// How the referee reads one field out of one sample, without knowing where the sample came
// from. Both implementations read *presence*, never assuming a schema-declared field arrived
// - the entity-state schema declares twelve fields and only eleven are ever published.
class FieldSource {
public:
    virtual ~FieldSource() = default;
    [[nodiscard]] virtual bool tryString(const std::string& field, std::string& out) const = 0;
    [[nodiscard]] virtual bool tryGeodetic(const std::string& field,
                                           geo::Geodetic& out) const = 0;
};

// Live: a decoded StreamValueMap straight off the bus.
class StreamValueSource final : public FieldSource {
public:
    explicit StreamValueSource(const n8ro::sim::StreamValueMap& values) : values_(values) {}
    [[nodiscard]] bool tryString(const std::string& field, std::string& out) const override;
    [[nodiscard]] bool tryGeodetic(const std::string& field, geo::Geodetic& out) const override;

private:
    const n8ro::sim::StreamValueMap& values_;
};

// Replay: a parsed `sample.fields` object out of a stored capture.
class JsonFieldSource final : public FieldSource {
public:
    explicit JsonFieldSource(const json::Value& fields) : fields_(fields) {}
    [[nodiscard]] bool tryString(const std::string& field, std::string& out) const override;
    [[nodiscard]] bool tryGeodetic(const std::string& field, geo::Geodetic& out) const override;

private:
    const json::Value& fields_;
};

// One decided condition, ready to be written as a `verdict` record.
struct Verdict {
    std::string conditionId;
    bool met = false;
    std::uint64_t segment = 0;
    double simTimeS = 0.0;
    std::vector<std::string> entities;
    // Ordered, so the record's bytes do not depend on a hash table (BTB-CAP-3). Values are
    // pre-rendered: a double goes through the same round-trip-exact writer everything else
    // does, so a verdict's numbers are as reproducible as a sample's.
    std::map<std::string, std::string> numberValues;
    std::map<std::string, std::string> stringValues;
};

class Referee {
public:
    // The field the platform publishes a geodetic position under. Named once here rather
    // than spelled at each use; it is a *field* name from the schema in the capture header,
    // not a topic string, so hard rule 3 does not apply - but a reader of a condition file
    // should still be able to find it.
    static constexpr const char* kPositionField = "positionGeodetic";

    explicit Referee(std::vector<Condition> conditions);

    // Fed in record order, from either source. `segment` is the enclosing segment, carried
    // into any verdict so a reader can locate the causing samples.
    void onSample(const std::string& entity, std::uint64_t occupancy, double simTimeS,
                  std::uint64_t segment, const FieldSource& fields);
    void onEntityRemove(const std::string& entity, std::uint64_t occupancy, double simTimeS,
                        std::uint64_t segment, const std::string& reason);

    // A new occupancy invalidates the previous tenure's position, so a proximity condition
    // can never be decided against a dead entity's last known place under a re-used name
    // (ADR-6). Called for every entity_add.
    void onEntityAdd(const std::string& entity, std::uint64_t occupancy, double simTimeS,
                     std::uint64_t segment);

    // Verdicts decided so far, in declaration order, drained for writing. A verdict appears
    // here at the moment its condition is first met.
    [[nodiscard]] std::vector<Verdict> drainVerdicts();

    // At end of run: an explicit not-met verdict for every condition never satisfied
    // (BTB-REF-2). `segment` and `simTimeS` are the last ones the run reached.
    [[nodiscard]] std::vector<Verdict> finalVerdicts(std::uint64_t segment, double simTimeS);

    [[nodiscard]] std::size_t conditionCount() const { return conditions_.size(); }
    [[nodiscard]] std::uint64_t verdictsEmitted() const { return emitted_; }

private:
    struct Known {
        std::uint64_t occupancy = 0;
        double simTimeS = 0.0;
        bool hasPosition = false;
        geo::Geodetic position{};
    };

    void evaluateProximity(const Condition& condition, std::size_t index, double simTimeS,
                           std::uint64_t segment);
    void evaluateArea(const Condition& condition, std::size_t index, const std::string& entity,
                      double simTimeS, std::uint64_t segment, const geo::Geodetic& position);
    void emit(Verdict verdict, std::size_t index);

    std::vector<Condition> conditions_;
    std::vector<bool> decided_;              // one per condition, in declaration order
    std::map<std::string, Known> live_;      // ordered - nothing unordered on this path
    std::vector<Verdict> pending_;
    std::uint64_t emitted_ = 0;
};

// One `verdict` capture record. Its shape is section 10 of the format specification.
[[nodiscard]] std::string writeVerdict(const Verdict& verdict);

}  // namespace n8ro::bridge
