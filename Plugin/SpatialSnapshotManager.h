#pragma once

#include "Globals.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

struct PlayerSpatialSnapshot {
    RE::FormID playerFormId = 0;
    RE::FormID cellFormId = 0;
    RE::NiPoint3 playerPosition{};
    float visionRange = 0.0f;
    std::chrono::steady_clock::time_point timestamp{};
    std::string refreshReason;
    std::vector<AudibleActorDescriptor> audibleActors;
    std::vector<AudibleActorDescriptor> evaluatedActors;

    const AudibleActorDescriptor* FindAudible(RE::FormID formId) const;
    const AudibleActorDescriptor* FindEvaluated(RE::FormID formId) const;
    std::string Describe(const std::string& separator) const;
};

struct PlayerSpatialTargetStatus {
    bool hasTarget = false;
    std::string name;
    RE::FormID formId = 0;
    float distanceMeters = 0.0f;
    std::string source;
    std::string reason;
    std::string status;
    bool targetable = false;
};

struct PlayerSpatialCandidate {
    std::shared_ptr<AIAgent> agent;
    RE::Actor* actor = nullptr;
    std::string name;
    RE::FormID formId = 0;
    float airDistance = 0.0f;
    float distanceMeters = 0.0f;
    std::string source;
    std::string reason;
    std::string status;
    int sortBucket = 4;
    bool targetable = false;
    bool autoEligible = false;
    bool lookTarget = false;
    bool narrator = false;
};

class SpatialSnapshotManager {
public:
    static PlayerSpatialSnapshot GetPlayerSnapshot(bool forceRefresh = false,
                                                   const std::string& reason = "cache");
    static bool UpdatePlayerSnapshotIncremental(const std::string& reason = "incremental",
                                                std::size_t maxEvaluations = 1);
    static PlayerSpatialTargetStatus GetPlayerCrosshairTargetStatus(bool forceRefresh = false,
                                                                    const std::string& reason = "crosshair");
    static std::vector<PlayerSpatialCandidate> GetPlayerConversationTargets(
        const std::string& reason = "conversation_targets", bool includeUnavailable = true);
    static void InvalidatePlayerSnapshot();
    static void InvalidateForEnvironmentChange(
        std::chrono::milliseconds settleDuration = std::chrono::milliseconds(2000));
};
