// EXT-08 - unit tests for the referee and the condition loader (BTB-REF-1, BTB-REF-2,
// BTB-REF-3).
//
// Needs no simulator, no bus and no model database. It drives the referee through the same
// FieldSource seam the live and replay paths use, so it exercises the real entry points
// rather than a stand-in - the same discipline tests/entity-picture/ follows.
//
// The PRD's test plan asks for "each of the three condition kinds against synthetic sample
// sequences, including the boundary case (exactly at the threshold) and the never-met case".
// All three are here, plus the two things that are easy to get wrong and impossible to see
// from the outside: that a re-created name does not inherit the previous tenure's position
// (ADR-6), and that the condition loader rejects a fourth kind by name rather than skipping
// it.
//
// Build (from a shell that has run C:\N8RO\setup.cmd and dev\setup-dev.cmd):
//
//   cl /std:c++17 /EHsc /W4 /O2 ^
//      /I %N8RO_RELEASE%\include\n8ro-core /I %N8RO_RELEASE%\include\n8ro-sim ^
//      /Fe:referee_test.exe ^
//      tests\referee\referee_test.cpp src\Referee.cpp src\Conditions.cpp ^
//      src\Geodesy.cpp src\JsonParse.cpp src\Json.cpp

#include "../../src/Conditions.h"
#include "../../src/Geodesy.h"
#include "../../src/Referee.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

int gChecks = 0;
int gFailures = 0;

void check(bool condition, const std::string& what) {
    ++gChecks;
    if (!condition) {
        ++gFailures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

void section(const std::string& name) { std::printf("\n- %s\n", name.c_str()); }

// A FieldSource over a plain map, so a test can hand the referee any sample it likes without
// building a StreamValueMap or a JSON document.
class TestSource final : public n8ro::bridge::FieldSource {
public:
    TestSource& withPosition(double lat, double lon, double alt) {
        hasPosition_ = true;
        position_ = n8ro::bridge::geo::Geodetic{lat, lon, alt};
        return *this;
    }
    TestSource& with(const std::string& field, const std::string& value) {
        strings_[field] = value;
        return *this;
    }

    [[nodiscard]] bool tryString(const std::string& field, std::string& out) const override {
        const auto it = strings_.find(field);
        if (it == strings_.end()) {
            return false;
        }
        out = it->second;
        return true;
    }
    [[nodiscard]] bool tryGeodetic(const std::string& field,
                                   n8ro::bridge::geo::Geodetic& out) const override {
        if (!hasPosition_ || field != n8ro::bridge::Referee::kPositionField) {
            return false;
        }
        out = position_;
        return true;
    }

private:
    bool hasPosition_ = false;
    n8ro::bridge::geo::Geodetic position_{};
    std::map<std::string, std::string> strings_;
};

using namespace n8ro::bridge;

[[nodiscard]] std::vector<Condition> load(const std::string& text) {
    std::vector<Condition> conditions;
    std::string error;
    if (!parseConditions(text, conditions, error)) {
        std::printf("  (loader rejected: %s)\n", error.c_str());
        return {};
    }
    return conditions;
}

[[nodiscard]] bool rejects(const std::string& text, std::string& error) {
    std::vector<Condition> conditions;
    return !parseConditions(text, conditions, error);
}

// ---------------------------------------------------------------------------------------

void testGeodesy() {
    section("the geodetic method is the one the README documents, and it is reproducible");

    // A degree of latitude at the equator is about 110.57 km on WGS-84. Checking against a
    // published figure rather than against our own output is what makes this a test.
    const double oneDegreeLat =
        geo::distanceM(geo::Geodetic{0.0, 0.0, 0.0}, geo::Geodetic{1.0, 0.0, 0.0});
    check(std::fabs(oneDegreeLat - 110574.0) < 400.0,
          "one degree of latitude at the equator is ~110.57 km, got " +
              std::to_string(oneDegreeLat));

    // Altitude is part of the distance. Two points at the same lat/lon 1000 m apart
    // vertically are 1000 m apart - which is the whole reason ECEF was chosen over haversine.
    const double vertical =
        geo::distanceM(geo::Geodetic{-23.5, -68.25, 0.0}, geo::Geodetic{-23.5, -68.25, 1000.0});
    check(std::fabs(vertical - 1000.0) < 0.001,
          "1000 m of altitude is 1000 m of distance, got " + std::to_string(vertical));

    check(geo::distanceM(geo::Geodetic{-23.5, -68.25, 400.0},
                         geo::Geodetic{-23.5, -68.25, 400.0}) == 0.0,
          "a point is zero metres from itself");

    section("region containment, including the boundary");

    const geo::Geodetic centre{0.0, 0.0, 0.0};
    // Walk out along a meridian until we are exactly at the radius, then test either side.
    const double metresPerDegree = oneDegreeLat;
    const double radius = 1000.0;
    const geo::Geodetic justInside{radius * 0.999 / metresPerDegree, 0.0, 0.0};
    const geo::Geodetic justOutside{radius * 1.001 / metresPerDegree, 0.0, 0.0};
    check(geo::insideCircle(justInside, centre, radius), "a point inside a circle is inside");
    check(!geo::insideCircle(justOutside, centre, radius),
          "a point outside a circle is outside");
    check(geo::insideCircle(centre, centre, radius), "the centre is inside its own circle");
    // The documented boundary rule: on the boundary is INSIDE.
    check(geo::insideCircle(centre, centre, 0.0),
          "with radius zero the centre is still inside - the comparison is <=, as documented");

    const std::vector<geo::Geodetic> square{{0.0, 0.0, 0.0},
                                            {0.0, 1.0, 0.0},
                                            {1.0, 1.0, 0.0},
                                            {1.0, 0.0, 0.0}};
    check(geo::insidePolygon(geo::Geodetic{0.5, 0.5, 0.0}, square),
          "a point in the middle of a polygon is inside");
    check(!geo::insidePolygon(geo::Geodetic{1.5, 0.5, 0.0}, square),
          "a point beyond a polygon is outside");
    check(geo::insidePolygon(geo::Geodetic{0.0, 0.5, 0.0}, square),
          "a point exactly on an edge is INSIDE, as documented");
    check(geo::insidePolygon(geo::Geodetic{0.0, 0.0, 0.0}, square),
          "a point exactly on a vertex is INSIDE, as documented");
    check(!geo::insidePolygon(geo::Geodetic{0.5, 0.5, 0.0},
                              std::vector<geo::Geodetic>{{0.0, 0.0, 0.0}, {1.0, 1.0, 0.0}}),
          "a two-vertex 'polygon' contains nothing");
}

void testProximity() {
    section("proximity, and the boundary case exactly at the threshold");

    // The threshold is set to the exact distance the two positions produce, so this tests the
    // <= semantics rather than approximately testing it.
    //
    // Worth stating why it is done that way: a geodetic distance is a computed double, so a
    // caller cannot reach the boundary by choosing a round threshold. Two points a nominal
    // 1000 m apart come out a fraction of a millimetre off 1000, and `within_m: 1000` does not
    // match them. The documented `<=` matters for *reproducibility* - the same input always
    // gives the same answer - not because anyone will land on it deliberately.
    const geo::Geodetic low{-23.5, -68.25, 0.0};
    const geo::Geodetic high{-23.5, -68.25, 1000.0};
    const double exact = geo::distanceM(low, high);
    check(exact != 1000.0,
          "a nominal 1000 m of altitude is not exactly 1000.0 m of computed distance, which is "
          "why the threshold below is the computed value rather than a round number");

    std::string thresholdText;
    {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.17g", exact);
        thresholdText = buffer;
    }
    Referee referee(load(R"({"conditions":[
        {"id":"pair","kind":"proximity","entities":["A","B"],"within_m":)" + thresholdText +
                         R"(}]})"));
    check(referee.conditionCount() == 1, "one condition loaded");

    referee.onEntityAdd("A", 1, 0.0, 0);
    referee.onEntityAdd("B", 1, 0.0, 0);
    referee.onSample("A", 1, 1.0, 0, TestSource().withPosition(-23.5, -68.25, 0.0));
    referee.onSample("B", 1, 1.0, 0, TestSource().withPosition(-23.5, -68.25, 2000.0));
    check(referee.drainVerdicts().empty(), "2000 m apart does not meet the threshold");

    referee.onSample("B", 1, 2.0, 0, TestSource().withPosition(-23.5, -68.25, 1000.0));
    std::vector<Verdict> verdicts = referee.drainVerdicts();
    check(verdicts.size() == 1, "exactly at the threshold is met - the comparison is <=");
    if (verdicts.size() == 1) {
        check(verdicts[0].met, "and the verdict says met");
        check(verdicts[0].conditionId == "pair", "carrying the condition id");
        check(verdicts[0].simTimeS == 2.0, "stamped with the simulation time it was decided");
        check(verdicts[0].entities.size() == 2, "naming both entities");
        // BTB-REF-2: enough to locate the causing samples.
        check(verdicts[0].numberValues.count("distance_m") == 1, "carrying the distance");
        check(verdicts[0].numberValues.count("sample_sim_time_a_s") == 1,
              "carrying each sample's own time, so the causing records can be found");
        check(verdicts[0].numberValues.count("occupancy_a") == 1,
              "carrying each occupancy, so the right tenure's records can be found");
    }

    referee.onSample("B", 1, 3.0, 0, TestSource().withPosition(-23.5, -68.25, 1000.0));
    check(referee.drainVerdicts().empty(),
          "a condition already met is not re-emitted on every later sample");

    check(referee.finalVerdicts(0, 9.0).empty(),
          "a condition already met produces no end-of-run verdict");
}

void testProximityNeedsBoth() {
    section("proximity is undecidable until both entities have published a position");

    Referee referee(load(R"({"conditions":[
        {"id":"pair","kind":"proximity","entities":["A","B"],"within_m":100000}]})"));
    referee.onEntityAdd("A", 1, 0.0, 0);
    referee.onSample("A", 1, 1.0, 0, TestSource().withPosition(0.0, 0.0, 0.0));
    check(referee.drainVerdicts().empty(),
          "one entity alone decides nothing, however generous the threshold");

    referee.onEntityAdd("B", 1, 0.0, 0);
    referee.onSample("B", 1, 2.0, 0, TestSource().withPosition(0.0, 0.0, 0.0));
    check(referee.drainVerdicts().size() == 1, "the second entity's first sample decides it");
}

void testOccupancyResetsPosition() {
    section("ADR-6: a re-created name does not inherit the previous tenure's position");

    // The trap this guards: A is destroyed next to B, then re-created far away. Without the
    // reset the referee still holds A's death position, and a proximity condition declared
    // afterwards would be met against a body that is no longer there.
    Referee referee(load(R"({"conditions":[
        {"id":"pair","kind":"proximity","entities":["A","B"],"within_m":10}]})"));

    referee.onEntityAdd("A", 1, 0.0, 0);
    referee.onEntityAdd("B", 1, 0.0, 0);
    referee.onSample("B", 1, 1.0, 0, TestSource().withPosition(0.0, 0.0, 0.0));
    referee.onSample("A", 1, 1.0, 0, TestSource().withPosition(0.0, 0.0, 1000.0));
    check(referee.drainVerdicts().empty(), "1000 m apart does not meet a 10 m threshold");

    referee.onEntityRemove("A", 1, 2.0, 0, "destroyed");
    // A returns under the same name at occupancy 2, and the roster says so before any sample.
    referee.onEntityAdd("A", 2, 3.0, 0);
    check(referee.drainVerdicts().empty(), "a re-creation on its own decides nothing");

    // B moves to where A's *first* tenure died. If the referee had kept the stale position
    // this would fire, and it would be wrong.
    referee.onSample("B", 1, 4.0, 0, TestSource().withPosition(0.0, 0.0, 1000.0));
    check(referee.drainVerdicts().empty(),
          "the dead tenure's position is not a fact about the live one");

    // Once the new tenure actually publishes, it decides normally.
    referee.onSample("A", 2, 5.0, 0, TestSource().withPosition(0.0, 0.0, 1000.0));
    const std::vector<Verdict> verdicts = referee.drainVerdicts();
    check(verdicts.size() == 1, "the new tenure's own sample decides it");
    if (verdicts.size() == 1) {
        check(verdicts[0].numberValues.at("occupancy_a") == "2",
              "and the verdict names occupancy 2, not 1");
    }
}

void testArea() {
    section("area, inside and outside, circle and polygon");

    Referee circle(load(R"({"conditions":[
        {"id":"in","kind":"area","entity":"A","test":"inside",
         "region":{"shape":"circle","centre":[0,0,0],"radius_m":1000}}]})"));
    circle.onEntityAdd("A", 1, 0.0, 0);
    circle.onSample("A", 1, 1.0, 0, TestSource().withPosition(1.0, 0.0, 0.0));
    check(circle.drainVerdicts().empty(), "a degree away is not within a kilometre");
    circle.onSample("A", 1, 2.0, 0, TestSource().withPosition(0.0, 0.0, 0.0));
    const std::vector<Verdict> hit = circle.drainVerdicts();
    check(hit.size() == 1, "arriving at the centre meets an inside-circle condition");
    if (hit.size() == 1) {
        check(hit[0].stringValues.at("shape") == "circle", "the verdict names the shape");
        check(hit[0].numberValues.count("distance_from_centre_m") == 1,
              "and how far from the centre it was, so the result can be checked by hand");
    }

    Referee outside(load(R"({"conditions":[
        {"id":"out","kind":"area","entity":"A","test":"outside",
         "region":{"shape":"circle","centre":[0,0,0],"radius_m":1000}}]})"));
    outside.onEntityAdd("A", 1, 0.0, 0);
    outside.onSample("A", 1, 1.0, 0, TestSource().withPosition(0.0, 0.0, 0.0));
    check(outside.drainVerdicts().empty(), "at the centre does not meet an outside condition");
    outside.onSample("A", 1, 2.0, 0, TestSource().withPosition(1.0, 0.0, 0.0));
    check(outside.drainVerdicts().size() == 1, "leaving the circle meets it");

    Referee polygon(load(R"({"conditions":[
        {"id":"poly","kind":"area","entity":"A","test":"inside",
         "region":{"shape":"polygon","vertices":[[0,0],[0,1],[1,1],[1,0]]}}]})"));
    polygon.onEntityAdd("A", 1, 0.0, 0);
    polygon.onSample("A", 1, 1.0, 0, TestSource().withPosition(2.0, 2.0, 0.0));
    check(polygon.drainVerdicts().empty(), "outside the polygon decides nothing");
    polygon.onSample("A", 1, 2.0, 0, TestSource().withPosition(0.5, 0.5, 0.0));
    check(polygon.drainVerdicts().size() == 1, "entering the polygon meets it");

    section("a sample carrying no position cannot decide an area condition");
    Referee noPosition(load(R"({"conditions":[
        {"id":"in","kind":"area","entity":"A","test":"outside",
         "region":{"shape":"circle","centre":[0,0,0],"radius_m":1}}]})"));
    noPosition.onEntityAdd("A", 1, 0.0, 0);
    // An "outside" test would be trivially true for a missing position if presence were
    // assumed rather than read - which is the schema-declared-but-never-published trap.
    noPosition.onSample("A", 1, 1.0, 0, TestSource().with("phase", "operational"));
    check(noPosition.drainVerdicts().empty(),
          "an absent position is absent, not a position outside the region");
}

void testTerminalState() {
    section("terminal state, by removal reason and by field value");

    Referee byReason(load(R"({"conditions":[
        {"id":"dead","kind":"terminal_state","entity":"A","removal_reason":"destroyed"}]})"));
    byReason.onEntityAdd("A", 1, 0.0, 0);
    byReason.onEntityRemove("B", 1, 1.0, 0, "destroyed");
    check(byReason.drainVerdicts().empty(), "another entity's removal is not this one's");
    byReason.onEntityRemove("A", 1, 2.0, 0, "expended");
    check(byReason.drainVerdicts().empty(), "the wrong reason does not match");
    byReason.onEntityRemove("A", 2, 3.0, 0, "destroyed");
    const std::vector<Verdict> dead = byReason.drainVerdicts();
    check(dead.size() == 1, "the declared reason matches");
    if (dead.size() == 1) {
        check(dead[0].stringValues.at("removal_reason") == "destroyed",
              "and the verdict carries the reason verbatim");
    }

    section("a supplier-specific reason the engine's own set does not contain still matches");
    Referee vendor(load(R"({"conditions":[
        {"id":"jam","kind":"terminal_state","entity":"A",
         "removal_reason":"acme_jammer_overloaded"}]})"));
    vendor.onEntityAdd("A", 1, 0.0, 0);
    vendor.onEntityRemove("A", 1, 1.0, 0, "acme_jammer_overloaded");
    check(vendor.drainVerdicts().size() == 1,
          "the removal-reason vocabulary is open, and the referee compares verbatim");

    section("terminal state by a declared field value");
    Referee byField(load(R"({"conditions":[
        {"id":"phase","kind":"terminal_state","entity":"A","field":"phase",
         "equals":"destroyed"}]})"));
    byField.onEntityAdd("A", 1, 0.0, 0);
    byField.onSample("A", 1, 1.0, 0, TestSource().with("phase", "operational"));
    check(byField.drainVerdicts().empty(), "a different value does not match");
    byField.onSample("A", 1, 2.0, 0, TestSource().with("phase", "destroyed"));
    check(byField.drainVerdicts().size() == 1, "the declared value matches");
}

void testNeverMet() {
    section("BTB-REF-2: a condition never met produces an explicit not-met verdict");

    Referee referee(load(R"({"conditions":[
        {"id":"a","kind":"proximity","entities":["X","Y"],"within_m":1},
        {"id":"b","kind":"area","entity":"X","test":"inside",
         "region":{"shape":"circle","centre":[0,0,0],"radius_m":1}},
        {"id":"c","kind":"terminal_state","entity":"X","removal_reason":"destroyed"}]})"));
    check(referee.drainVerdicts().empty(), "nothing decided while nothing has happened");

    const std::vector<Verdict> finals = referee.finalVerdicts(3, 42.5);
    check(finals.size() == 3, "every undecided condition produces a verdict at end of run");
    for (const Verdict& verdict : finals) {
        check(!verdict.met, "and every one of them says not met, rather than saying nothing");
        check(verdict.simTimeS == 42.5, "stamped with the run's last simulation time");
        check(verdict.segment == 3, "and the segment it ended in");
        check(verdict.stringValues.count("kind") == 1,
              "naming the kind, so a not-met verdict is still traceable to its declaration");
    }
    check(referee.finalVerdicts(3, 42.5).empty(),
          "and they are produced exactly once, not on every call");
}

void testVerdictRecordShape() {
    section("a verdict record is stable, ordered, and shaped as the format specifies");

    Verdict verdict;
    verdict.conditionId = "id\"with\"quotes";
    verdict.met = true;
    verdict.segment = 2;
    verdict.simTimeS = 0.05;
    verdict.entities = {"B", "A"};
    verdict.numberValues["zeta"] = "1";
    verdict.numberValues["alpha"] = "2";
    verdict.stringValues["kind"] = "proximity";

    const std::string line = writeVerdict(verdict);
    check(line.rfind("{\"type\":\"verdict\",\"sim_time_s\":0.05,\"segment\":2,", 0) == 0,
          "the envelope opens with type, sim_time_s and segment, in that order");
    check(line.find("\"condition_id\":\"id\\\"with\\\"quotes\"") != std::string::npos,
          "the condition id is escaped rather than breaking the line");
    check(line.find("\"entities\":[\"B\",\"A\"]") != std::string::npos,
          "entities keep the order the referee named them in, not a sorted order");
    check(line.find("\"values\":{\"alpha\":2,\"zeta\":1,\"kind\":\"proximity\"}") !=
              std::string::npos,
          "values are emitted from ordered maps, so the bytes do not depend on a hash table");
    check(line.find('\n') == std::string::npos, "a record is one line and carries no newline");

    check(writeVerdict(verdict) == line, "the same verdict always produces the same bytes");
}

void testLoaderRejections() {
    section("BTB-REF-1/REF-3: the loader names what is wrong rather than skipping it");

    std::string error;
    check(rejects("not json at all", error), "a file that is not JSON is rejected");
    check(error.find("not valid JSON") != std::string::npos, "and says so");

    check(rejects(R"({"nothing":[]})", error), "a file with no conditions array is rejected");

    check(rejects(R"({"conditions":[]})", error),
          "an empty condition list is rejected, not treated as a run where all passed");

    check(rejects(R"({"conditions":[{"id":"x","kind":"telepathy","entity":"A"}]})", error),
          "a fourth condition kind is rejected");
    check(error.find("telepathy") != std::string::npos && error.find("closed") != std::string::npos,
          "naming the kind and saying the vocabulary is closed, never silently skipping it");

    check(rejects(R"({"conditions":[{"kind":"proximity","entities":["A","B"],"within_m":1}]})",
                  error),
          "a condition with no id is rejected - the id is how its verdict is traced");

    check(rejects(R"({"conditions":[
            {"id":"dup","kind":"terminal_state","entity":"A","removal_reason":"destroyed"},
            {"id":"dup","kind":"terminal_state","entity":"B","removal_reason":"destroyed"}]})",
                  error),
          "a duplicate id is rejected");
    check(error.find("duplicate") != std::string::npos, "and says which");

    check(rejects(R"({"conditions":[{"id":"x","kind":"proximity","entities":["A"],
                     "within_m":1}]})", error),
          "proximity with one entity is rejected");
    check(rejects(R"({"conditions":[{"id":"x","kind":"proximity","entities":["A","A"],
                     "within_m":1}]})", error),
          "proximity naming the same entity twice is rejected - it is met at distance zero");
    check(rejects(R"({"conditions":[{"id":"x","kind":"proximity","entities":["A","B"]}]})",
                  error),
          "proximity with no threshold is rejected");

    check(rejects(R"({"conditions":[{"id":"x","kind":"area","entity":"A",
                     "region":{"shape":"polygon","vertices":[[0,0],[1,1]]}}]})", error),
          "a polygon with two vertices is rejected");
    check(rejects(R"({"conditions":[{"id":"x","kind":"area","entity":"A",
                     "region":{"shape":"trapezoid"}}]})", error),
          "an unknown region shape is rejected");
    check(rejects(R"({"conditions":[{"id":"x","kind":"area","entity":"A",
                     "region":{"shape":"circle","centre":[91,0,0],"radius_m":1}}]})", error),
          "a latitude outside [-90, 90] is rejected");

    check(rejects(R"({"conditions":[{"id":"x","kind":"terminal_state","entity":"A"}]})", error),
          "terminal_state with neither a reason nor a field is rejected");
    check(rejects(R"({"conditions":[{"id":"x","kind":"terminal_state","entity":"A",
                     "field":"phase"}]})", error),
          "terminal_state with a field and no value is rejected");
    check(rejects(R"({"conditions":[{"id":"x","kind":"terminal_state","entity":"A",
                     "removal_reason":"destroyed","field":"phase","equals":"x"}]})", error),
          "terminal_state with both forms is rejected rather than one being ignored");

    section("what the loader accepts");
    std::vector<Condition> ok;
    check(parseConditions(R"({"conditions":[
            {"id":"p","kind":"proximity","entities":["A","B"],"within_m":0},
            {"id":"a","kind":"area","entity":"A",
             "region":{"shape":"circle","center":[0,0],"radius_m":5}},
            {"id":"t","kind":"terminal_state","entity":"A","removal_reason":"whatever"}]})",
                          ok, error),
          "a well-formed file of all three kinds loads");
    check(ok.size() == 3, "with all three conditions");
    if (ok.size() == 3) {
        check(ok[0].withinM == 0.0, "a zero threshold is legal - it means exactly coincident");
        check(ok[1].region.centre[2] == 0.0,
              "a two-element centre defaults its altitude to zero");
        check(ok[1].test == AreaTest::Inside, "test defaults to inside");
    }
    check(parseConditions(R"({"conditions":[{"id":"a","kind":"area","entity":"A",
            "region":{"shape":"circle","centre":[0,0],"radius_m":5}}]})", ok, error),
          "both British and American spellings of centre are accepted");
}

}  // namespace

int main() {
    std::printf("EXT-08 referee - unit tests (BTB-REF-1, BTB-REF-2, BTB-REF-3)\n");

    testGeodesy();
    testProximity();
    testProximityNeedsBoth();
    testOccupancyResetsPosition();
    testArea();
    testTerminalState();
    testNeverMet();
    testVerdictRecordShape();
    testLoaderRejections();

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
