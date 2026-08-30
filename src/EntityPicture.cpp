#include "EntityPicture.h"

#include <messaging/EventNames.h>

#include <utility>

namespace n8ro::bridge {

std::optional<std::string> tryReadString(
    const n8ro::sim::StreamValueMap& values, const std::string& field) {
    const auto it = values.find(field);   // a lookup, not an iteration
    if (it == values.end()) {
        return std::nullopt;
    }
    if (const std::string* text = it->second.tryGet<std::string>()) {
        return *text;
    }
    return std::nullopt;
}

std::optional<double> tryReadDouble(
    const n8ro::sim::StreamValueMap& values, const std::string& field) {
    const auto it = values.find(field);
    if (it == values.end()) {
        return std::nullopt;
    }
    if (const double* number = it->second.tryGet<double>()) {
        return *number;
    }
    // A schema may declare an integral field the referee still wants as a number. Widening
    // here is lossless for anything the bus carries and keeps a caller from having to ask
    // twice; it is a read convenience only, and nothing durable is written from it.
    if (const std::int64_t* integer = it->second.tryGet<std::int64_t>()) {
        return static_cast<double>(*integer);
    }
    return std::nullopt;
}

void EntityPicture::pushEventLocked(RosterEvent event) {
    if (eventLog_.size() >= kEventLogCapacity) {
        // Drop the oldest and count it. Bounded memory beats an unbounded queue in a
        // process that must not fall over; the count is what keeps the loss honest.
        eventLog_.pop_front();
        ++counters_.eventQueueDropped;
    }
    eventLog_.push_back(std::move(event));
}

SampleOutcome EntityPicture::onSample(const n8ro::sim::StreamValueMap& values) {
    // Read the two keys we need before taking the lock. Everything else stays verbatim.
    const std::optional<std::string> name = tryReadString(values, "scenarioEntityName");
    const std::optional<double> simTime = tryReadDouble(values, "simulationTime");

    const std::lock_guard<std::mutex> guard(mutex_);

    if (!name) {
        // Cannot be keyed, so it cannot enter the map. Counted rather than dropped quietly.
        ++counters_.samplesUnnamed;
        return SampleOutcome{};
    }
    if (!simTime) {
        // A sample with no clock of its own has nothing durable to be stamped with, and
        // wall-clock is not a substitute (tenet 2).
        ++counters_.samplesUntimed;
        return SampleOutcome{};
    }

    const auto rosterEntry = roster_.find(*name);
    const bool hasOpenOccupancy = rosterEntry != roster_.end() && rosterEntry->second.open;
    if (!hasOpenOccupancy) {
        // The real BTB-EP-3 violation: a sample for an entity with no open tenure. A
        // re-create under the same name opens a new occupancy first, so the scenario_unload
        // churn M1 found does not land here.
        ++counters_.samplesOrphaned;
        return SampleOutcome{};
    }

    LatestSample& entry = latest_[*name];
    entry.simulationTimeS = *simTime;
    entry.generation = rosterEntry->second.generation;
    entry.values = values;   // verbatim copy; the courier's whole job
    ++counters_.samplesAccepted;

    return SampleOutcome{true, *name, entry.generation, *simTime};
}

void EntityPicture::onEntityEvent(const n8ro::sim::StreamValueMap& values) {
    const std::optional<std::string> eventName = tryReadString(values, "eventName");
    const std::optional<std::string> name = tryReadString(values, "scenarioEntityName");
    const std::optional<std::string> reason = tryReadString(values, "reason");
    const std::optional<std::string> profile = tryReadString(values, "profileName");
    const std::optional<std::string> team = tryReadString(values, "teamName");
    const std::optional<double> simTime = tryReadDouble(values, "simulationTime");

    const std::lock_guard<std::mutex> guard(mutex_);

    if (!eventName) {
        ++counters_.eventsUnnamed;
        return;
    }
    if (!name) {
        // An event on the entity-event topic that names no entity. Counted on its own
        // rather than folded into the unhandled-name tally, which would misreport a
        // known event with a missing field as an unknown event.
        ++counters_.eventsWithoutEntity;
        return;
    }

    const double eventTime = simTime.value_or(0.0);

    if (*eventName == n8ro::sim::kEventEntityCreated) {
        Occupancy& entry = roster_[*name];
        // A name reused after a delete opens the next occupancy rather than resurrecting
        // the last one. First sighting of a name lands on a default-constructed entry with
        // generation 0, so this is the same increment in both cases.
        ++entry.generation;
        entry.open = true;
        entry.profileName = profile.value_or(std::string{});
        entry.teamName = team.value_or(std::string{});
        entry.lastRemovalReason.clear();
        entry.createdSimTimeS = eventTime;
        entry.removedSimTimeS = 0.0;
        ++counters_.entityCreated;

        // The previous occupancy's last sample belongs to the previous occupancy. Dropping
        // it here is what makes "no sample after the removal" true within an occupancy.
        latest_.erase(*name);

        pushEventLocked(RosterEvent{*eventName, *name, std::string{}, eventTime, entry.generation});
        return;
    }

    if (*eventName == n8ro::sim::kEventEntityDeleted) {
        const auto entry = roster_.find(*name);
        if (entry == roster_.end()) {
            ++counters_.deleteOfUnknownEntity;
            return;
        }
        entry->second.open = false;
        // Verbatim, including a supplier-specific value outside the engine's own set. The
        // roster never coerces a reason it does not recognise, and never drops one.
        entry->second.lastRemovalReason = reason.value_or(std::string{});
        entry->second.removedSimTimeS = eventTime;
        ++counters_.entityDeleted;
        ++removalsByReason_[entry->second.lastRemovalReason];

        pushEventLocked(RosterEvent{*eventName, *name, entry->second.lastRemovalReason,
                                    eventTime, entry->second.generation});
        return;
    }

    // entity_updated, and anything a scenario or plugin author added to the vocabulary
    // without this build knowing about it. Counted by name so the notes deliverable can
    // say what the stream carried that we did not expect.
    ++unhandledEventNames_[*eventName];
}

bool PictureSnapshot::isLive(const std::string& name) const {
    const auto entry = roster.find(name);
    return entry != roster.end() && entry->second.open;
}

const LatestSample* PictureSnapshot::liveSample(const std::string& name) const {
    if (!isLive(name)) {
        return nullptr;
    }
    return lastKnownSample(name);
}

const LatestSample* PictureSnapshot::lastKnownSample(const std::string& name) const {
    const auto entry = latest.find(name);
    return entry == latest.end() ? nullptr : &entry->second;
}

PictureSnapshot EntityPicture::snapshot() const {
    const std::lock_guard<std::mutex> guard(mutex_);

    PictureSnapshot out;
    out.roster = roster_;
    out.latest = latest_;
    out.removalsByReason = removalsByReason_;
    out.unhandledEventNames = unhandledEventNames_;
    out.counters = counters_;
    for (const auto& entry : out.roster) {   // std::map - ordered iteration
        if (entry.second.open) {
            ++out.liveCount;
        }
    }
    return out;
}

std::vector<RosterEvent> EntityPicture::drainEvents() {
    const std::lock_guard<std::mutex> guard(mutex_);
    std::vector<RosterEvent> out(
        std::make_move_iterator(eventLog_.begin()), std::make_move_iterator(eventLog_.end()));
    eventLog_.clear();
    return out;
}

std::size_t EntityPicture::liveCount() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    std::size_t live = 0;
    for (const auto& entry : roster_) {
        if (entry.second.open) {
            ++live;
        }
    }
    return live;
}

}  // namespace n8ro::bridge
