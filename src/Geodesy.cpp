#include "Geodesy.h"

#include <cmath>

namespace n8ro::bridge::geo {
namespace {

constexpr double kPi = 3.14159265358979323846;

[[nodiscard]] double radians(double degrees) { return degrees * kPi / 180.0; }

// First eccentricity squared, from the flattening. e^2 = 2f - f^2.
[[nodiscard]] double eccentricitySquared() {
    return 2.0 * kFlattening - kFlattening * kFlattening;
}

// Distance from `point` to the segment ab, in the lat/lon plane. Used only for the polygon
// boundary test, where "on the edge" has to be decided before ray casting.
[[nodiscard]] double pointToSegment(double px, double py, double ax, double ay, double bx,
                                    double by) {
    const double dx = bx - ax;
    const double dy = by - ay;
    const double lengthSquared = dx * dx + dy * dy;
    if (lengthSquared == 0.0) {
        // A degenerate edge - two identical vertices. Distance to the point itself.
        const double ox = px - ax;
        const double oy = py - ay;
        return std::sqrt(ox * ox + oy * oy);
    }
    double t = ((px - ax) * dx + (py - ay) * dy) / lengthSquared;
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    const double cx = ax + t * dx;
    const double cy = ay + t * dy;
    const double ox = px - cx;
    const double oy = py - cy;
    return std::sqrt(ox * ox + oy * oy);
}

// How close to an edge counts as on it. In degrees, so about a tenth of a millimetre of
// latitude - far below any position the platform publishes and far above the rounding of a
// double, which is what makes the boundary rule decidable rather than luck.
constexpr double kEdgeToleranceDeg = 1e-9;

}  // namespace

Ecef toEcef(const Geodetic& position) {
    const double lat = radians(position[0]);
    const double lon = radians(position[1]);
    const double alt = position[2];

    const double sinLat = std::sin(lat);
    const double cosLat = std::cos(lat);
    const double e2 = eccentricitySquared();

    // Radius of curvature in the prime vertical.
    const double n = kSemiMajorAxisM / std::sqrt(1.0 - e2 * sinLat * sinLat);

    return Ecef{(n + alt) * cosLat * std::cos(lon),
                (n + alt) * cosLat * std::sin(lon),
                (n * (1.0 - e2) + alt) * sinLat};
}

double distanceM(const Geodetic& a, const Geodetic& b) {
    const Ecef pa = toEcef(a);
    const Ecef pb = toEcef(b);
    const double dx = pa[0] - pb[0];
    const double dy = pa[1] - pb[1];
    const double dz = pa[2] - pb[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool insideCircle(const Geodetic& point, const Geodetic& centre, double radiusM) {
    // Both at the centre's altitude, so the region is a cylinder rather than a sphere: an
    // operator asking "is it in the corridor" is asking a horizontal question.
    const Geodetic flattened{point[0], point[1], centre[2]};
    return distanceM(flattened, centre) <= radiusM;
}

bool insidePolygon(const Geodetic& point, const std::vector<Geodetic>& polygon) {
    if (polygon.size() < 3) {
        // Not a region. Nothing is inside it, and the condition loader rejects such a polygon
        // at parse time so this is defence in depth rather than the diagnosis.
        return false;
    }

    const double py = point[0];   // latitude
    const double px = point[1];   // longitude

    // The boundary first, so that a point on an edge or a vertex is inside by the stated rule
    // rather than by whichever way the crossing count happened to fall.
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        if (pointToSegment(px, py, polygon[j][1], polygon[j][0], polygon[i][1],
                           polygon[i][0]) <= kEdgeToleranceDeg) {
            return true;
        }
    }

    // Ray casting along increasing longitude.
    bool inside = false;
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const double yi = polygon[i][0];
        const double xi = polygon[i][1];
        const double yj = polygon[j][0];
        const double xj = polygon[j][1];
        if ((yi > py) != (yj > py)) {
            const double crossing = (xj - xi) * (py - yi) / (yj - yi) + xi;
            if (px < crossing) {
                inside = !inside;
            }
        }
    }
    return inside;
}

}  // namespace n8ro::bridge::geo
