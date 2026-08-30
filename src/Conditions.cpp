#include "Conditions.h"

#include "JsonParse.h"

#include <fstream>
#include <set>
#include <sstream>

namespace n8ro::bridge {
namespace {

// Reads a [lat, lon, alt] triple. Altitude may be omitted and defaults to 0, because a
// horizontal region does not need one and requiring it would be ceremony.
[[nodiscard]] bool readGeodetic(const json::Value& value, const std::string& what,
                                geo::Geodetic& out, std::string& error) {
    if (value.kind != json::Value::Kind::Array || value.elements.size() < 2 ||
        value.elements.size() > 3) {
        error = what + " must be [latitude, longitude] or [latitude, longitude, altitude]";
        return false;
    }
    out = geo::Geodetic{0.0, 0.0, 0.0};
    for (std::size_t i = 0; i < value.elements.size(); ++i) {
        if (value.elements[i]->kind != json::Value::Kind::Number) {
            error = what + " element " + std::to_string(i) + " is not a number";
            return false;
        }
        out[i] = value.elements[i]->number;
    }
    if (out[0] < -90.0 || out[0] > 90.0) {
        error = what + " latitude " + std::to_string(out[0]) + " is outside [-90, 90]";
        return false;
    }
    if (out[1] < -180.0 || out[1] > 180.0) {
        error = what + " longitude " + std::to_string(out[1]) + " is outside [-180, 180]";
        return false;
    }
    return true;
}

[[nodiscard]] bool readRegion(const json::Value& object, const std::string& id, Region& out,
                              std::string& error) {
    const json::Value* region = object.findOfKind("region", json::Value::Kind::Object);
    if (region == nullptr) {
        error = "condition \"" + id + "\": area needs a \"region\" object";
        return false;
    }
    std::string shape;
    if (!region->getString("shape", shape)) {
        error = "condition \"" + id + "\": region needs a \"shape\" of circle or polygon";
        return false;
    }

    if (shape == "circle") {
        out.shape = RegionShape::Circle;
        const json::Value* centre = region->find("centre");
        if (centre == nullptr) {
            centre = region->find("center");   // both spellings, so neither is a trap
        }
        if (centre == nullptr) {
            error = "condition \"" + id + "\": circle region needs \"centre\"";
            return false;
        }
        if (!readGeodetic(*centre, "condition \"" + id + "\": region centre", out.centre,
                          error)) {
            return false;
        }
        if (!region->getNumber("radius_m", out.radiusM) || out.radiusM <= 0.0) {
            error = "condition \"" + id + "\": circle region needs a positive \"radius_m\"";
            return false;
        }
        return true;
    }

    if (shape == "polygon") {
        out.shape = RegionShape::Polygon;
        const json::Value* vertices = region->findOfKind("vertices", json::Value::Kind::Array);
        if (vertices == nullptr) {
            error = "condition \"" + id + "\": polygon region needs a \"vertices\" array";
            return false;
        }
        if (vertices->elements.size() < 3) {
            error = "condition \"" + id + "\": a polygon needs at least three vertices, got " +
                    std::to_string(vertices->elements.size());
            return false;
        }
        for (const json::ValuePtr& vertex : vertices->elements) {
            geo::Geodetic point{};
            if (!readGeodetic(*vertex, "condition \"" + id + "\": polygon vertex", point,
                              error)) {
                return false;
            }
            out.polygon.push_back(point);
        }
        return true;
    }

    error = "condition \"" + id + "\": region shape \"" + shape +
            "\" is not one of circle, polygon";
    return false;
}

[[nodiscard]] bool readOne(const json::Value& object, Condition& out, std::string& error) {
    if (object.kind != json::Value::Kind::Object) {
        error = "every entry in \"conditions\" must be an object";
        return false;
    }
    if (!object.getString("id", out.id) || out.id.empty()) {
        error = "every condition needs a non-empty \"id\"; it is what its verdict is traced by";
        return false;
    }
    std::string kind;
    if (!object.getString("kind", kind)) {
        error = "condition \"" + out.id + "\" has no \"kind\"";
        return false;
    }

    if (kind == "proximity") {
        out.kind = ConditionKind::Proximity;
        const json::Value* entities = object.findOfKind("entities", json::Value::Kind::Array);
        if (entities == nullptr || entities->elements.size() != 2 ||
            entities->elements[0]->kind != json::Value::Kind::String ||
            entities->elements[1]->kind != json::Value::Kind::String) {
            error = "condition \"" + out.id +
                    "\": proximity needs \"entities\" as exactly two entity-name strings";
            return false;
        }
        out.entityA = entities->elements[0]->text;
        out.entityB = entities->elements[1]->text;
        if (out.entityA == out.entityB) {
            error = "condition \"" + out.id +
                    "\": proximity names the same entity twice, which is always met at "
                    "distance zero and is certainly not what was meant";
            return false;
        }
        if (!object.getNumber("within_m", out.withinM) || out.withinM < 0.0) {
            error = "condition \"" + out.id +
                    "\": proximity needs a non-negative \"within_m\" in metres";
            return false;
        }
        return true;
    }

    if (kind == "area") {
        out.kind = ConditionKind::Area;
        if (!object.getString("entity", out.entity) || out.entity.empty()) {
            error = "condition \"" + out.id + "\": area needs an \"entity\"";
            return false;
        }
        std::string test;
        if (!object.getString("test", test)) {
            test = "inside";   // the common case; stated in the README as the default
        }
        if (test == "inside") {
            out.test = AreaTest::Inside;
        } else if (test == "outside") {
            out.test = AreaTest::Outside;
        } else {
            error = "condition \"" + out.id + "\": area \"test\" must be inside or outside, got " +
                    test;
            return false;
        }
        return readRegion(object, out.id, out.region, error);
    }

    if (kind == "terminal_state") {
        out.kind = ConditionKind::TerminalState;
        if (!object.getString("entity", out.entity) || out.entity.empty()) {
            error = "condition \"" + out.id + "\": terminal_state needs an \"entity\"";
            return false;
        }
        const bool hasReason = object.getString("removal_reason", out.removalReason);
        const bool hasField = object.getString("field", out.fieldName);
        const bool hasEquals = object.getString("equals", out.fieldEquals);
        if (hasReason && (hasField || hasEquals)) {
            error = "condition \"" + out.id +
                    "\": terminal_state takes either \"removal_reason\" or \"field\"+\"equals\", "
                    "not both";
            return false;
        }
        if (hasField != hasEquals) {
            error = "condition \"" + out.id +
                    "\": terminal_state's \"field\" and \"equals\" go together";
            return false;
        }
        if (!hasReason && !hasField) {
            error = "condition \"" + out.id +
                    "\": terminal_state needs either \"removal_reason\" or \"field\"+\"equals\"";
            return false;
        }
        return true;
    }

    // The closed vocabulary. A fourth kind is a named parse error and a non-zero exit, never
    // a silently skipped condition - a run that reports "all passed" because it quietly
    // dropped the one that mattered is the failure BTB-REF-3 exists to prevent.
    error = "condition \"" + out.id + "\": kind \"" + kind +
            "\" is not one of proximity, area, terminal_state. The vocabulary is closed at "
            "three kinds in n8ro-capture/1 (BTB-REF-3); a fourth is a PRD revision, not a "
            "configuration change";
    return false;
}

}  // namespace

const char* conditionKindName(ConditionKind kind) {
    switch (kind) {
        case ConditionKind::Proximity:     return "proximity";
        case ConditionKind::Area:          return "area";
        case ConditionKind::TerminalState: return "terminal_state";
    }
    return "unknown";
}

bool parseConditions(const std::string& text, std::vector<Condition>& out, std::string& error) {
    out.clear();
    json::ValuePtr document;
    if (!json::parse(text, document, error)) {
        error = "condition file is not valid JSON: " + error;
        return false;
    }
    const json::Value* conditions = document->findOfKind("conditions", json::Value::Kind::Array);
    if (conditions == nullptr) {
        error = "condition file needs a top-level \"conditions\" array";
        return false;
    }
    if (conditions->elements.empty()) {
        // Not an error the parser has to reject, but it is one the caller must not treat as
        // success by accident, so it is named here rather than discovered at the verdict file.
        error = "condition file declares no conditions. A run that evaluates nothing and "
                "reports nothing is indistinguishable from one where everything passed";
        return false;
    }

    std::set<std::string> seen;
    for (const json::ValuePtr& entry : conditions->elements) {
        Condition condition;
        if (!readOne(*entry, condition, error)) {
            return false;
        }
        if (!seen.insert(condition.id).second) {
            error = "duplicate condition id \"" + condition.id +
                    "\"; ids are how a verdict is traced back to its declaration and must be "
                    "unique";
            return false;
        }
        out.push_back(std::move(condition));
    }
    return true;
}

bool loadConditions(const std::string& path, std::vector<Condition>& out, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "could not open condition file: " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    if (!file && !file.eof()) {
        error = "could not read condition file: " + path;
        return false;
    }
    return parseConditions(buffer.str(), out, error);
}

}  // namespace n8ro::bridge
