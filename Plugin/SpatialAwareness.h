#pragma once

#include <string>

#include "RE/Skyrim.h"

namespace SpatialAwareness
{
    struct Settings
    {
        float maxAirDistance = 4000.0f;
        float immediateDistance = 150.0f;
        // Tuned so clear-line practical audibility (volume >= 0.15) is roughly:
        // - indoors:  ~22.5 ft (~750 units)
        // - outdoors: ~37.5 ft (~1250 units)
        float interiorMaxDistance = 750.0f;
        float exteriorMaxDistance = 1250.0f;
        float minDistanceFactor = 0.1f;
        float interiorBaseModifier = 1.0f;
        float exteriorBaseModifier = 0.7f;
        float openDoorPenaltyBase = 0.85f;
        float aroundCornerPenalty = 0.60f;
        float minimumAudibleVolume = 0.15f;
        float doorTriangulationPercentTolerance = 0.20f;
        float doorTriangulationAbsoluteTolerance = 80.0f;
        float pathRatioReject = 4.0f;
        float pathRatioDistanceReject = 2.5f;
        float pathRatioDistanceRejectMinAir = 500.0f;
        float pathComplexityStartRatio = 1.2f;
        float pathComplexityScale = 0.6f;
        float pathComplexityMin = 0.3f;
        float navmeshSnapDistance = 450.0f;
    };

    struct Result
    {
        bool canCommunicate = false;
        float volume = 0.0f;
        float airDistance = 0.0f;
        float pathDistance = -1.0f;
        float pathRatio = -1.0f;
        int detectionLevel = -999;
        int openDoorCount = 0;
        int closedDoorCount = 0;
        bool navmeshPathUsed = false;
        bool navmeshPathFound = false;
        bool losFallbackUsed = false;
        bool hasLineOfSight = false;
        std::string reason = "unknown";
    };

    enum class PathStatus
    {
        kUnavailable,
        kNoPath,
        kSuccess
    };

    struct PathResult
    {
        PathStatus status = PathStatus::kUnavailable;
        float airDistance = 0.0f;
        float pathDistance = -1.0f;
    };

    Settings GetSettings();
    void SetInteriorMaxDistance(float interiorMaxDistance);
    void SetExteriorMaxDistance(float exteriorMaxDistance);

    PathResult EvaluatePath(RE::Actor* speaker, RE::Actor* listener, const Settings& settings = GetSettings());
    Result Evaluate(RE::Actor* speaker, RE::Actor* listener, const Settings& settings = GetSettings());
}
