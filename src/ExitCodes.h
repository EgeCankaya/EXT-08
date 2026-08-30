// EXT-08 Bus Telemetry Bridge - process exit codes.
//
// Distinct values so a script can tell a bad invocation from a bad configuration from a
// schema mismatch without scraping the log. Every non-zero code has a named diagnostic
// logged before it is returned; none is ever returned silently.

#pragma once

namespace n8ro::bridge {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;                    // unknown option, missing value, missing required
constexpr int kExitCreateFailed = 3;             // SimulationEngineClient::create() -> nullopt
constexpr int kExitUnexpected = 4;               // an exception reached main
constexpr int kExitModelOpenFailed = 5;          // DbModel::Open() failed
constexpr int kExitSchemaLoadFailed = 6;         // loadAllFromDb() failed
constexpr int kExitRegistryEmpty = 7;            // BTB-EP-1: the loud empty registry
constexpr int kExitEntityStateUnresolved = 8;    // no schema for the entity-state message name
constexpr int kExitEntityStateShapeWrong = 9;    // resolved, but not shaped like entity state
constexpr int kExitEntityEventUnresolved = 10;   // entity-event topic unresolvable; no roster
constexpr int kExitSubscribeFailed = 11;         // subscribeByTopic() returned no subscription
constexpr int kExitNoMessageBus = 12;            // client exposed no message bus

}  // namespace n8ro::bridge
