#pragma once

#include <cmath>

namespace SpatialGeometryPolicy
{
    struct Point3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    // Limit door candidates to a narrow corridor along the actor-to-actor segment.
    inline bool IsPointWithinSegmentCorridor(const Point3& start, const Point3& end,
                                             const Point3& candidate, float corridorHalfWidth)
    {
        if (!std::isfinite(corridorHalfWidth) || corridorHalfWidth < 0.0f) {
            return false;
        }

        const Point3 segment{end.x - start.x, end.y - start.y, end.z - start.z};
        const float segmentLengthSquared =
            segment.x * segment.x + segment.y * segment.y + segment.z * segment.z;
        if (!std::isfinite(segmentLengthSquared) || segmentLengthSquared <= 0.001f) {
            return false;
        }

        const Point3 toCandidate{
            candidate.x - start.x, candidate.y - start.y, candidate.z - start.z};
        const float projection =
            (toCandidate.x * segment.x + toCandidate.y * segment.y + toCandidate.z * segment.z) /
            segmentLengthSquared;
        if (!std::isfinite(projection) || projection <= 0.0f || projection >= 1.0f) {
            return false;
        }

        const Point3 closest{
            start.x + segment.x * projection,
            start.y + segment.y * projection,
            start.z + segment.z * projection};
        const float deltaX = candidate.x - closest.x;
        const float deltaY = candidate.y - closest.y;
        const float deltaZ = candidate.z - closest.z;
        const float perpendicularDistanceSquared =
            deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ;
        const float corridorHalfWidthSquared = corridorHalfWidth * corridorHalfWidth;
        return std::isfinite(perpendicularDistanceSquared) &&
               perpendicularDistanceSquared <= corridorHalfWidthSquared;
    }
}
