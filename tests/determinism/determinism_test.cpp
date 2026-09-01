// EXT-08 - the determinism harness (BTB-CAP-3, R4).
//
// BTB-CAP-3 binds the recorder to what the recorder controls: **given the same sequence of
// published messages, produce the same bytes** - on every host, every build, every run. It
// does not, and cannot, bind the publisher; §14 of the format spec explains why at length.
//
// So this harness tests the emission path directly rather than end to end. That is not a
// weaker test, it is a more precise one: an end-to-end pair on a wall-clock-paced host varies
// because the *host* varies, which is exactly the confusion PRD rev 5 was written to remove.
//
// The three known non-determinism sources are all ours, and R4 says they are easy to
// reintroduce. Each gets a test that would fail if it came back:
//
//   1. StreamValueMap is an std::unordered_map. Iterating it would make the capture's byte
//      layout depend on hash-table internals. Tested by building the *same* payload with
//      different insertion orders and requiring identical output.
//   2. Float formatting is lossy and locale-sensitive by default. Tested by serialising under
//      a comma-decimal locale and requiring byte-identical output - the failure `%.17g` would
//      produce silently.
//   3. Any container of entities iterated for output must be ordered. Tested through the
//      header's schema array and the verdict's values object.
//
// Build (from a shell that has run C:\N8RO\setup.cmd and dev\setup-dev.cmd):
//
// It also carries the ONE writer-side invariant that is checkable with no file and no
// simulator: the handler-to-writer queue's structural reserve (BTB-BP-4, D-8, format spec
// §16). That is not a determinism property, and it lives here because RecordQueue links no
// import library and has no other simulator-free home - a separate harness for one class
// would be a third build line for the same three checks.
//
// Build (from a shell that has run C:\N8RO\setup.cmd and dev\setup-dev.cmd):
//
//   cl /std:c++17 /EHsc /W4 /O2 ^
//      /I %N8RO_RELEASE%\include\n8ro-core /I %N8RO_RELEASE%\include\n8ro-sim ^
//      /Fe:determinism_test.exe ^
//      tests\determinism\determinism_test.cpp src\CaptureFormat.cpp src\Json.cpp ^
//      src\Referee.cpp src\Conditions.cpp src\Geodesy.cpp src\JsonParse.cpp ^
//      src\RecordQueue.cpp

#include "../../src/CaptureFormat.h"
#include "../../src/CaptureRecord.h"
#include "../../src/RecordQueue.h"
#include "../../src/Referee.h"

#include <chrono>
#include <clocale>
#include <cstdio>
#include <locale>
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

using namespace n8ro::bridge;

// A schema shaped like the real entity-state one, including the twelfth field that this
// platform declares and never publishes.
n8ro::sim::MessageSchema entityStateSchema() {
    n8ro::sim::MessageSchema schema;
    schema.messageName = "simEntityStateUpdate";
    schema.topic = "sim/entity/state";
    schema.schemaHash = 2652370635u;
    schema.messageId = 1308183250u;
    schema.wireVersion = 1;
    const char* names[] = {"simulationTime", "scenarioEntityName", "name",
                           "team",           "phase",              "health",
                           "presence",       "conditions",         "positionGeodetic",
                           "orientationYprRad", "velocityNed",     "activeAnimation"};
    for (const char* name : names) {
        n8ro::sim::FieldSchema field;
        field.name = name;
        field.size = 1;
        schema.fields.push_back(field);
    }
    return schema;
}

// Values chosen to exercise every shape a real capture contains: accumulated frame error, a
// round number that shortest round-trip writes without a fractional part, a scientific-
// notation value, a negative zero, and a string needing escapes.
void fillPayload(n8ro::sim::StreamValueMap& values, bool reverseInsertion) {
    struct Entry {
        const char* name;
        int kind;   // 0 string, 1 double, 2 int, 3 vector<double>
    };
    const Entry entries[] = {
        {"simulationTime", 1},   {"scenarioEntityName", 0}, {"name", 0},
        {"team", 0},             {"phase", 0},              {"health", 0},
        {"presence", 0},         {"conditions", 2},         {"positionGeodetic", 3},
        {"orientationYprRad", 3}, {"velocityNed", 3},
    };
    const int count = static_cast<int>(sizeof(entries) / sizeof(entries[0]));

    for (int step = 0; step < count; ++step) {
        const int i = reverseInsertion ? count - 1 - step : step;
        const Entry& entry = entries[i];
        n8ro::sim::StreamValue value;
        switch (entry.kind) {
            case 0:
                value.data = std::string("Red\"UAV\\N_01\ttab");
                break;
            case 1:
                value.data = 72.09999999999805;   // accumulated frame error, preserved to the bit
                break;
            case 2:
                value.data = static_cast<std::int64_t>(0);
                break;
            default:
                // 400 is a double and shortest round-trip writes it as `400`, not `400.0`;
                // the third is scientific notation. Both are real shapes from a real capture.
                value.data = std::vector<double>{-23.454302692591245, -68.25275, 400.0};
                break;
        }
        values.emplace(entry.name, std::move(value));
    }
    n8ro::sim::StreamValue negZero;
    negZero.data = std::vector<double>{-55.0, 6.735557395310443e-15, -0.0};
    values.insert_or_assign("velocityNed", std::move(negZero));
}

CaptureRecord sampleRecord(bool reverseInsertion) {
    CaptureRecord record;
    record.kind = RecordKind::Sample;
    record.subject = "RedUAV_N_01";
    record.occupancy = 2;
    record.simTimeS = 72.09999999999805;
    fillPayload(record.values, reverseInsertion);
    return record;
}

capture::HeaderInfo headerInfo() {
    capture::HeaderInfo info;
    info.platform.engineConfig = "SimEngineClient_SharedMemory";
    info.platform.modelPath = "C:\\N8RO\\data\\db";
    info.platform.schemaFile = "N8roSimSchema";
    info.platform.schemaVersion = "";
    info.platform.runtimeVersion = "unknown";
    info.attachedMidRun = false;
    info.subscription.topic = "sim/entity/state";
    info.subscription.backpressurePolicy = "FIFO_DROP";
    info.subscription.queueSize = 1024;
    // BTB-CAP-6. A bound and a rotation position, so the golden lines below cover the keys
    // that carry them rather than only the keys that predate them.
    info.limits.maxBytes = 104857600;
    info.limits.maxSamples = 0;
    info.limits.onSizeLimit = "rotate";
    info.part = 2;
    info.continuesFrom = "capture-atacama-air-defense-000.part001.n8rocap.jsonl";
    // Deliberately out of alphabetical order: the header sorts by message name, and this is
    // what would catch that sort being removed.
    n8ro::sim::MessageSchema second = entityStateSchema();
    second.messageName = "aaaFirstAlphabetically";
    info.schemas.push_back(entityStateSchema());
    info.schemas.push_back(second);
    return info;
}

// Everything a capture can contain, in one string, so a single comparison covers the lot.
std::string serialiseEverything(bool reverseInsertion) {
    const n8ro::sim::MessageSchema schema = entityStateSchema();
    std::string out;
    out += capture::writeHeader(headerInfo());
    out += '\n';
    out += capture::writeSegmentOpen(0.0, 0, "Atacama Air Defense");
    out += '\n';
    out += capture::writeEntityAdd(0.0, 0, "RedUAV_N_01", 1);
    out += '\n';
    out += capture::writeSample(sampleRecord(reverseInsertion), 0, schema);
    out += '\n';
    out += capture::writeEntityRemove(149.44999999999973, 0, "RedUAV_N_01", 1, "destroyed");
    out += '\n';

    Verdict verdict;
    verdict.conditionId = "red-leader-reaches-airfield";
    verdict.met = true;
    verdict.segment = 0;
    verdict.simTimeS = 149.04999999999964;
    verdict.entities = {"RedUAV_N_01", "BlueBase_Airfield"};
    verdict.numberValues["distance_m"] = "2999.9981116642175";
    verdict.numberValues["within_m"] = "3000";
    verdict.stringValues["kind"] = "proximity";
    out += writeVerdict(verdict);
    out += '\n';

    out += capture::writeSegmentClose(0.0, 0, "Atacama Air Defense", "scenario_unloaded");
    out += '\n';

    capture::TrailerCounts counts;
    counts.segments = 2;
    counts.samples = 132150;
    counts.entitiesAdded = 132;
    counts.entitiesRemoved = 90;
    counts.verdicts = 7;
    capture::TrailerDrops drops;
    capture::TrailerBusMetrics metrics;
    out += capture::writeTrailer(0.0, "host_lost", counts, drops, metrics);
    out += '\n';
    return out;
}

// ---------------------------------------------------------------------------------------

void testRepeatable() {
    section("the same records always produce the same bytes");

    const std::string first = serialiseEverything(false);
    bool stable = true;
    for (int i = 0; i < 1000; ++i) {
        if (serialiseEverything(false) != first) {
            stable = false;
            break;
        }
    }
    check(stable, "1000 serialisations of one record set are byte-identical");
    check(!first.empty(), "and they produced something");
}

void testInsertionOrderDoesNotShow() {
    section("R4 hazard 1: StreamValueMap iteration order must not reach the file");

    // The same payload, built by inserting its fields in the opposite order. An unordered_map
    // is free to lay those out differently, so a serialiser that iterated it would produce
    // different bytes. One that walks MessageSchema::fields and looks each up cannot.
    const std::string forward = serialiseEverything(false);
    const std::string backward = serialiseEverything(true);
    check(forward == backward,
          "a payload built in the opposite insertion order serialises identically - field "
          "order comes from the schema, never from the map");

    // And the order is the schema's, not alphabetical and not the map's.
    const std::string sample =
        capture::writeSample(sampleRecord(true), 0, entityStateSchema());
    const std::size_t timePos = sample.find("\"simulationTime\"");
    const std::size_t namePos = sample.find("\"scenarioEntityName\"");
    const std::size_t teamPos = sample.find("\"team\"");
    check(timePos != std::string::npos && namePos != std::string::npos &&
              teamPos != std::string::npos,
          "the sample carries the fields it was given");
    check(timePos < namePos && namePos < teamPos,
          "and they appear in MessageSchema::fields order");
    check(sample.find("\"activeAnimation\"") == std::string::npos,
          "a schema-declared field the publisher did not send is absent, not defaulted");
}

void testLocaleIndependence() {
    section("R4 hazard 2: float formatting must be locale-independent");

    const std::string cLocale = serialiseEverything(false);

    // The failure this catches is silent. `%.17g` is round-trip exact and locale-dependent:
    // under a comma-decimal locale it emits `0,05`, which is not JSON, and nothing reports an
    // error. This machine's own locale is comma-decimal, which is what made OQ-5 worth
    // settling by test rather than by reading the standard.
    bool switched = false;
    if (std::setlocale(LC_ALL, "de-DE") != nullptr) {
        switched = true;
    } else if (std::setlocale(LC_ALL, "German_Germany.1252") != nullptr) {
        switched = true;
    }
    check(switched,
          "a comma-decimal locale is available to test against (skipped if the host has none)");

    if (switched) {
        const std::string commaLocale = serialiseEverything(false);
        check(cLocale == commaLocale,
              "output under a comma-decimal locale is byte-identical to output under C");
        check(commaLocale.find(',') == std::string::npos ||
                  commaLocale.find(",\"") != std::string::npos,
              "no decimal comma appears where a decimal point belongs");
        check(commaLocale.find("72.09999999999805") != std::string::npos,
              "the accumulated frame error is still written with a decimal point");
    }

    // Also through the C++ locale machinery, which some formatting paths consult instead.
    try {
        std::locale::global(std::locale("de-DE"));
        check(serialiseEverything(false) == cLocale,
              "and identical again with the global C++ locale set to a comma-decimal one");
    } catch (const std::exception&) {
        std::printf("  (no de-DE C++ locale on this host; the C-locale check above stands)\n");
    }
    std::locale::global(std::locale::classic());
    std::setlocale(LC_ALL, "C");
}

void testOrderedContainers() {
    section("R4 hazard 3: nothing with unspecified iteration order is iterated for output");

    const std::string header = capture::writeHeader(headerInfo());
    const std::size_t firstPos = header.find("\"aaaFirstAlphabetically\"");
    const std::size_t secondPos = header.find("\"simEntityStateUpdate\"");
    check(firstPos != std::string::npos && secondPos != std::string::npos,
          "the header carries both schemas");
    check(firstPos < secondPos,
          "header.schemas is sorted by message_name, whatever order it was supplied in");

    Verdict verdict;
    verdict.conditionId = "x";
    verdict.numberValues["zulu"] = "1";
    verdict.numberValues["alpha"] = "2";
    verdict.numberValues["mike"] = "3";
    const std::string line = writeVerdict(verdict);
    check(line.find("\"alpha\":2,\"mike\":3,\"zulu\":1") != std::string::npos,
          "a verdict's values come out of an ordered map, in key order");
}

// BTB-CAP-4's central promise, and the half of UAC-BTB-CAP-4 that is checkable without a
// simulator: adding a field to a message's schema makes it appear in new captures **with no
// code change**. The other half - a real engine publishing a real new field - cannot be
// staged here, because we cannot make the host publish a field its build does not have. What
// this does test is the whole of what EXT-08 controls: the writer is driven by
// MessageSchema::fields and never by a compiled-in list.
void testSchemaGrowth() {
    section("BTB-CAP-4: a field added to the schema appears, with no code change");

    n8ro::sim::MessageSchema grown = entityStateSchema();
    n8ro::sim::FieldSchema added;
    added.name = "fuelFraction";     // deliberately not last alphabetically, and not first
    added.size = 1;
    // Inserted in the MIDDLE of the declaration, not appended. Appending would pass even on a
    // writer that emitted a hard-coded prefix and then whatever was left over; inserting only
    // passes if field order really does come from the schema vector.
    grown.fields.insert(grown.fields.begin() + 3, added);

    n8ro::sim::StreamValueMap values;
    fillPayload(values, false);
    n8ro::sim::StreamValue fuel;
    fuel.data = 0.375;
    values["fuelFraction"] = fuel;

    CaptureRecord record;
    record.kind = RecordKind::Sample;
    record.subject = "RedUAV_N_01";
    record.occupancy = 1;
    record.simTimeS = 12.5;
    record.values = values;

    const std::string line = capture::writeSample(record, 0, grown);
    check(line.find("\"fuelFraction\":0.375") != std::string::npos,
          "a schema-declared field the publisher sent appears in the sample record");

    // In schema order. Inserted at index 3, the declaration now reads
    //   simulationTime, scenarioEntityName, name, fuelFraction, team, phase, ...
    // so the new field must land between `name` and `team`.
    //
    // Searched inside the `fields` object rather than the whole line: the envelope repeats
    // two of the message's own values on purpose (spec 8.4), so a find() over the whole
    // record can match an envelope key and compare positions across two different objects.
    const std::size_t fieldsAt = line.find("\"fields\":{");
    check(fieldsAt != std::string::npos, "the record carries a `fields` object");
    const std::string fields = fieldsAt == std::string::npos ? std::string()
                                                            : line.substr(fieldsAt);
    const std::size_t nameAt = fields.find("\"name\"");
    const std::size_t fuelAt = fields.find("\"fuelFraction\"");
    const std::size_t teamAt = fields.find("\"team\"");
    check(nameAt != std::string::npos && fuelAt != std::string::npos &&
              teamAt != std::string::npos && nameAt < fuelAt && fuelAt < teamAt,
          "and in the position the schema declares it - between `name` and `team`, not "
          "appended to the end");

    // The converse, and the reason BTB-CAP-4's rule is "verbatim" rather than "complete":
    // a declared field the publisher did not send is ABSENT, never defaulted. This is the
    // activeAnimation case - twelve declared, eleven ever published (notes.md, M3).
    check(line.find("activeAnimation") == std::string::npos,
          "a declared field the publisher did not send is absent, not defaulted to a zero");

    // And the header still carries the full declaration, so a reader can tell the two apart.
    capture::HeaderInfo info = headerInfo();
    info.schemas.clear();
    info.schemas.push_back(grown);
    const std::string header = capture::writeHeader(info);
    check(header.find("\"fuelFraction\"") != std::string::npos &&
              header.find("\"activeAnimation\"") != std::string::npos,
          "header.schemas declares both - the sent field and the never-sent one (spec 8.2)");
}

void testKnownBytes() {
    section("the exact bytes, so a change to them is a deliberate act");

    // A golden line. If a future change alters the format's spelling, this fails and the
    // author has to decide whether that was intended - which for a cross-repo contract after
    // the M7 freeze is exactly the friction wanted.
    const std::string expected =
        "{\"type\":\"entity_remove\",\"sim_time_s\":149.44999999999973,\"segment\":0,"
        "\"entity\":\"RedUAV_N_01\",\"occupancy\":1,\"reason\":\"destroyed\"}";
    check(capture::writeEntityRemove(149.44999999999973, 0, "RedUAV_N_01", 1, "destroyed") ==
              expected,
          "entity_remove is spelled exactly as the format specification says");

    check(capture::writeSegmentOpen(0.0, 1, "Atacama Air Defense") ==
              "{\"type\":\"segment_open\",\"sim_time_s\":0,\"segment\":1,"
              "\"scenario\":\"Atacama Air Defense\"}",
          "segment_open likewise - and note sim_time_s is `0`, not `0.0` (spec 8.3)");

    const std::string header = capture::writeHeader(headerInfo());
    check(header.rfind("{\"format_version\":\"n8ro-capture/1\",\"type\":\"header\"", 0) == 0,
          "format_version is the first key of the header, so an unknown version is rejected "
          "before anything else is parsed");

    // BTB-CAP-6's three header keys, spelled exactly as docs/capture-format-v1.md 6.6 and
    // 6.7 specify them. They are what a bounded capture states about its own bound, and they
    // cross the repo boundary like every other key here.
    check(header.find("\"limits\":{\"max_bytes\":104857600,\"max_samples\":0,"
                      "\"on_size_limit\":\"rotate\"}") != std::string::npos,
          "header.limits is spelled exactly as the format specification says (spec 6.6)");
    check(header.find("\"part\":2,\"continues_from\":"
                      "\"capture-atacama-air-defense-000.part001.n8rocap.jsonl\"") !=
              std::string::npos,
          "header.part and header.continues_from likewise, and in that order (spec 6.7)");

    // The ordinary case: no bound configured, and no continuation. `part` is still written -
    // absent and 0 mean the same thing to a reader, but writing it costs nine bytes and
    // removes a case from every reader that would otherwise have to handle both.
    capture::HeaderInfo plain = headerInfo();
    plain.limits = capture::SizeLimitInfo{};
    plain.part = 0;
    plain.continuesFrom.clear();
    const std::string plainHeader = capture::writeHeader(plain);
    check(plainHeader.find("\"limits\":{\"max_bytes\":0,\"max_samples\":0,"
                           "\"on_size_limit\":\"stop\"}") != std::string::npos,
          "an unbounded capture still states that it is unbounded, rather than saying nothing");
    check(plainHeader.find("\"continues_from\"") == std::string::npos,
          "continues_from is omitted on a first part, not written empty (spec 6.7)");

    // A trailer that continues, and one that does not. `continued_in` is the last key and is
    // present only when there is a next part - its absence is how a reader knows a file ends
    // the run rather than merely ending.
    const capture::TrailerCounts counts{1, 6, 0, 0, 0};
    const std::string continued = capture::writeTrailer(
        1.5, "size_limit", counts, capture::TrailerDrops{}, capture::TrailerBusMetrics{},
        "capture-atacama-air-defense-000.part003.n8rocap.jsonl");
    check(continued.find("\"continued_in\":\"capture-atacama-air-defense-000.part003."
                         "n8rocap.jsonl\"}") != std::string::npos,
          "trailer.continued_in is the last key of a rotated part's trailer (spec 11)");
    const std::string terminal = capture::writeTrailer(
        1.5, "host_lost", counts, capture::TrailerDrops{}, capture::TrailerBusMetrics{});
    check(terminal.find("continued_in") == std::string::npos,
          "and is absent entirely from the trailer that ends a run");
}

void testNoWallClock() {
    section("BTB-CAP-2: no wall-clock-derived value anywhere in the emitted bytes");

    const std::string everything = serialiseEverything(false);
    // Strict date and clock shapes. A bare-year regex would match scenario entity names -
    // the platform names weapons things like BlueSAM_ShortRange_wpn_20900_2 - which is a
    // false positive M4 had to exclude by name.
    const char* shapes[] = {"T00:", "T01:", "GMT", "UTC", "Z\"", "1970", "2026-", "20:00:"};
    bool clean = true;
    for (const char* shape : shapes) {
        if (everything.find(shape) != std::string::npos) {
            clean = false;
            std::printf("  (found %s)\n", shape);
        }
    }
    check(clean, "no date or clock shape appears in any record");
}

// ---------------------------------------------------------------------------------------
// BTB-BP-4 / D-8 / format spec section 16: overload costs data and never structure.
//
// The reserve is two mechanisms, not one. The threshold (a sample is refused above
// `sampleCapacity`, a structural record only above `sampleCapacity + reserve`) is what stops
// a sample from *filling* the queue past a roster record's headroom. Under `drop_oldest` a
// second mechanism is needed, because an arriving sample does not merely fail to fit - it
// chooses a victim, and the front of the queue during a scenario load is the entity_created
// burst. Section 16 tells a reader in writing that `events_not_recorded` cannot be non-zero
// from overload alone, so this is checked for BOTH policies.
// ---------------------------------------------------------------------------------------

n8ro::bridge::CaptureRecord sampleRecord(const std::string& entity) {
    n8ro::bridge::CaptureRecord record;
    record.kind = n8ro::bridge::RecordKind::Sample;
    record.subject = entity;
    record.occupancy = 1;
    return record;
}

n8ro::bridge::CaptureRecord addRecord(const std::string& entity) {
    n8ro::bridge::CaptureRecord record;
    record.kind = n8ro::bridge::RecordKind::EntityAdd;
    record.subject = entity;
    record.occupancy = 1;
    return record;
}

std::vector<n8ro::bridge::CaptureRecord> drainAll(n8ro::bridge::RecordQueue& queue) {
    std::vector<n8ro::bridge::CaptureRecord> out;
    static_cast<void>(queue.waitAndDrain(out, std::chrono::milliseconds(0)));
    return out;
}

std::size_t countStructural(const std::vector<n8ro::bridge::CaptureRecord>& records) {
    std::size_t n = 0;
    for (const n8ro::bridge::CaptureRecord& record : records) {
        if (n8ro::bridge::isStructuralRecord(record.kind)) {
            ++n;
        }
    }
    return n;
}

void testStructuralReserve() {
    section("BTB-BP-4: overload costs samples and never structure, under BOTH policies");

    for (const n8ro::bridge::OverflowPolicy policy :
         {n8ro::bridge::OverflowPolicy::DropNewest, n8ro::bridge::OverflowPolicy::DropOldest}) {
        const std::string name = n8ro::bridge::overflowPolicyName(policy);

        // The shape D-8's overload experiment had: a creation burst enqueued at load, then a
        // sample stream that immediately overruns a tiny queue.
        n8ro::bridge::RecordQueue queue(4, 8, policy);
        for (int i = 0; i < 6; ++i) {
            queue.offer(addRecord("Blue_" + std::to_string(i)));
        }
        for (int i = 0; i < 500; ++i) {
            queue.offer(sampleRecord("Blue_0"));
        }

        const n8ro::bridge::QueueCounters counters = queue.counters();
        check(counters.structuralDropped == 0,
              name + ": 500 samples against a 4-record queue drop no structural record (got " +
                  std::to_string(counters.structuralDropped) + ")");
        check(counters.samplesDropped > 0,
              name + ": and the samples that did not fit are counted");

        const std::vector<n8ro::bridge::CaptureRecord> drained = drainAll(queue);
        check(countStructural(drained) == 6,
              name + ": all 6 entity_add records are still in the queue (got " +
                  std::to_string(countStructural(drained)) + ")");

        // Nothing invented and nothing lost: every offered record either reached the queue or
        // was counted as dropped. This is the arithmetic tenet 3 rests on.
        const std::uint64_t accountedFor = static_cast<std::uint64_t>(drained.size()) +
                                           counters.samplesDropped + counters.structuralDropped;
        check(accountedFor == counters.samplesOffered + counters.structuralOffered,
              name + ": every offered record is either queued or counted as dropped");
    }

    // The corner the two-threshold design leaves: a queue holding nothing but structural
    // records has no sample to give up. The arrival is refused rather than a roster record
    // evicted, and it is still counted.
    {
        n8ro::bridge::RecordQueue queue(2, 0, n8ro::bridge::OverflowPolicy::DropOldest);
        queue.offer(addRecord("Blue_0"));
        queue.offer(addRecord("Blue_1"));
        queue.offer(sampleRecord("Blue_0"));

        const n8ro::bridge::QueueCounters counters = queue.counters();
        check(counters.structuralDropped == 0 && counters.samplesDropped == 1,
              "drop_oldest with no sample to evict refuses the arriving sample and counts it");
        check(countStructural(drainAll(queue)) == 2,
              "and both structural records survive");
    }
}

}  // namespace

int main() {
    std::printf("EXT-08 determinism harness (BTB-CAP-3, R4)\n");
    std::printf("Given the same records, the recorder must produce the same bytes.\n");

    testRepeatable();
    testInsertionOrderDoesNotShow();
    testLocaleIndependence();
    testOrderedContainers();
    testKnownBytes();
    testSchemaGrowth();
    testNoWallClock();
    testStructuralReserve();

    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
