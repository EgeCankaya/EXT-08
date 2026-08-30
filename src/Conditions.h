// EXT-08 Bus Telemetry Bridge - M6: conditions declared outside the code (BTB-REF-1,
// BTB-REF-3).
//
// A condition file is JSON. Adding or changing a condition requires no rebuild, which is the
// whole requirement: conditions compiled into the binary cannot be re-applied to a stored
// run, and re-applying them to a stored run is what makes the capture worth keeping.
//
// **The vocabulary is closed at three kinds** and a fourth is a named parse error, never a
// silent skip. BTB-REF-3 closes it and the PRD's Out-of-Scope table says why: a general
// expression language is a parser, and a parser is a project. A fourth kind requires a PRD
// revision, which is the point of closing it.
//
//   proximity       two named entities within a distance threshold
//   area            one named entity inside or outside a declared geodetic region
//   terminal_state  one named entity reaching a removal reason, or a declared field value
//
// File shape:
//
//   {
//     "conditions": [
//       {"id": "red-reaches-base",   "kind": "proximity",
//        "entities": ["RedUAV_N_01", "BlueBase_Airfield"], "within_m": 5000},
//
//       {"id": "red-in-corridor",    "kind": "area", "entity": "RedUAV_N_01",
//        "test": "inside",
//        "region": {"shape": "circle", "centre": [-23.45, -68.25, 400], "radius_m": 20000}},
//
//       {"id": "leader-survives",    "kind": "terminal_state", "entity": "RedUAV_N_01",
//        "removal_reason": "destroyed"}
//     ]
//   }
//
// Every value the platform publishes is used verbatim and unconverted: distances are metres,
// angles are degrees, and the geodetic order is the platform's own [lat, lon, alt]
// (BTB-CAP-4, and section 15 of the format spec).

#pragma once

#include "Geodesy.h"

#include <cstdint>
#include <string>
#include <vector>

namespace n8ro::bridge {

enum class ConditionKind {
    Proximity,
    Area,
    TerminalState,
};

[[nodiscard]] const char* conditionKindName(ConditionKind kind);

enum class RegionShape {
    Circle,
    Polygon,
};

enum class AreaTest {
    Inside,
    Outside,
};

struct Region {
    RegionShape shape = RegionShape::Circle;
    geo::Geodetic centre{};          // Circle
    double radiusM = 0.0;            // Circle
    std::vector<geo::Geodetic> polygon;   // Polygon
};

// One declared condition. `id` is the stable identifier that appears in its verdict, so a
// verdict can be traced back to the declaration that produced it (BTB-REF-1).
struct Condition {
    std::string id;
    ConditionKind kind = ConditionKind::Proximity;

    // Proximity.
    std::string entityA;
    std::string entityB;
    double withinM = 0.0;

    // Area.
    std::string entity;
    AreaTest test = AreaTest::Inside;
    Region region;

    // TerminalState. Exactly one of these is set, checked at load.
    std::string removalReason;   // matched against entity_remove.reason, verbatim
    std::string fieldName;       // matched against a sample field
    std::string fieldEquals;
};

// Loads and validates a condition file. Returns false with a named error on any problem -
// a malformed file, an unrecognised kind, a missing parameter, a duplicate id, or a polygon
// with fewer than three vertices.
//
// BTB-REF-1 requires this to fail **before any subscription is made**, never a silent
// zero-condition run: a campaign that reports "all conditions passed" because it loaded none
// is worse than one that refuses to start.
[[nodiscard]] bool loadConditions(const std::string& path, std::vector<Condition>& out,
                                  std::string& error);

// Parses an already-read document. Split out so the tests can drive it without a file.
[[nodiscard]] bool parseConditions(const std::string& text, std::vector<Condition>& out,
                                   std::string& error);

}  // namespace n8ro::bridge
