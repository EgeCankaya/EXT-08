// EXT-08 Bus Telemetry Bridge - M6: geodetic distance and containment.
//
// BTB-REF-3 requires that "distance uses a stated geodetic method, documented in the README,
// so a result is reproducible by a third party". This header is that method, and it is
// deliberately the simplest one that is both exact and closed-form.
//
// **Positions are converted to earth-centred, earth-fixed (ECEF) coordinates on the WGS-84
// ellipsoid, and distance is the straight-line Euclidean distance between them in metres.**
//
// Why that and not the obvious alternatives:
//
//   haversine    Great-circle distance on a sphere. Ignores altitude entirely, which makes
//                it wrong for the question the referee is actually asked - "did these two
//                aircraft come within 5 km" is a question about two bodies in space, and two
//                aircraft stacked 6 km apart vertically are not close.
//   Vincenty     Ellipsoidal surface distance, iterative, and famously fails to converge for
//                near-antipodal pairs. It also answers the surface question, not the
//                three-dimensional one.
//   ECEF         Closed form, no iteration, no convergence case, handles altitude naturally,
//                and reproducible from this file's formulae alone by anyone with a
//                calculator. For distances at the scale a scenario cares about it is
//                indistinguishable from the ellipsoidal surface distance anyway.
//
// The platform publishes `positionGeodetic` as [latitude degrees, longitude degrees,
// altitude metres] and the capture records it verbatim, unconverted (BTB-CAP-4). Note the
// altitude caveat that follows the data rather than this code: where the host's geoid grid
// is absent - as it is on the development machine - altitudes are ellipsoidal rather than
// orthometric. That is exactly the datum ECEF wants, so the absence helps here; the README
// records it either way.

#pragma once

#include <array>
#include <vector>

namespace n8ro::bridge::geo {

// WGS-84, the datum the platform's geodetic positions are expressed in. Stated as constants
// rather than pulled from a library so that a third party reproducing a verdict has the
// exact numbers this program used.
constexpr double kSemiMajorAxisM = 6378137.0;
constexpr double kFlattening = 1.0 / 298.257223563;

// [latitude degrees, longitude degrees, altitude metres] - the platform's own order.
using Geodetic = std::array<double, 3>;
using Ecef = std::array<double, 3>;

[[nodiscard]] Ecef toEcef(const Geodetic& position);

// Straight-line distance in metres between two geodetic positions.
[[nodiscard]] double distanceM(const Geodetic& a, const Geodetic& b);

// Great-circle-free horizontal containment: is `point` inside the circle of `radiusM` about
// `centre`? Altitude is ignored for a circle - a circle on the ground is a cylinder in the
// air, which is what an operator means by "the corridor" - so the comparison uses both
// positions at the centre's altitude.
//
// **Boundary semantics: a point exactly on the boundary is INSIDE** (the comparison is <=).
// Stated because BTB-REF-3 requires the boundary case to be documented, and because a
// threshold test that is ambiguous at the threshold is untestable.
[[nodiscard]] bool insideCircle(const Geodetic& point, const Geodetic& centre, double radiusM);

// Is `point` inside the polygon, treating latitude and longitude as plane coordinates?
//
// Ray casting, with the ray along increasing longitude. Plane treatment is accurate for the
// scenario-sized regions this is used on and is stated rather than hidden: a polygon that
// spans the antimeridian or a pole is not supported, and the README says so. Altitude is
// ignored, for the same reason as the circle.
//
// **Boundary semantics: a point exactly on an edge or vertex is INSIDE.** Ray casting alone
// does not guarantee that, so the edge case is tested for explicitly before the crossing
// count - which also removes the parity ambiguity a vertex hit would otherwise create.
[[nodiscard]] bool insidePolygon(const Geodetic& point, const std::vector<Geodetic>& polygon);

}  // namespace n8ro::bridge::geo
