// EXT-08 - unit tests for the entity picture (BTB-EP-3, BTB-EP-4, ADR-6).
//
// The picture is ours to own permanently (OQ-1, decided), so it gets tests that do not
// need a simulator, a bus, or a model database. Every case below drives EntityPicture by
// handing it StreamValueMaps directly - which is exactly what MessageBusPacked's
// DecodedHandler does, so the tests exercise the real entry points and not a stand-in.
//
// That is also why the picture takes a StreamValueMap rather than a Message: the same
// class can be fed from the bus (M3) or from a stored capture on the replay path (M6),
// with no second implementation and no interface needed to make it substitutable.
//
// No framework by design - the repo's other probe (tests/float-format) is standalone too.
// Build and run:
//     cl /std:c++17 /EHsc /W4 /I<release>\include\n8ro-core /I<release>\include\n8ro-sim ^
//        /Fe:entity_picture_test.exe entity_picture_test.cpp ..\..\src\EntityPicture.cpp
//     entity_picture_test.exe
// Exit code 0 if every check passes; 1 otherwise, with the failures named.

#include "../../src/EntityPicture.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

using n8ro::bridge::EntityPicture;
using n8ro::bridge::LatestSample;
using n8ro::bridge::PictureSnapshot;
using n8ro::bridge::RosterEvent;
using n8ro::sim::StreamValue;
using n8ro::sim::StreamValueMap;

int g_checks = 0;
int g_failures = 0;
const char* g_case = "";

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  [%s] %s\n", g_case, what);
    }
}

template <typename TLeft, typename TRight>
void checkEq(const TLeft& actual, const TRight& expected, const char* what) {
    ++g_checks;
    if (!(actual == static_cast<TLeft>(expected))) {
        ++g_failures;
        std::printf("  FAIL  [%s] %s\n", g_case, what);
    }
}

void beginCase(const char* name) {
    g_case = name;
    std::printf("- %s\n", name);
}

// Non-throwing map lookups. std::map::at throws on a missing key, which would abort the
// run and hide every later case behind one failure - and a test binary that crashes
// instead of reporting is worth less than one that says which check failed.
std::uint64_t countOf(const std::map<std::string, std::uint64_t>& counts, const std::string& key) {
    const auto it = counts.find(key);
    return it == counts.end() ? 0u : it->second;
}

std::string reasonOf(const std::map<std::string, n8ro::bridge::Occupancy>& roster,
                     const std::string& key) {
    const auto it = roster.find(key);
    return it == roster.end() ? std::string("<absent>") : it->second.lastRemovalReason;
}

// --- payload builders -------------------------------------------------------------
// Shaped like the real thing: sim/entity/state and sim/entity/event as the runtime
// schemas declare them. Fields are added individually so a test can deliberately omit
// one, which is the case that matters - a packed payload carries only what the publisher
// wrote, and the picture must read presence rather than assume it.

StreamValueMap sample(const std::string& name, double simTime) {
    StreamValueMap values;
    values["simulationTime"] = StreamValue(simTime);
    values["scenarioEntityName"] = StreamValue(name);
    values["name"] = StreamValue(std::string("Air_UAV_LoiteringMunition_Generic"));
    values["team"] = StreamValue(std::string("Red"));
    values["phase"] = StreamValue(std::string("operational"));
    values["health"] = StreamValue(std::string("nominal"));
    values["presence"] = StreamValue(std::string("active"));
    values["conditions"] = StreamValue(static_cast<std::int64_t>(0));
    values["positionGeodetic"] = StreamValue(std::vector<double>{-23.4, -68.2, 400.0});
    values["orientationYprRad"] = StreamValue(std::vector<double>{3.14159, 0.0, 0.0});
    values["velocityNed"] = StreamValue(std::vector<double>{-55.0, 0.0, 0.0});
    return values;
}

StreamValueMap createEvent(const std::string& name, double simTime) {
    StreamValueMap values;
    values["eventName"] = StreamValue(std::string("entity_created"));
    values["simulationTime"] = StreamValue(simTime);
    values["scenarioEntityName"] = StreamValue(name);
    values["profileName"] = StreamValue(std::string("Air_UAV_LoiteringMunition_Generic"));
    values["teamName"] = StreamValue(std::string("Red"));
    return values;
}

StreamValueMap deleteEvent(const std::string& name, double simTime, const std::string& reason) {
    StreamValueMap values;
    values["eventName"] = StreamValue(std::string("entity_deleted"));
    values["simulationTime"] = StreamValue(simTime);
    values["scenarioEntityName"] = StreamValue(name);
    values["reason"] = StreamValue(reason);
    return values;
}

// --- the cases --------------------------------------------------------------------

void testAcceptsSampleWithinOpenOccupancy() {
    beginCase("a sample inside an open occupancy is accepted, keyed and stamped");
    EntityPicture picture;
    picture.onEntityEvent(createEvent("RedUAV_N_01", 0.0));
    picture.onSample(sample("RedUAV_N_01", 12.5));

    const PictureSnapshot snap = picture.snapshot();
    checkEq(snap.counters.samplesAccepted, 1u, "one sample accepted");
    checkEq(snap.counters.samplesOrphaned, 0u, "none orphaned");
    checkEq(snap.liveCount, std::size_t{1}, "one live entity");
    const LatestSample* live = snap.liveSample("RedUAV_N_01");
    check(live != nullptr, "liveSample returns the entry");
    if (live != nullptr) {
        checkEq(live->simulationTimeS, 12.5, "carries the sample's own simulation time");
        checkEq(live->generation, 1u, "belongs to occupancy 1");
    }
}

void testSampleBeforeAnyCreateIsOrphaned() {
    // The mid-run-attach case: the bridge missed the entity_created burst. This must be
    // counted, not silently accepted, or an empty roster looks like an idle scenario.
    beginCase("a sample with no occupancy ever opened is orphaned, not accepted");
    EntityPicture picture;
    picture.onSample(sample("RedUAV_N_01", 12.5));

    const PictureSnapshot snap = picture.snapshot();
    checkEq(snap.counters.samplesAccepted, 0u, "nothing accepted");
    checkEq(snap.counters.samplesOrphaned, 1u, "counted as orphaned");
    check(snap.latest.empty(), "never entered the latest-sample map");
    check(snap.liveSample("RedUAV_N_01") == nullptr, "not readable as live");
}

// --- orphansBeforeFirstAccepted: the late-attach signature, stated causally -------
//
// This counter is what the capture's `attached_mid_run` is derived from (BTB-CAP-1). It has
// to be decided by what happened rather than by what a status tick observed a second after
// start-up, because a race would put a run-to-run-variable value into a file that must be
// byte-reproducible (BTB-CAP-3). Three cases: attached early, attached late, and frozen.

void testEarlyAttachHasNoOrphansBeforeFirstSample() {
    beginCase("a bridge present at scenario load accepts its first sample with no orphans behind it");
    EntityPicture picture;
    picture.onEntityEvent(createEvent("RedUAV_N_01", 0.0));
    picture.onSample(sample("RedUAV_N_01", 0.05));

    const PictureSnapshot snap = picture.snapshot();
    checkEq(snap.counters.samplesAccepted, 1u, "the sample was accepted");
    checkEq(snap.counters.samplesOrphaned, 0u, "nothing was orphaned");
    checkEq(snap.counters.orphansBeforeFirstAccepted, 0u,
            "so the late-attach signal is zero - this bridge saw the roster being built");
}

void testLateAttachIsVisibleAtTheFirstAcceptedSample() {
    // M3's measured case: the entity_created burst fires once, at scenario load, and a
    // bridge that arrives after it sees nothing but samples for entities it never saw
    // created - 7 740 of them on the run that found this, with zero drops and no error.
    beginCase("a bridge that missed the creation burst carries orphans into its first acceptance");
    EntityPicture picture;
    for (int i = 0; i < 5; ++i) {
        picture.onSample(sample("RedUAV_N_01", 12.5 + 0.05 * i));   // all orphaned
    }
    // The engine eventually creates something, and from there the picture works normally.
    picture.onEntityEvent(createEvent("RedUAV_N_02", 13.0));
    picture.onSample(sample("RedUAV_N_02", 13.05));

    const PictureSnapshot snap = picture.snapshot();
    checkEq(snap.counters.samplesAccepted, 1u, "one sample accepted");
    checkEq(snap.counters.samplesOrphaned, 5u, "five orphaned before it");
    checkEq(snap.counters.orphansBeforeFirstAccepted, 5u,
            "and the signal carries them - this bridge attached mid-run");
}

void testOrphansBeforeFirstAcceptedIsFrozen() {
    // Read at the end of a run it would answer a different question: by then almost every
    // bridge has orphans, because the engine's teardown churn produces them. It must be the
    // value at the FIRST acceptance and never move again.
    beginCase("the late-attach signal freezes at the first acceptance and never moves");
    EntityPicture picture;
    picture.onEntityEvent(createEvent("RedUAV_N_01", 0.0));
    picture.onSample(sample("RedUAV_N_01", 0.05));
    checkEq(picture.snapshot().counters.orphansBeforeFirstAccepted, 0u, "zero at the first sample");

    // Now orphan a pile of samples under a name that was never created, and accept more
    // under the one that was.
    for (int i = 0; i < 9; ++i) {
        picture.onSample(sample("NeverCreated", 1.0 + 0.05 * i));
        picture.onSample(sample("RedUAV_N_01", 1.0 + 0.05 * i));
    }

    const PictureSnapshot snap = picture.snapshot();
    checkEq(snap.counters.samplesOrphaned, 9u, "the later orphans are still counted");
    checkEq(snap.counters.samplesAccepted, 10u, "and the later acceptances too");
    checkEq(snap.counters.orphansBeforeFirstAccepted, 0u,
            "but the signal is unmoved - it is a fact about attachment, not about the run");
}

void testNoSampleAfterRemovalWithinOccupancy() {
    // BTB-EP-3's criterion, in its satisfiable form.
    beginCase("BTB-EP-3: no sample enters an occupancy after that occupancy is removed");
    EntityPicture picture;
    picture.onEntityEvent(createEvent("RedUAV_N_01", 0.0));
    picture.onSample(sample("RedUAV_N_01", 10.0));
    picture.onEntityEvent(deleteEvent("RedUAV_N_01", 18.2, "destroyed"));
    picture.onSample(sample("RedUAV_N_01", 18.25));   // must not land

    const PictureSnapshot snap = picture.snapshot();
    checkEq(snap.counters.samplesAccepted, 1u, "only the pre-removal sample accepted");
    checkEq(snap.counters.samplesOrphaned, 1u, "the post-removal sample is orphaned");
    checkEq(snap.liveCount, std::size_t{0}, "nothing live");
    check(snap.liveSample("RedUAV_N_01") == nullptr, "removed entity is not live");

    const LatestSample* last = snap.lastKnownSample("RedUAV_N_01");
    check(last != nullptr, "its final sample is still retained for 'where did it die'");
    if (last != nullptr) {
        checkEq(last->simulationTimeS, 10.0, "retained sample is the last one before removal");
    }
}

void testRemovalReasonVerbatim() {
    beginCase("removal reasons are preserved verbatim, including outside the engine's set");
    EntityPicture picture;
    for (const std::string& name : {std::string("a"), std::string("b"), std::string("c")}) {
        picture.onEntityEvent(createEvent(name, 0.0));
    }
    picture.onEntityEvent(deleteEvent("a", 1.0, "destroyed"));
    picture.onEntityEvent(deleteEvent("b", 2.0, "expended"));
    // A supplier-specific value the engine itself never raises. It must survive uncoerced.
    picture.onEntityEvent(deleteEvent("c", 3.0, "acme_radar_jammer_consumed"));

    const PictureSnapshot snap = picture.snapshot();
    checkEq(snap.removalsByReason.size(), std::size_t{3}, "three distinct reasons");
    checkEq(snap.removalsByReason.count("destroyed"), std::size_t{1}, "destroyed recorded");
    checkEq(snap.removalsByReason.count("expended"), std::size_t{1}, "expended recorded");
    checkEq(snap.removalsByReason.count("acme_radar_jammer_consumed"), std::size_t{1},
            "supplier-specific reason recorded verbatim, not coerced or dropped");
    checkEq(reasonOf(snap.roster, "c"), std::string("acme_radar_jammer_consumed"),
            "the roster entry carries it too");
    checkEq(reasonOf(snap.roster, "a"), std::string("destroyed"), "and each carries its own");
}

void testRecreatedNameOpensNextOccupancy() {
    // ADR-6, and the exact sequence the reference run produced for RedUAV_N_01: killed
    // mid-run, then re-created by the engine's stop-path reset.
    beginCase("ADR-6: a re-created name opens the next occupancy and samples resume");
    EntityPicture picture;
    picture.onEntityEvent(createEvent("RedUAV_N_01", 0.0));
    picture.onSample(sample("RedUAV_N_01", 100.0));
    picture.onEntityEvent(deleteEvent("RedUAV_N_01", 149.45, "destroyed"));
    picture.onEntityEvent(createEvent("RedUAV_N_01", 0.0));      // re-created, same name
    picture.onSample(sample("RedUAV_N_01", 0.05));

    const PictureSnapshot snap = picture.snapshot();
    checkEq(snap.roster.find("RedUAV_N_01")->second.generation, 2u,
            "second tenure is occupancy 2");
    check(snap.isLive("RedUAV_N_01"), "the new occupancy is open");
    checkEq(snap.counters.samplesOrphaned, 0u,
            "the resumed sample is NOT an orphan - this is what the criterion turns on");
    checkEq(snap.counters.samplesAccepted, 2u, "both samples accepted, in different tenures");

    const LatestSample* live = snap.liveSample("RedUAV_N_01");
    check(live != nullptr, "live again under the new occupancy");
    if (live != nullptr) {
        checkEq(live->generation, 2u, "the retained sample belongs to occupancy 2");
        checkEq(live->simulationTimeS, 0.05, "and is the post-recreation sample");
    }
    checkEq(reasonOf(snap.roster, "RedUAV_N_01"), std::string(""),
            "a re-created occupancy does not inherit the previous tenure's removal reason");
}

void testRecreateDoesNotInheritPreviousSample() {
    // Found by mutation testing: deleting the latest_.erase() on re-create left every
    // earlier test green, because each of them published a fresh sample immediately after
    // re-creating. The gap is the window *between* the two - a stale entry from a closed
    // tenure readable as the current state of the new one, which is exactly what
    // BTB-EP-4's ordering criterion forbids.
    beginCase("a new occupancy starts empty - the previous tenure's sample does not carry over");
    EntityPicture picture;
    picture.onEntityEvent(createEvent("RedUAV_N_01", 0.0));
    picture.onSample(sample("RedUAV_N_01", 100.0));
    picture.onEntityEvent(deleteEvent("RedUAV_N_01", 149.45, "destroyed"));
    picture.onEntityEvent(createEvent("RedUAV_N_01", 0.0));
    // Deliberately no sample yet. This is the whole point of the case.

    const PictureSnapshot snap = picture.snapshot();
    check(snap.isLive("RedUAV_N_01"), "the new occupancy is live");
    check(snap.liveSample("RedUAV_N_01") == nullptr,
          "it has published nothing yet, so there is no live sample to read");
    check(snap.lastKnownSample("RedUAV_N_01") == nullptr,
          "and the closed tenure's sample went with its occupancy rather than lingering");
}

void testScenarioUnloadTeardownProducesNoOrphans() {
    // The whole-roster churn M1 found and M3 measured: delete every entity with
    // scenario_unload, then immediately re-create all of them, then samples resume.
    beginCase("the scenario_unload teardown burst produces zero orphans");
    EntityPicture picture;
    std::vector<std::string> names;
    for (int i = 0; i < 20; ++i) {
        names.push_back("Entity_" + std::to_string(i));
    }
    for (const std::string& n : names) { picture.onEntityEvent(createEvent(n, 0.0)); }
    for (const std::string& n : names) { picture.onSample(sample(n, 50.0)); }
    for (const std::string& n : names) {
        picture.onEntityEvent(deleteEvent(n, 0.0, "scenario_unload"));
    }
    for (const std::string& n : names) { picture.onEntityEvent(createEvent(n, 0.0)); }
    for (const std::string& n : names) { picture.onSample(sample(n, 0.05)); }

    const PictureSnapshot snap = picture.snapshot();
    checkEq(snap.counters.samplesOrphaned, 0u, "zero orphans across a full teardown cycle");
    checkEq(snap.liveCount, std::size_t{20}, "all 20 live again after the reset");
    checkEq(snap.counters.entityCreated, 40u, "40 occupancies opened over 20 names");
    checkEq(snap.roster.size(), std::size_t{20}, "still 20 distinct names");
    checkEq(countOf(snap.removalsByReason, "scenario_unload"), 20u,
            "20 unload removals recorded");
}

void testVerbatimPayloadIsPreserved() {
    // BTB-CAP-4. The picture must not curate: a field this build has never heard of has to
    // survive into the latest-sample map, or the twelfth-field class of finding becomes
    // invisible and the capture silently drops data on the next schema change.
    beginCase("BTB-CAP-4: an unknown field survives verbatim into the latest sample");
    EntityPicture picture;
    picture.onEntityEvent(createEvent("e", 0.0));
    StreamValueMap values = sample("e", 1.0);
    values["activeAnimation"] = StreamValue(std::string("idle"));
    values["someFieldAddedNextRelease"] = StreamValue(static_cast<std::int64_t>(42));
    picture.onSample(values);

    const PictureSnapshot snap = picture.snapshot();
    const LatestSample* live = snap.liveSample("e");
    check(live != nullptr, "sample accepted");
    if (live != nullptr) {
        checkEq(live->values.size(), values.size(), "every field retained, none filtered");
        const auto anim = live->values.find("activeAnimation");
        check(anim != live->values.end(), "activeAnimation retained");
        const auto added = live->values.find("someFieldAddedNextRelease");
        check(added != live->values.end(), "a field no code knows about is retained");
        if (added != live->values.end()) {
            const std::int64_t* v = added->second.tryGet<std::int64_t>();
            check(v != nullptr && *v == 42, "and keeps its published value");
        }
    }
}

void testMalformedPayloadsAreCountedNotCrashed() {
    // A packed payload carries only the fields the publisher wrote. Every one of these is
    // counted under its own name so a stream that goes wrong says how.
    beginCase("absent fields are counted by cause, never assumed and never fatal");
    EntityPicture picture;
    picture.onEntityEvent(createEvent("e", 0.0));

    StreamValueMap noName = sample("e", 1.0);
    noName.erase("scenarioEntityName");
    picture.onSample(noName);

    StreamValueMap noTime = sample("e", 1.0);
    noTime.erase("simulationTime");
    picture.onSample(noTime);

    StreamValueMap noEventName = createEvent("e", 1.0);
    noEventName.erase("eventName");
    picture.onEntityEvent(noEventName);

    StreamValueMap noEntity = createEvent("e", 1.0);
    noEntity.erase("scenarioEntityName");
    picture.onEntityEvent(noEntity);

    picture.onEntityEvent(deleteEvent("never_created", 1.0, "destroyed"));

    StreamValueMap other = createEvent("e", 1.0);
    other["eventName"] = StreamValue(std::string("entity_updated"));
    picture.onEntityEvent(other);

    const PictureSnapshot snap = picture.snapshot();
    checkEq(snap.counters.samplesUnnamed, 1u, "sample with no entity name counted");
    checkEq(snap.counters.samplesUntimed, 1u, "sample with no simulation time counted");
    checkEq(snap.counters.eventsUnnamed, 1u, "event with no eventName counted");
    checkEq(snap.counters.eventsWithoutEntity, 1u, "event with no entity name counted");
    checkEq(snap.counters.deleteOfUnknownEntity, 1u, "delete of an unknown name counted");
    checkEq(snap.counters.samplesAccepted, 0u, "no malformed sample was accepted");
    checkEq(countOf(snap.unhandledEventNames, "entity_updated"), 1u,
            "an event this build does not handle is counted by name, not discarded");
}

void testDeterministicOrdering() {
    // BTB-EP-4 / R4. Insertion order must not leak into iteration order, or the capture
    // varies between runs and EXT-17's byte-for-byte self-test fails.
    beginCase("BTB-EP-4: iteration order is sorted, not insertion order");
    EntityPicture a;
    EntityPicture b;
    const std::vector<std::string> forward = {"alpha", "bravo", "charlie", "delta", "echo"};
    std::vector<std::string> reverse(forward.rbegin(), forward.rend());
    for (const std::string& n : forward) { a.onEntityEvent(createEvent(n, 0.0)); }
    for (const std::string& n : reverse) { b.onEntityEvent(createEvent(n, 0.0)); }

    std::vector<std::string> fromA;
    std::vector<std::string> fromB;
    for (const auto& e : a.snapshot().roster) { fromA.push_back(e.first); }
    for (const auto& e : b.snapshot().roster) { fromB.push_back(e.first); }
    check(fromA == forward, "roster iterates in sorted order regardless of arrival order");
    check(fromA == fromB, "two opposite insertion orders yield the identical iteration");
}

void testEventLogDrainAndBound() {
    beginCase("the roster-event log drains in FIFO order and its overflow is counted");
    EntityPicture picture;
    picture.onEntityEvent(createEvent("first", 1.0));
    picture.onEntityEvent(createEvent("second", 2.0));
    picture.onEntityEvent(deleteEvent("first", 3.0, "destroyed"));

    std::vector<RosterEvent> drained = picture.drainEvents();
    checkEq(drained.size(), std::size_t{3}, "three transitions drained");
    if (drained.size() == 3) {
        checkEq(drained[0].scenarioEntityName, std::string("first"), "FIFO: first event first");
        checkEq(drained[2].eventName, std::string("entity_deleted"), "FIFO: removal last");
        checkEq(drained[2].reason, std::string("destroyed"), "removal carries its reason");
    }
    check(picture.drainEvents().empty(), "draining twice yields nothing the second time");

    // Overflow: push well past capacity without a drain and confirm the loss is counted
    // rather than growing without bound.
    EntityPicture flooded;
    const std::size_t over = EntityPicture::kEventLogCapacity + 50;
    for (std::size_t i = 0; i < over; ++i) {
        flooded.onEntityEvent(createEvent("n" + std::to_string(i), 0.0));
    }
    const PictureSnapshot snap = flooded.snapshot();
    checkEq(snap.counters.eventQueueDropped, 50u, "exactly the overflow is counted");
    checkEq(flooded.drainEvents().size(), EntityPicture::kEventLogCapacity,
            "the log stays bounded at its capacity");
}

void testConcurrentHandlersAndSnapshots() {
    // The handlers run on the bus pump thread while our own thread snapshots. This does
    // not prove the absence of a race, but it does catch the obvious ones under a tool
    // and asserts that no update is lost under contention.
    beginCase("concurrent handler traffic and snapshots lose no update");
    EntityPicture picture;
    const int threads = 4;
    const int perThread = 2000;
    for (int t = 0; t < threads; ++t) {
        picture.onEntityEvent(createEvent("e" + std::to_string(t), 0.0));
    }

    std::atomic<bool> stop{false};
    std::thread reader([&picture, &stop] {
        while (!stop.load()) {
            const PictureSnapshot s = picture.snapshot();
            // Reading it is the point; the assertion is that this never tears or crashes.
            (void)s.liveCount;
        }
    });

    std::vector<std::thread> writers;
    for (int t = 0; t < threads; ++t) {
        writers.emplace_back([&picture, t, perThread] {
            const std::string name = "e" + std::to_string(t);
            for (int i = 0; i < perThread; ++i) {
                picture.onSample(sample(name, static_cast<double>(i) * 0.05));
            }
        });
    }
    for (std::thread& w : writers) { w.join(); }
    stop.store(true);
    reader.join();

    const PictureSnapshot snap = picture.snapshot();
    checkEq(snap.counters.samplesAccepted,
            static_cast<std::uint64_t>(threads) * static_cast<std::uint64_t>(perThread),
            "every sample from every thread was accounted for");
    checkEq(snap.counters.samplesOrphaned, 0u, "none lost to a torn roster read");
    checkEq(snap.liveCount, static_cast<std::size_t>(threads), "roster intact");
}

}  // namespace

int main() {
    std::printf("EXT-08 entity picture - unit tests (BTB-EP-3, BTB-EP-4, ADR-6)\n\n");

    testAcceptsSampleWithinOpenOccupancy();
    testSampleBeforeAnyCreateIsOrphaned();
    testEarlyAttachHasNoOrphansBeforeFirstSample();
    testLateAttachIsVisibleAtTheFirstAcceptedSample();
    testOrphansBeforeFirstAcceptedIsFrozen();
    testNoSampleAfterRemovalWithinOccupancy();
    testRemovalReasonVerbatim();
    testRecreatedNameOpensNextOccupancy();
    testRecreateDoesNotInheritPreviousSample();
    testScenarioUnloadTeardownProducesNoOrphans();
    testVerbatimPayloadIsPreserved();
    testMalformedPayloadsAreCountedNotCrashed();
    testDeterministicOrdering();
    testEventLogDrainAndBound();
    testConcurrentHandlersAndSnapshots();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
