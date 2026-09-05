#include "SpatialSnapshotManager.h"

#include "PlayerConversationRouter.h"
#include "SpatialAwareness.h"
#include "SpatialGeometryPolicy.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "RE/B/BGSOpenCloseForm.h"

namespace logger = SKSE::log;

namespace
{
    std::mutex g_playerSnapshotMutex;
    PlayerSpatialSnapshot g_playerSnapshot;
    struct IncrementalSnapshotCandidate {
        RE::FormID formId = 0;
        float airDistance = 0.0f;
    };

    std::vector<IncrementalSnapshotCandidate> g_incrementalCandidates;
    std::size_t g_incrementalCandidateIndex = 0;
    bool g_incrementalScanComplete = true;
    RE::NiPoint3 g_incrementalAnchorPosition{};
    RE::FormID g_incrementalAnchorCellFormId = 0;
    float g_incrementalAnchorEffectiveVisionRange = 0.0f;
    constexpr float kIncrementalSnapshotMoveTolerance = 384.0f;

    std::mutex g_playerCrosshairTargetMutex;
    PlayerSpatialTargetStatus g_playerCrosshairTarget;
    RE::FormID g_playerCrosshairTargetFormId = 0;
    std::chrono::steady_clock::time_point g_playerCrosshairTargetAt{};
    constexpr auto kPlayerSnapshotTtl = std::chrono::seconds(5);
    constexpr auto kPlayerSnapshotMaxStaleReadTtl = std::chrono::seconds(15);
    constexpr auto kIncrementalSnapshotRescanInterval = std::chrono::seconds(2);
    constexpr float kPlayerSnapshotMoveTolerance = 128.0f;
    // Prisma polls target state at 250ms. Keep these TTLs above that cadence so
    // UI reads do not rebuild/fallback-scan the managed actor list every pulse.
    constexpr auto kPlayerCrosshairTargetTtl = std::chrono::milliseconds(250);
    constexpr auto kPlayerConversationTargetsTtl = std::chrono::milliseconds(250);
    constexpr float kPlayerConversationTargetsMoveTolerance = 48.0f;
    constexpr auto kCellEntrySpatialSettleTime = std::chrono::seconds(2);
    constexpr auto kPositiveTargetRefinementTtl = std::chrono::seconds(10);
    constexpr auto kNegativeTargetRefinementTtl = std::chrono::milliseconds(1200);
    constexpr auto kClosedDoorTargetRefinementTtl = std::chrono::seconds(10);
    constexpr auto kPriorityLosRefinementMinInterval = std::chrono::milliseconds(250);
    constexpr auto kBackgroundLosRefinementMinInterval = std::chrono::milliseconds(500);
    constexpr auto kPriorityPathFallbackMinInterval = std::chrono::milliseconds(500);
    constexpr auto kClosePathFallbackMinInterval = std::chrono::milliseconds(750);
    constexpr auto kPathFallbackMinInterval = std::chrono::milliseconds(2500);
    constexpr auto kDistantPathFallbackMinInterval = std::chrono::milliseconds(4000);
    constexpr float kTargetRefinementMoveTolerance = 64.0f;
    constexpr float kDifferentLevelVerticalDelta = 160.0f;
    constexpr float kSkyrimUnitsToMeters = 1.0f / SpatialAwareness::kSkyrimUnitsPerMeter;
    constexpr std::size_t kTargetRefinementCacheMaxEntries = 64;

    RE::FormID g_observedPlayerCellFormId = 0;
    std::chrono::steady_clock::time_point g_observedPlayerCellAt{};
    std::chrono::steady_clock::time_point g_environmentSpatialSettleUntil{};

    std::mutex g_playerConversationTargetsMutex;
    std::vector<PlayerSpatialCandidate> g_playerConversationTargetsCache;
    std::chrono::steady_clock::time_point g_playerConversationTargetsAt{};
    RE::FormID g_playerConversationTargetsCellFormId = 0;
    RE::NiPoint3 g_playerConversationTargetsPlayerPosition{};
    bool g_playerConversationTargetsIncludeUnavailable = true;
    bool g_playerConversationTargetsCacheValid = false;

    struct TargetRefinementCacheEntry {
        RE::FormID playerFormId = 0;
        RE::FormID targetFormId = 0;
        RE::FormID cellFormId = 0;
        RE::NiPoint3 playerPosition{};
        RE::NiPoint3 targetPosition{};
        std::chrono::steady_clock::time_point timestamp{};
        SpatialAwareness::Result result{};
    };

    std::mutex g_targetRefinementMutex;
    std::unordered_map<std::uint64_t, TargetRefinementCacheEntry> g_targetRefinementCache;
    bool g_targetRefinementTaskQueued = false;
    std::chrono::steady_clock::time_point g_nextPriorityLosRefinementAt{};
    std::chrono::steady_clock::time_point g_nextBackgroundLosRefinementAt{};
    std::chrono::steady_clock::time_point g_nextPriorityPathFallbackAt{};
    std::chrono::steady_clock::time_point g_nextPathFallbackAt{};

    std::uint64_t TargetRefinementKey(RE::FormID playerFormId, RE::FormID targetFormId)
    {
        return (static_cast<std::uint64_t>(playerFormId) << 32U) ^
               static_cast<std::uint64_t>(targetFormId);
    }

    struct DoorBarrierScan {
        int openDoorCount = 0;
        int closedDoorCount = 0;
        RE::FormID closedDoorCandidateFormId = 0;
    };

    bool IsClosedDoorState(RE::BGSOpenCloseForm::OPEN_STATE state)
    {
        return state == RE::BGSOpenCloseForm::OPEN_STATE::kClosed ||
               state == RE::BGSOpenCloseForm::OPEN_STATE::kClosing;
    }

    bool IsOpenDoorState(RE::BGSOpenCloseForm::OPEN_STATE state)
    {
        return state == RE::BGSOpenCloseForm::OPEN_STATE::kOpen ||
               state == RE::BGSOpenCloseForm::OPEN_STATE::kOpening;
    }

    std::string ActorLabel(RE::Actor* actor)
    {
        if (!actor) {
            return {};
        }

        const char* name = actor->GetDisplayFullName();
        if (name && name[0] != '\0') {
            return name;
        }

        return std::format("form_{:08X}", actor->GetFormID());
    }

    DoorBarrierScan ScanDoorBarrierBetween(RE::Actor* player, RE::Actor* target,
                                           const SpatialAwareness::Settings& settings)
    {
        DoorBarrierScan scan{};
        if (!player || !target) {
            return scan;
        }

        auto* playerCell = player->GetParentCell();
        auto* targetCell = target->GetParentCell();
        if (!playerCell || !targetCell || !playerCell->IsInteriorCell() || playerCell != targetCell) {
            return scan;
        }

        const auto playerPosition = SpatialAwareness::GetEffectiveActorPosition(player);
        const auto targetPosition = SpatialAwareness::GetEffectiveActorPosition(target);
        const float airDistance = playerPosition.GetDistance(targetPosition);
        if (!std::isfinite(airDistance) || airDistance <= 0.001f) {
            return scan;
        }

        const float corridorHalfWidth = std::max(settings.doorTriangulationAbsoluteTolerance, 0.0f);
        const float scanRadius = airDistance + corridorHalfWidth;
        playerCell->ForEachReferenceInRange(playerPosition, scanRadius, [&](RE::TESObjectREFR& reference) {
            const auto* baseObject = reference.GetBaseObject();
            if (!baseObject || baseObject->GetFormType() != RE::FormType::Door) {
                return RE::BSContainer::ForEachResult::kContinue;
            }

            const auto doorPosition = reference.GetPosition();
            if (!SpatialGeometryPolicy::IsPointWithinSegmentCorridor(
                    {playerPosition.x, playerPosition.y, playerPosition.z},
                    {targetPosition.x, targetPosition.y, targetPosition.z},
                    {doorPosition.x, doorPosition.y, doorPosition.z}, corridorHalfWidth)) {
                return RE::BSContainer::ForEachResult::kContinue;
            }

            const auto openState = RE::BGSOpenCloseForm::GetOpenState(&reference);
            if (IsClosedDoorState(openState)) {
                ++scan.closedDoorCount;
                scan.closedDoorCandidateFormId = reference.GetFormID();
                return RE::BSContainer::ForEachResult::kStop;
            }

            if (IsOpenDoorState(openState)) {
                ++scan.openDoorCount;
            }

            return RE::BSContainer::ForEachResult::kContinue;
        });

        return scan;
    }

    bool NeedsTargetRefinement(const SpatialAwareness::Result& result)
    {
        return result.canCommunicate || result.reason == "vertical_separation";
    }

    bool IsClosePathFallbackTarget(const SpatialAwareness::Result& result,
                                   const SpatialAwareness::Settings& settings)
    {
        const float maxAudibleDistance = std::max(settings.interiorMaxDistance, settings.exteriorMaxDistance);
        const float closeDistance = std::max(settings.immediateDistance * 4.0f, maxAudibleDistance * 0.35f);
        return std::isfinite(result.airDistance) && result.airDistance <= closeDistance;
    }

    std::chrono::milliseconds PathFallbackIntervalForTarget(const SpatialAwareness::Result& result,
                                                            const SpatialAwareness::Settings& settings,
                                                            bool priorityTarget)
    {
        if (priorityTarget) {
            return kPriorityPathFallbackMinInterval;
        }

        const float maxAudibleDistance = std::max(settings.interiorMaxDistance, settings.exteriorMaxDistance);
        if (!std::isfinite(result.airDistance) || maxAudibleDistance <= 0.0f) {
            return kPathFallbackMinInterval;
        }

        const float distanceRatio = result.airDistance / maxAudibleDistance;
        if (distanceRatio <= 0.35f) {
            return kClosePathFallbackMinInterval;
        }
        if (distanceRatio >= 0.75f) {
            return kDistantPathFallbackMinInterval;
        }

        return kPathFallbackMinInterval;
    }

    bool IsEnvironmentSpatialSettling(const std::chrono::steady_clock::time_point now)
    {
        std::lock_guard<std::mutex> lock(g_playerSnapshotMutex);
        return now < g_environmentSpatialSettleUntil;
    }

    bool CanReusePlayerSnapshot(RE::Actor* player, RE::TESObjectCELL* cell, float visionRange,
                                const RE::NiPoint3& position,
                                const std::chrono::steady_clock::time_point now)
    {
        if (!player || !cell || g_playerSnapshot.timestamp.time_since_epoch().count() == 0) {
            return false;
        }

        if (g_playerSnapshot.playerFormId != player->GetFormID() ||
            g_playerSnapshot.cellFormId != cell->GetFormID()) {
            return false;
        }

        if (std::abs(g_playerSnapshot.visionRange - visionRange) > 0.001f) {
            return false;
        }

        if (now - g_playerSnapshot.timestamp > kPlayerSnapshotTtl) {
            return false;
        }

        return g_playerSnapshot.playerPosition.GetDistance(position) <= kPlayerSnapshotMoveTolerance;
    }

    bool CanReadCachedPlayerSnapshot(RE::Actor* player, RE::TESObjectCELL* cell, float visionRange,
                                      const std::chrono::steady_clock::time_point now)
    {
        if (!player || !cell || g_playerSnapshot.timestamp.time_since_epoch().count() == 0) {
            return false;
        }

        if (g_playerSnapshot.playerFormId != player->GetFormID() ||
            g_playerSnapshot.cellFormId != cell->GetFormID()) {
            return false;
        }

        if (now - g_playerSnapshot.timestamp > kPlayerSnapshotMaxStaleReadTtl) {
            return false;
        }

        return std::abs(g_playerSnapshot.visionRange - visionRange) <= 0.001f;
    }

    bool ShouldDeferPlayerSpatialForCell(RE::TESObjectCELL* cell, std::chrono::steady_clock::time_point now,
                                         const std::string& reason)
    {
        if (!cell || !cell->IsAttached()) {
            return true;
        }

        const RE::FormID cellFormId = cell->GetFormID();
        std::lock_guard<std::mutex> lock(g_playerSnapshotMutex);
        if (g_observedPlayerCellFormId != cellFormId) {
            g_observedPlayerCellFormId = cellFormId;
            g_observedPlayerCellAt = now;
            g_incrementalCandidates.clear();
            g_incrementalCandidateIndex = 0;
            g_incrementalScanComplete = true;
            g_incrementalAnchorPosition = {};
            g_incrementalAnchorCellFormId = cellFormId;
            g_playerSnapshot = {};
            logger::info("[SpatialSnapshot] Deferring spatial refresh for {}ms after cell change {:08X} reason='{}'",
                         std::chrono::duration_cast<std::chrono::milliseconds>(kCellEntrySpatialSettleTime).count(),
                         cellFormId, reason);
            return true;
        }

        return now - g_observedPlayerCellAt < kCellEntrySpatialSettleTime;
    }

    bool IsDialogDisplayCandidateQuiet(const std::shared_ptr<AIAgent>& agent, RE::Actor* actor)
    {
        if (!agent || agent->isNarrator() || !actor || actor->IsDead()) {
            return false;
        }

        if (agent->hasConversationCooldown()) {
            return false;
        }

        if (!CombatDialogueEnabled &&
            (actor->IsInCombat() || actor->IsAttacking() || actor->IsInKillMove())) {
            return false;
        }

        return true;
    }

    std::string FormatSpatialStatus(const std::string& prefix, const std::string& reason)
    {
        std::string label = reason.empty() ? "spatial check" : reason;
        std::replace(label.begin(), label.end(), '_', ' ');
        return prefix + ": " + label;
    }

    std::string DescribeVerticalSeparation(float verticalDelta)
    {
        if (!std::isfinite(verticalDelta) || std::abs(verticalDelta) < kDifferentLevelVerticalDelta) {
            return "";
        }

        return verticalDelta > 0.0f ? "above you" : "below you";
    }

    std::string FormatTargetStatus(const std::string& reason, bool canCommunicate, float verticalDelta = 0.0f)
    {
        const auto verticalStatus = DescribeVerticalSeparation(verticalDelta);

        if (reason == "closed_door_between") {
            return "Can't hear you: closed door";
        }

        if (canCommunicate) {
            if (reason == "open_door_muffled") {
                return "Can hear you, muffled by door";
            }
            if (reason == "path_fallback_clear") {
                return "Can hear you: around a wall";
            }
            if (!verticalStatus.empty()) {
                return "Can hear you: " + verticalStatus;
            }
            return "Can hear you";
        }

        if (reason == "path_ratio_los_blocked" || reason == "path_ratio_blocked" ||
            reason == "path_ratio_distance_blocked" || reason == "navmesh_no_path") {
            return "Can't hear you: around a wall";
        }
        if (reason == "different_area" || reason == "different_interior_cells" ||
            reason == "interior_exterior_boundary") {
            return "";
        }
        if (reason == "too_quiet") {
            return "Can't hear you clearly";
        }
        if (reason == "too_far") {
            return "Too far away";
        }
        if (reason == "line_of_sight_blocked") {
            return "Can't hear you: around a wall";
        }
        if (reason == "vertical_separation") {
            return verticalStatus.empty() ? "Can't hear you" : "Can't hear you: " + verticalStatus;
        }
        if (reason.empty() || reason == "pending_spatial") {
            return "";
        }

        return "";
    }

    SpatialAwareness::Result EvaluateCheapPlayerSpatial(RE::Actor* player, RE::Actor* target,
                                                        const SpatialAwareness::Settings& settings)
    {
        SpatialAwareness::Result result{};
        result.reason = "unknown";

        if (!player || !target) {
            result.reason = "invalid_actor";
            return result;
        }

        auto* playerCell = player->GetParentCell();
        auto* targetCell = target->GetParentCell();
        if (!playerCell || !playerCell->IsAttached() || !targetCell || !targetCell->IsAttached()) {
            result.reason = "different_area";
            return result;
        }

        const bool playerInterior = playerCell->IsInteriorCell();
        const bool targetInterior = targetCell->IsInteriorCell();
        if (playerInterior != targetInterior || (playerInterior && playerCell != targetCell)) {
            result.reason = "different_area";
            return result;
        }

        const auto playerPosition = player->GetPosition();
        const auto targetPosition = target->GetPosition();
        const float airDistance = playerPosition.GetDistance(targetPosition);
        result.airDistance = airDistance;
        result.pathDistance = -1.0f;
        result.pathRatio = -1.0f;
        result.navmeshPathUsed = false;
        result.navmeshPathFound = false;
        result.losFallbackUsed = false;
        result.hasLineOfSight = false;

        if (!std::isfinite(airDistance)) {
            result.reason = "invalid_distance";
            return result;
        }

        if (settings.maxAirDistance > 0.0f && airDistance > settings.maxAirDistance) {
            result.reason = "too_far";
            return result;
        }

        if (settings.immediateDistance > 0.0f && airDistance <= settings.immediateDistance) {
            result.canCommunicate = true;
            result.volume = 1.0f;
            result.reason = "immediate_proximity";
            return result;
        }

        const float audibleDistance =
            playerInterior ? settings.interiorMaxDistance : settings.exteriorMaxDistance;
        if (audibleDistance > 0.0f && airDistance > audibleDistance) {
            result.reason = "too_far";
            return result;
        }

        const float maxDistance = std::max(audibleDistance, 1.0f);
        const float distanceFactor =
            std::clamp(1.0f - (airDistance / maxDistance), settings.minDistanceFactor, 1.0f);
        const float environmentModifier =
            playerInterior ? settings.interiorBaseModifier : settings.exteriorBaseModifier;
        result.volume = std::clamp(distanceFactor * environmentModifier, 0.0f, 1.0f);

        const float verticalDelta = targetPosition.z - playerPosition.z;
        if (playerInterior && std::isfinite(verticalDelta) &&
            std::abs(verticalDelta) >= kDifferentLevelVerticalDelta) {
            result.reason = "vertical_separation";
            return result;
        }

        if (result.volume < settings.minimumAudibleVolume) {
            result.reason = "too_quiet";
            return result;
        }

        result.canCommunicate = true;
        result.reason = "distance_cell_clear";
        return result;
    }

    AudibleActorDescriptor BuildAudibleDescriptor(RE::Actor* player, RE::Actor* target,
                                                  const SpatialAwareness::Result& spatialResult)
    {
        AudibleActorDescriptor audibleActor{};
        if (!target) {
            return audibleActor;
        }

        audibleActor.formId = target->GetFormID();
        audibleActor.label = ActorLabel(target);
        audibleActor.airDistance = spatialResult.airDistance;
        audibleActor.volume = spatialResult.volume;
        audibleActor.pathDistance = spatialResult.pathDistance;
        audibleActor.pathRatio = spatialResult.pathRatio;
        audibleActor.reason = spatialResult.reason;
        audibleActor.openDoorCount = spatialResult.openDoorCount;
        audibleActor.closedDoorCount = spatialResult.closedDoorCount;
        audibleActor.navmeshPathUsed = spatialResult.navmeshPathUsed;
        audibleActor.navmeshPathFound = spatialResult.navmeshPathFound;
        audibleActor.losFallbackUsed = spatialResult.losFallbackUsed;
        audibleActor.hasLineOfSight = spatialResult.hasLineOfSight;
        audibleActor.hostile = player && target->IsHostileToActor(player);
        audibleActor.busy = !audibleActor.hostile && target->GetCurrentScene();
        audibleActor.inCombat = !audibleActor.hostile && !audibleActor.busy && target->IsInCombat();
        audibleActor.restrained = !audibleActor.hostile && !audibleActor.busy && !audibleActor.inCombat &&
                                  target->AsActorState()->GetLifeState() == RE::ACTOR_LIFE_STATE::kRestrained;
        audibleActor.canCommunicate = spatialResult.canCommunicate;
        return audibleActor;
    }

    bool TryGetCachedTargetRefinement(RE::Actor* player, RE::Actor* target, SpatialAwareness::Result& result,
                                      const std::chrono::steady_clock::time_point now)
    {
        if (!player || !target) {
            return false;
        }

        auto* cell = player->GetParentCell();
        if (!cell) {
            return false;
        }

        const auto key = TargetRefinementKey(player->GetFormID(), target->GetFormID());
        std::lock_guard<std::mutex> lock(g_targetRefinementMutex);
        const auto it = g_targetRefinementCache.find(key);
        if (it == g_targetRefinementCache.end()) {
            return false;
        }
        const auto& entry = it->second;
        if (entry.playerFormId != player->GetFormID() ||
            entry.targetFormId != target->GetFormID() ||
            entry.cellFormId != cell->GetFormID()) {
            return false;
        }
        const auto ttl = entry.result.reason == "closed_door_between" ? kClosedDoorTargetRefinementTtl :
                         (entry.result.canCommunicate ? kPositiveTargetRefinementTtl : kNegativeTargetRefinementTtl);
        if (now - entry.timestamp > ttl) {
            return false;
        }
        if (entry.playerPosition.GetDistance(player->GetPosition()) > kTargetRefinementMoveTolerance ||
            entry.targetPosition.GetDistance(target->GetPosition()) > kTargetRefinementMoveTolerance) {
            return false;
        }

        result = entry.result;
        return true;
    }

    void StoreTargetRefinement(RE::Actor* player, RE::Actor* target, const SpatialAwareness::Result& result)
    {
        auto clearQueued = []() {
            std::lock_guard<std::mutex> lock(g_targetRefinementMutex);
            g_targetRefinementTaskQueued = false;
        };

        if (!player || !target) {
            clearQueued();
            return;
        }

        auto* cell = player->GetParentCell();
        if (!cell) {
            clearQueued();
            return;
        }

        TargetRefinementCacheEntry entry{};
        entry.playerFormId = player->GetFormID();
        entry.targetFormId = target->GetFormID();
        entry.cellFormId = cell->GetFormID();
        entry.playerPosition = player->GetPosition();
        entry.targetPosition = target->GetPosition();
        entry.timestamp = std::chrono::steady_clock::now();
        entry.result = result;

        std::lock_guard<std::mutex> lock(g_targetRefinementMutex);
        if (g_targetRefinementCache.size() >= kTargetRefinementCacheMaxEntries) {
            g_targetRefinementCache.erase(g_targetRefinementCache.begin());
        }
        g_targetRefinementCache[TargetRefinementKey(entry.playerFormId, entry.targetFormId)] = std::move(entry);
        g_targetRefinementTaskQueued = false;
    }

    SpatialAwareness::Result EvaluatePathFallbackForTarget(RE::Actor* player, RE::Actor* target,
                                                           const SpatialAwareness::Settings& settings,
                                                           SpatialAwareness::Result baseResult)
    {
        const auto pathResult = SpatialAwareness::EvaluatePath(player, target, settings);
        baseResult.navmeshPathUsed = true;
        baseResult.navmeshPathFound = pathResult.status == SpatialAwareness::PathStatus::kSuccess;
        baseResult.pathDistance = pathResult.pathDistance;

        if (pathResult.status == SpatialAwareness::PathStatus::kNoPath) {
            baseResult.canCommunicate = false;
            baseResult.volume = 0.0f;
            baseResult.reason = "navmesh_no_path";
            return baseResult;
        }

        if (pathResult.status != SpatialAwareness::PathStatus::kSuccess ||
            !std::isfinite(pathResult.pathDistance) || pathResult.pathDistance < 0.0f) {
            baseResult.canCommunicate = false;
            baseResult.volume = 0.0f;
            baseResult.reason = "path_unavailable";
            return baseResult;
        }

        const bool interior = player && player->GetParentCell() && player->GetParentCell()->IsInteriorCell();
        const float maxDistance = interior ? settings.interiorMaxDistance : settings.exteriorMaxDistance;
        if (maxDistance > 0.0f && pathResult.pathDistance > maxDistance) {
            baseResult.canCommunicate = false;
            baseResult.volume = 0.0f;
            baseResult.reason = "too_far";
            return baseResult;
        }

        if (baseResult.airDistance > 0.001f) {
            baseResult.pathRatio = pathResult.pathDistance / baseResult.airDistance;
            if (baseResult.pathRatio >= settings.pathRatioReject ||
                (baseResult.pathRatio >= settings.pathRatioDistanceReject &&
                 baseResult.airDistance >= settings.pathRatioDistanceRejectMinAir)) {
                baseResult.canCommunicate = false;
                baseResult.volume = 0.0f;
                baseResult.reason = "path_ratio_blocked";
                return baseResult;
            }
        }

        baseResult.canCommunicate = true;
        baseResult.reason = "path_fallback_clear";
        return baseResult;
    }

    void QueueTargetRefinement(RE::Actor* player, RE::Actor* target,
                               const SpatialAwareness::Settings& settings,
                               const SpatialAwareness::Result& cheapResult,
                               const std::string& reason,
                               bool priorityTarget = false)
    {
        if (!player || !target || !NeedsTargetRefinement(cheapResult)) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        {
            SpatialAwareness::Result cached{};
            if (TryGetCachedTargetRefinement(player, target, cached, now)) {
                return;
            }
        }

        RE::FormID playerFormId = player->GetFormID();
        RE::FormID targetFormId = target->GetFormID();
        {
            std::lock_guard<std::mutex> lock(g_targetRefinementMutex);
            auto& nextLosRefinementAt = priorityTarget ? g_nextPriorityLosRefinementAt : g_nextBackgroundLosRefinementAt;
            const auto losInterval = priorityTarget ? kPriorityLosRefinementMinInterval : kBackgroundLosRefinementMinInterval;
            if (g_targetRefinementTaskQueued || now < nextLosRefinementAt) {
                return;
            }
            g_targetRefinementTaskQueued = true;
            nextLosRefinementAt = now + losInterval;
        }

        auto* taskInterface = SKSE::GetTaskInterface();
        if (!taskInterface) {
            std::lock_guard<std::mutex> lock(g_targetRefinementMutex);
            g_targetRefinementTaskQueued = false;
            return;
        }

        taskInterface->AddTask([playerFormId, targetFormId, settings, priorityTarget]() {
            auto clearQueued = []() {
                std::lock_guard<std::mutex> lock(g_targetRefinementMutex);
                g_targetRefinementTaskQueued = false;
            };

            auto* playerForm = RE::TESForm::LookupByID(playerFormId);
            auto* targetForm = RE::TESForm::LookupByID(targetFormId);
            auto* playerActor = playerForm ? playerForm->As<RE::Actor>() : nullptr;
            auto* targetActor = targetForm ? targetForm->As<RE::Actor>() : nullptr;
            if (!playerActor || !targetActor || targetActor->IsDead() || !targetActor->Is3DLoaded()) {
                clearQueued();
                return;
            }

            const auto storeResult = [&](const SpatialAwareness::Result& result) {
                StoreTargetRefinement(playerActor, targetActor, result);
            };

            auto result = EvaluateCheapPlayerSpatial(playerActor, targetActor, settings);
            if (!NeedsTargetRefinement(result)) {
                storeResult(result);
                return;
            }

            auto* refinementPlayerCell = playerActor->GetParentCell();
            const bool playerInteriorForDoors = refinementPlayerCell && refinementPlayerCell->IsInteriorCell();
            if (playerInteriorForDoors) {
                const auto doorScan = ScanDoorBarrierBetween(playerActor, targetActor, settings);
                result.openDoorCount = doorScan.openDoorCount;
                result.closedDoorCount = doorScan.closedDoorCount;
                result.closedDoorCandidateFormId = doorScan.closedDoorCandidateFormId;
            }

            bool hasLineOfSight = false;
            const bool losQueryOk = playerActor->HasLineOfSight(targetActor->AsReference(), hasLineOfSight);
            result.losFallbackUsed = true;
            result.losQueryOk = losQueryOk;
            result.hasLineOfSight = losQueryOk && hasLineOfSight;
            if (result.hasLineOfSight) {
                result.closedDoorCount = 0;
                result.canCommunicate = true;
                result.reason = "line_of_sight_clear";
                storeResult(result);
                return;
            }

            if (result.closedDoorCount > 0) {
                result.canCommunicate = false;
                result.volume = 0.0f;
                result.reason = "closed_door_between";
                storeResult(result);
                return;
            }

            if (result.reason == "vertical_separation") {
                result.canCommunicate = false;
                result.volume = 0.0f;
                result.reason = "line_of_sight_blocked";
                storeResult(result);
                return;
            }

            bool runPathFallback = false;
            const auto pathNow = std::chrono::steady_clock::now();
            const bool closePathTarget = IsClosePathFallbackTarget(result, settings);
            const bool usePriorityPathLane = priorityTarget || closePathTarget;
            const auto pathInterval = PathFallbackIntervalForTarget(result, settings, priorityTarget);
            {
                std::lock_guard<std::mutex> lock(g_targetRefinementMutex);
                auto& nextPathFallbackAt = usePriorityPathLane ? g_nextPriorityPathFallbackAt : g_nextPathFallbackAt;
                if (pathNow >= nextPathFallbackAt) {
                    nextPathFallbackAt = pathNow + pathInterval;
                    runPathFallback = true;
                }
            }

            if (runPathFallback) {
                result = EvaluatePathFallbackForTarget(playerActor, targetActor, settings, result);
            } else {
                clearQueued();
                return;
            }

            storeResult(result);
        });
    }

    bool IsPresentDisplayCandidate(const std::shared_ptr<AIAgent>& agent, RE::Actor* actor, RE::Actor* player)
    {
        if (!agent || agent->isNarrator() || !actor || !player) {
            return false;
        }
        if (actor->GetFormID() == player->GetFormID()) {
            return false;
        }
        if (actor->IsDead() || actor->IsDeleted() || actor->IsDisabled()) {
            return false;
        }
        if (!actor->GetActorRuntimeData().currentProcess || !actor->Is3DLoaded()) {
            return false;
        }

        return true;
    }

    bool IsAutoBlocked(const std::shared_ptr<AIAgent>& agent, RE::Actor* actor, RE::Actor* player,
                       std::string& status)
    {
        const std::string reason =
            PlayerConversationRouter::GetAutomaticBlockReason(agent, actor, player);
        if (reason.empty()) {
            return false;
        }
        if (reason == "cooldown") {
            status = "Busy";
        } else if (reason == "hostile") {
            status = "Hostile";
        } else if (reason == "combat") {
            status = "In combat";
        } else if (reason == "restrained") {
            status = "Restrained";
        } else if (reason == "unconscious") {
            status = "Unconscious";
        } else if (reason == "sleeping") {
            status = "Sleeping";
        } else if (reason == "scene") {
            status = "In scene";
        } else {
            status = "Unavailable";
        }
        return true;
    }

    std::shared_ptr<AIAgent> FindDisplayAgentByFormId(RE::FormID formId)
    {
        AIAgentManager& aiam = AIAgentManager::getInstance();
        for (const auto& agent : aiam.getAgents()) {
            if (!agent || agent->isNarrator()) {
                continue;
            }

            auto* actor = agent->getActor();
            if (!actor) {
                actor = agent->getActorByFormId();
            }
            if (!actor || actor->GetFormID() != formId) {
                continue;
            }

            if (!IsDialogDisplayCandidateQuiet(agent, actor)) {
                continue;
            }

            return std::const_pointer_cast<AIAgent>(agent);
        }

        return nullptr;
    }

    struct LookFallbackTarget {
        std::shared_ptr<AIAgent> agent;
        RE::Actor* actor = nullptr;
        float distance = 0.0f;
    };

    LookFallbackTarget FindLookFallbackTarget(RE::Actor* player)
    {
        LookFallbackTarget best{};
        if (!player) {
            return best;
        }

        auto* playerCell = player->GetParentCell();
        if (!playerCell || !playerCell->IsAttached()) {
            return best;
        }

        // Look fallback is target selection, not scene population. Keep it tied to
        // spatial hearing distance so MCM hearing range is the single speech boundary.
        constexpr float kLookFallbackCosine = 0.86f;
        const auto playerPosition = player->GetPosition();
        const float yaw = player->GetAngleZ();
        RE::NiPoint3 playerForward(std::sin(yaw), std::cos(yaw), 0.0f);
        const float forwardLength = playerForward.Length();
        if (forwardLength <= 0.0f) {
            return best;
        }
        playerForward /= forwardLength;

        const bool playerInterior = playerCell->IsInteriorCell();
        const auto spatialSettings = GetPlayerSpeechSpatialSettings(player, HERIKA_MAX_VISION_RANGE);
        float lookFallbackMaxDistance =
            playerInterior ? spatialSettings.interiorMaxDistance : spatialSettings.exteriorMaxDistance;
        if (spatialSettings.maxAirDistance > 0.0f) {
            lookFallbackMaxDistance = std::min(lookFallbackMaxDistance, spatialSettings.maxAirDistance);
        }
        if (!std::isfinite(lookFallbackMaxDistance) || lookFallbackMaxDistance <= 0.0f) {
            return best;
        }

        float bestScore = -(std::numeric_limits<float>::max)();
        AIAgentManager& aiam = AIAgentManager::getInstance();

        for (const auto& agent : aiam.getAgents()) {
            if (!agent || agent->isNarrator()) {
                continue;
            }

            auto* actor = agent->getActor();
            if (!actor) {
                actor = agent->getActorByFormId();
            }
            if (!IsDialogDisplayCandidateQuiet(agent, actor) || actor->GetFormID() == player->GetFormID()) {
                continue;
            }
            if (!actor->GetActorRuntimeData().currentProcess || !actor->Is3DLoaded()) {
                continue;
            }

            auto* actorCell = actor->GetParentCell();
            if (!actorCell || !actorCell->IsAttached()) {
                continue;
            }
            const bool actorInterior = actorCell->IsInteriorCell();
            if (playerInterior != actorInterior) {
                continue;
            }
            if (playerInterior && actorCell != playerCell) {
                continue;
            }

            RE::NiPoint3 toActor = actor->GetPosition() - playerPosition;
            const float distance = toActor.Length();
            if (!std::isfinite(distance) || distance <= 0.0f || distance > lookFallbackMaxDistance) {
                continue;
            }
            toActor /= distance;

            const float dot = playerForward.Dot(toActor);
            if (dot < kLookFallbackCosine) {
                continue;
            }

            const float distancePenalty = distance / lookFallbackMaxDistance;
            const float score = (dot * 2.0f) - distancePenalty;
            if (score > bestScore) {
                bestScore = score;
                best.agent = std::const_pointer_cast<AIAgent>(agent);
                best.actor = actor;
                best.distance = distance;
            }
        }

        return best;
    }

    bool IsBehindPlayerFacing(RE::Actor* player, const RE::NiPoint3& targetPosition)
    {
        constexpr float kBehindPlayerCosine = -0.5f;
        if (!player) {
            return false;
        }

        const auto playerPosition = player->GetPosition();
        RE::NiPoint3 toTarget(targetPosition.x - playerPosition.x, targetPosition.y - playerPosition.y, 0.0f);
        const float distance = toTarget.Length();
        if (!std::isfinite(distance) || distance <= 0.0f) {
            return false;
        }
        toTarget /= distance;

        const float yaw = player->GetAngleZ();
        RE::NiPoint3 playerForward(std::sin(yaw), std::cos(yaw), 0.0f);
        const float forwardLength = playerForward.Length();
        if (!std::isfinite(forwardLength) || forwardLength <= 0.0f) {
            return false;
        }
        playerForward /= forwardLength;

        return playerForward.Dot(toTarget) <= kBehindPlayerCosine;
    }

    void ApplyBehindPlayerClearLosStatus(RE::Actor* player, const RE::NiPoint3& actorPosition,
                                         PlayerSpatialCandidate& target)
    {
        if (target.targetable && target.reason == "line_of_sight_clear" &&
            IsBehindPlayerFacing(player, actorPosition)) {
            target.status = "Can hear you: behind you";
        }
    }

    bool NeedsNewIncrementalCandidateSet(RE::Actor* player, RE::TESObjectCELL* cell,
                                         float effectiveVisionRange)
    {
        if (!player || !cell) {
            return false;
        }

        if (g_incrementalAnchorCellFormId == 0 ||
            g_incrementalAnchorCellFormId != cell->GetFormID()) {
            return true;
        }

        if (std::abs(g_incrementalAnchorEffectiveVisionRange - effectiveVisionRange) > 0.001f) {
            return true;
        }

        if (g_incrementalScanComplete && g_playerSnapshot.timestamp.time_since_epoch().count() != 0 &&
            std::chrono::steady_clock::now() - g_playerSnapshot.timestamp >= kIncrementalSnapshotRescanInterval) {
            return true;
        }

        return g_incrementalAnchorPosition.GetDistance(player->GetPosition()) >= kIncrementalSnapshotMoveTolerance;
    }

    std::vector<IncrementalSnapshotCandidate> BuildIncrementalCandidates(RE::Actor* player, float visionRange)
    {
        std::vector<IncrementalSnapshotCandidate> candidates;
        if (!player) {
            return candidates;
        }

        auto* playerCell = player->GetParentCell();
        if (!playerCell || !playerCell->IsAttached()) {
            return candidates;
        }

        const auto spatialSettings = GetPlayerSpeechSpatialSettings(player, visionRange);
        const bool playerInterior = playerCell->IsInteriorCell();
        const auto playerPosition = player->GetPosition();
        const auto now = std::chrono::steady_clock::now();
        candidates.reserve(32);

        AIAgentManager& aiam = AIAgentManager::getInstance();
        for (const auto& agent : aiam.getAgents()) {
            if (!agent || agent->isNarrator()) {
                continue;
            }

            auto* target = agent->getActor();
            if (!target) {
                auto* targetForm = RE::TESForm::LookupByID(agent->GetFormId());
                target = targetForm ? targetForm->As<RE::Actor>() : nullptr;
            }

            if (!target || target->GetFormID() == player->GetFormID()) {
                continue;
            }

            if (!target->GetActorRuntimeData().currentProcess || !target->Is3DLoaded() || target->IsDead()) {
                continue;
            }

            auto* targetCell = target->GetParentCell();
            if (!targetCell || !targetCell->IsAttached()) {
                continue;
            }

            const bool targetInterior = targetCell->IsInteriorCell();
            if (playerInterior != targetInterior) {
                continue;
            }

            if (playerInterior && targetCell != playerCell) {
                continue;
            }

            const float airDistance = playerPosition.GetDistance(target->GetPosition());
            if (!std::isfinite(airDistance) ||
                (spatialSettings.maxAirDistance > 0.0f && airDistance >= spatialSettings.maxAirDistance)) {
                continue;
            }

            candidates.push_back({target->GetFormID(), airDistance});
        }

        std::sort(candidates.begin(), candidates.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.airDistance < rhs.airDistance;
        });

        return candidates;
    }

    bool UpsertDescriptor(std::vector<AudibleActorDescriptor>& actors, AudibleActorDescriptor actor)
    {
        for (auto& existing : actors) {
            if (existing.formId == actor.formId) {
                existing = std::move(actor);
                return false;
            }
        }

        actors.push_back(std::move(actor));
        return true;
    }

    bool IsIncrementalCandidate(const std::vector<IncrementalSnapshotCandidate>& candidates, RE::FormID formId)
    {
        return std::any_of(candidates.begin(), candidates.end(), [formId](const auto& candidate) {
            return candidate.formId == formId;
        });
    }

    void RetainCandidateDescriptors(std::vector<AudibleActorDescriptor>& actors,
                                    const std::vector<IncrementalSnapshotCandidate>& candidates)
    {
        actors.erase(std::remove_if(actors.begin(), actors.end(), [&](const auto& actor) {
            return !IsIncrementalCandidate(candidates, actor.formId);
        }), actors.end());
    }

    void RemoveDescriptor(std::vector<AudibleActorDescriptor>& actors, RE::FormID formId)
    {
        actors.erase(std::remove_if(actors.begin(), actors.end(), [formId](const auto& actor) {
            return actor.formId == formId;
        }), actors.end());
    }

    void PrepareSnapshotForIncrementalRescan(PlayerSpatialSnapshot& snapshot,
                                             const std::vector<IncrementalSnapshotCandidate>& candidates,
                                             RE::Actor* player, RE::TESObjectCELL* cell,
                                             float visionRange,
                                             const std::chrono::steady_clock::time_point now,
                                             const std::string& reason)
    {
        snapshot.playerFormId = player ? player->GetFormID() : 0;
        snapshot.cellFormId = cell ? cell->GetFormID() : 0;
        snapshot.playerPosition = player ? player->GetPosition() : RE::NiPoint3{};
        snapshot.visionRange = visionRange;
        snapshot.timestamp = now;
        snapshot.refreshReason = reason;
        RetainCandidateDescriptors(snapshot.audibleActors, candidates);
        RetainCandidateDescriptors(snapshot.evaluatedActors, candidates);
    }

    bool UpsertAudibleActor(PlayerSpatialSnapshot& snapshot, AudibleActorDescriptor actor)
    {
        return UpsertDescriptor(snapshot.audibleActors, std::move(actor));
    }

    void RemoveAudibleActor(PlayerSpatialSnapshot& snapshot, RE::FormID formId)
    {
        RemoveDescriptor(snapshot.audibleActors, formId);
    }

    bool UpsertEvaluatedActor(PlayerSpatialSnapshot& snapshot, AudibleActorDescriptor actor)
    {
        return UpsertDescriptor(snapshot.evaluatedActors, std::move(actor));
    }
}

const AudibleActorDescriptor* PlayerSpatialSnapshot::FindAudible(RE::FormID formId) const
{
    for (const auto& actor : audibleActors) {
        if (actor.formId == formId) {
            return &actor;
        }
    }

    return nullptr;
}

const AudibleActorDescriptor* PlayerSpatialSnapshot::FindEvaluated(RE::FormID formId) const
{
    for (const auto& actor : evaluatedActors) {
        if (actor.formId == formId) {
            return &actor;
        }
    }

    return FindAudible(formId);
}

std::string PlayerSpatialSnapshot::Describe(const std::string& separator) const
{
    return DescribeAudibleActors(audibleActors, separator);
}

PlayerSpatialSnapshot SpatialSnapshotManager::GetPlayerSnapshot(bool forceRefresh, const std::string& reason)
{
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        logger::warn("[SpatialSnapshot] No player for snapshot request '{}'", reason);
        return {};
    }

    auto* cell = player->GetParentCell();
    if (!cell || !cell->IsAttached()) {
        logger::warn("[SpatialSnapshot] Player cell unavailable for snapshot request '{}'", reason);
        return {};
    }

    constexpr float visionRange = HERIKA_MAX_VISION_RANGE;
    const auto position = player->GetPosition();
    const auto now = std::chrono::steady_clock::now();
    const auto spatialSettings = GetPlayerSpeechSpatialSettings(player, visionRange);
    const float effectiveVisionRange = spatialSettings.maxAirDistance;

    // Cell entry settling is only a refinement throttle. Keep the cheap
    // air-distance candidate snapshot warm so UI and routing do not go blind
    // for the full settle window after cell-load event bursts.
    ShouldDeferPlayerSpatialForCell(cell, now, reason);

    {
        std::lock_guard<std::mutex> lock(g_playerSnapshotMutex);
        if (!forceRefresh && CanReusePlayerSnapshot(player, cell, effectiveVisionRange, position, now)) {
            return g_playerSnapshot;
        }
        if (!forceRefresh) {
            if (CanReadCachedPlayerSnapshot(player, cell, effectiveVisionRange, now)) {
                return g_playerSnapshot;
            }

            return {};
        }
    }

    PlayerSpatialSnapshot refreshed;
    refreshed.playerFormId = player->GetFormID();
    refreshed.cellFormId = cell->GetFormID();
    refreshed.playerPosition = position;
    refreshed.visionRange = effectiveVisionRange;
    refreshed.timestamp = now;
    refreshed.refreshReason = reason;
    const auto candidates = BuildIncrementalCandidates(player, visionRange);
    refreshed.audibleActors.reserve(candidates.size());
    refreshed.evaluatedActors.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        auto* targetForm = RE::TESForm::LookupByID(candidate.formId);
        auto* target = targetForm ? targetForm->As<RE::Actor>() : nullptr;
        if (!target || target->IsDead() || !target->Is3DLoaded()) {
            continue;
        }

        const auto spatialResult = EvaluateCheapPlayerSpatial(player, target, spatialSettings);
        auto descriptor = BuildAudibleDescriptor(player, target, spatialResult);
        UpsertEvaluatedActor(refreshed, descriptor);
        if (descriptor.canCommunicate) {
            UpsertAudibleActor(refreshed, std::move(descriptor));
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_playerSnapshotMutex);
        g_playerSnapshot = refreshed;
        return g_playerSnapshot;
    }
}

bool SpatialSnapshotManager::UpdatePlayerSnapshotIncremental(const std::string& reason, std::size_t maxEvaluations)
{
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        return false;
    }

    auto* cell = player->GetParentCell();
    if (!cell || !cell->IsAttached()) {
        return false;
    }

    constexpr float visionRange = HERIKA_MAX_VISION_RANGE;
    const auto initialSpatialSettings = GetPlayerSpeechSpatialSettings(player, visionRange);
    const float effectiveVisionRange = initialSpatialSettings.maxAirDistance;
    const auto now = std::chrono::steady_clock::now();

    // This pass only builds/evaluates the cheap air-distance snapshot. Do not
    // starve it during cell-entry settling; expensive LOS/navmesh refinement is
    // throttled by the target-refinement queue.
    ShouldDeferPlayerSpatialForCell(cell, now, reason);

    {
        std::lock_guard<std::mutex> lock(g_playerSnapshotMutex);
        if (NeedsNewIncrementalCandidateSet(player, cell, effectiveVisionRange)) {
            g_incrementalCandidates = BuildIncrementalCandidates(player, visionRange);
            g_incrementalCandidateIndex = 0;
            g_incrementalScanComplete = g_incrementalCandidates.empty();
            g_incrementalAnchorPosition = player->GetPosition();
            g_incrementalAnchorCellFormId = cell->GetFormID();
            g_incrementalAnchorEffectiveVisionRange = effectiveVisionRange;

            PrepareSnapshotForIncrementalRescan(g_playerSnapshot, g_incrementalCandidates, player, cell,
                                                effectiveVisionRange, now, reason);
        } else if (g_incrementalScanComplete) {
            return false;
        }
    }

    std::size_t evaluations = 0;
    std::size_t advanced = 0;
    while (evaluations < maxEvaluations) {
        IncrementalSnapshotCandidate candidate{};
        {
            std::lock_guard<std::mutex> lock(g_playerSnapshotMutex);
            if (g_incrementalCandidateIndex >= g_incrementalCandidates.size()) {
                g_incrementalScanComplete = true;
                break;
            }

            candidate = g_incrementalCandidates[g_incrementalCandidateIndex++];
        }

        ++advanced;
        auto* targetForm = RE::TESForm::LookupByID(candidate.formId);
        auto* target = targetForm ? targetForm->As<RE::Actor>() : nullptr;
        if (!target || target->IsDead() || !target->Is3DLoaded()) {
            continue;
        }

        const auto spatialSettings = GetPlayerSpeechSpatialSettings(player, visionRange);
        const auto spatialResult = EvaluateCheapPlayerSpatial(player, target, spatialSettings);
        auto audibleActor = BuildAudibleDescriptor(player, target, spatialResult);
        ++evaluations;

        {
            std::lock_guard<std::mutex> lock(g_playerSnapshotMutex);
            g_playerSnapshot.timestamp = now;
            g_playerSnapshot.refreshReason = reason;
            UpsertEvaluatedActor(g_playerSnapshot, audibleActor);
            if (audibleActor.canCommunicate) {
                UpsertAudibleActor(g_playerSnapshot, std::move(audibleActor));
            } else {
                RemoveAudibleActor(g_playerSnapshot, audibleActor.formId);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_playerSnapshotMutex);
        if (g_incrementalCandidateIndex >= g_incrementalCandidates.size()) {
            g_incrementalScanComplete = true;
        }
    }

    return advanced > 0;
}

PlayerSpatialTargetStatus SpatialSnapshotManager::GetPlayerCrosshairTargetStatus(bool forceRefresh,
                                                                                 const std::string& reason)
{
    PlayerSpatialTargetStatus resolved{};
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        return resolved;
    }

    auto* playerCell = player->GetParentCell();
    if (!playerCell || !playerCell->IsAttached()) {
        return resolved;
    }

    const auto now = std::chrono::steady_clock::now();
    // Observe cell changes, but do not suppress the cheap crosshair/look
    // status. Only the queued LOS/navmesh refinement below is blocked during
    // the settle window.
    const bool spatialRefinementSettling = ShouldDeferPlayerSpatialForCell(playerCell, now, reason);

    RE::FormID crosshairFormId = 0;
    if (auto* crosshairPickData = RE::CrosshairPickData::GetSingleton();
        crosshairPickData && crosshairPickData->target) {
        if (auto crosshairTarget = crosshairPickData->target.get();
            crosshairTarget && crosshairTarget->GetFormType() == RE::FormType::ActorCharacter) {
            crosshairFormId = crosshairTarget->GetFormID();
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_playerCrosshairTargetMutex);
        if (!forceRefresh && g_playerCrosshairTargetAt.time_since_epoch().count() != 0 &&
            now - g_playerCrosshairTargetAt < kPlayerCrosshairTargetTtl) {
            if (crosshairFormId == 0 || crosshairFormId == g_playerCrosshairTargetFormId) {
                return g_playerCrosshairTarget;
            }
        }
    }

    std::shared_ptr<AIAgent> crosshairAgent = nullptr;
    RE::Actor* targetActorOverride = nullptr;
    bool usedLookFallback = false;
    if (crosshairFormId != 0) {
        crosshairAgent = FindDisplayAgentByFormId(crosshairFormId);
    }
    if (!crosshairAgent) {
        const auto fallback = FindLookFallbackTarget(player);
        if (fallback.agent && fallback.actor) {
            crosshairAgent = fallback.agent;
            targetActorOverride = fallback.actor;
            crosshairFormId = fallback.actor->GetFormID();
            usedLookFallback = true;
        }
    }

    if (crosshairAgent && (targetActorOverride || crosshairAgent->getActor())) {
        auto* crosshairActor = targetActorOverride ? targetActorOverride : crosshairAgent->getActor();
        const auto spatialSettings = GetPlayerSpeechSpatialSettings(player, HERIKA_MAX_VISION_RANGE);
        auto spatial = EvaluateCheapPlayerSpatial(player, crosshairActor, spatialSettings);
        SpatialAwareness::Result cachedRefinement{};
        if (TryGetCachedTargetRefinement(player, crosshairActor, cachedRefinement, now)) {
            spatial = cachedRefinement;
        } else if (!spatialRefinementSettling) {
            QueueTargetRefinement(player, crosshairActor, spatialSettings, spatial, reason, true);
        }
        resolved.hasTarget = true;
        resolved.name = crosshairAgent->getActorName().empty() ? ActorLabel(crosshairActor)
                                                                : crosshairAgent->getActorName();
        resolved.formId = crosshairActor->GetFormID();
        resolved.airDistance = spatial.airDistance;
        resolved.distanceMeters = spatial.airDistance * kSkyrimUnitsToMeters;
        const std::string sourcePrefix = usedLookFallback ? "look" : "crosshair";
        resolved.source = sourcePrefix + (spatial.canCommunicate ? "_audible" : "_blocked");
        resolved.reason = spatial.reason;
        resolved.status = FormatTargetStatus(spatial.reason, spatial.canCommunicate,
                                             crosshairActor->GetPosition().z - player->GetPosition().z);
        resolved.targetable = spatial.canCommunicate;
    } else {
        // Overlay/status polling must not refresh the full player spatial snapshot.
        // A dense palace scan here caused periodic freezes because it ran even when
        // the player was not actively choosing a target.
        resolved.status = "No crosshair target";
    }

    {
        std::lock_guard<std::mutex> lock(g_playerCrosshairTargetMutex);
        g_playerCrosshairTarget = resolved;
        g_playerCrosshairTargetFormId = crosshairFormId;
        g_playerCrosshairTargetAt = now;
    }

    return resolved;
}

float SpatialSnapshotManager::GetAutoHearingRadiusUnits()
{
    const auto settings = SpatialAwareness::GetSettings();
    if (std::isfinite(settings.autoHearingDistance) && settings.autoHearingDistance > 0.0f) {
        return settings.autoHearingDistance;
    }

    return SpatialAwareness::kAutoHearingDistance;
}

bool SpatialSnapshotManager::IsActorWithinAutoHearingRadius(RE::Actor* actor, float maxDistanceUnits)
{
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!std::isfinite(maxDistanceUnits) || maxDistanceUnits <= 0.0f) {
        maxDistanceUnits = GetAutoHearingRadiusUnits();
    }
    if (!player || !actor || !std::isfinite(maxDistanceUnits) || maxDistanceUnits <= 0.0f) {
        return false;
    }

    auto* playerCell = player->GetParentCell();
    auto* actorCell = actor->GetParentCell();
    if (!playerCell || !playerCell->IsAttached() || !actorCell || !actorCell->IsAttached()) {
        return false;
    }

    const bool playerInterior = playerCell->IsInteriorCell();
    const bool actorInterior = actorCell->IsInteriorCell();
    if (playerInterior != actorInterior || (playerInterior && actorCell != playerCell)) {
        return false;
    }

    const float airDistance = player->GetPosition().GetDistance(actor->GetPosition());
    return std::isfinite(airDistance) && airDistance <= maxDistanceUnits;
}

std::vector<PlayerSpatialCandidate> SpatialSnapshotManager::GetPlayerNearbyManagedTargets(float maxDistanceUnits)
{
    std::vector<PlayerSpatialCandidate> targets;

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!std::isfinite(maxDistanceUnits) || maxDistanceUnits <= 0.0f) {
        maxDistanceUnits = GetAutoHearingRadiusUnits();
    }
    if (!player || !std::isfinite(maxDistanceUnits) || maxDistanceUnits <= 0.0f) {
        return targets;
    }

    auto* playerCell = player->GetParentCell();
    if (!playerCell || !playerCell->IsAttached()) {
        return targets;
    }

    const auto playerPosition = player->GetPosition();
    AIAgentManager& aiam = AIAgentManager::getInstance();
    for (const auto& agent : aiam.getAgents()) {
        if (!agent) {
            continue;
        }

        auto* actor = agent->getActor();
        if (!actor) {
            actor = agent->getActorByFormId();
        }
        if (!IsPresentDisplayCandidate(agent, actor, player)) {
            continue;
        }
        if (!IsActorWithinAutoHearingRadius(actor, maxDistanceUnits)) {
            continue;
        }

        const float airDistance = playerPosition.GetDistance(actor->GetPosition());
        if (!std::isfinite(airDistance)) {
            continue;
        }

        PlayerSpatialCandidate target{};
        target.agent = std::const_pointer_cast<AIAgent>(agent);
        target.actor = actor;
        target.formId = actor->GetFormID();
        target.name = agent->getActorName().empty() ? ActorLabel(actor) : agent->getActorName();
        target.airDistance = airDistance;
        target.distanceMeters = airDistance * kSkyrimUnitsToMeters;
        target.source = "player_nearby_context";
        target.reason = "player_nearby_air";
        target.status = "Nearby";
        targets.push_back(std::move(target));
    }

    std::sort(targets.begin(), targets.end(), [](const PlayerSpatialCandidate& lhs,
                                                 const PlayerSpatialCandidate& rhs) {
        if (std::abs(lhs.distanceMeters - rhs.distanceMeters) > 0.001f) {
            return lhs.distanceMeters < rhs.distanceMeters;
        }
        return lhs.name < rhs.name;
    });

    return targets;
}

std::vector<PlayerSpatialCandidate> SpatialSnapshotManager::GetPlayerConversationTargets(
    const std::string& reason, bool includeUnavailable)
{
    std::vector<PlayerSpatialCandidate> targets;
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        return targets;
    }

    auto* playerCell = player->GetParentCell();
    if (!playerCell || !playerCell->IsAttached()) {
        return targets;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto playerPosition = player->GetPosition();
    {
        std::lock_guard<std::mutex> lock(g_playerConversationTargetsMutex);
        if (g_playerConversationTargetsCacheValid &&
            g_playerConversationTargetsIncludeUnavailable == includeUnavailable &&
            g_playerConversationTargetsCellFormId == playerCell->GetFormID() &&
            now - g_playerConversationTargetsAt < kPlayerConversationTargetsTtl &&
            g_playerConversationTargetsPlayerPosition.GetDistance(playerPosition) <=
                kPlayerConversationTargetsMoveTolerance) {
            return g_playerConversationTargetsCache;
        }
    }

    const auto spatialSnapshot = GetPlayerSnapshot(false, reason + "_snapshot");
    const auto lookTarget = GetPlayerCrosshairTargetStatus(false, reason + "_look");
    const auto spatialSettings = GetPlayerSpeechSpatialSettings(player, HERIKA_MAX_VISION_RANGE);
    const bool playerInterior = playerCell->IsInteriorCell();
    const bool spatialRefinementSettling = IsPlayerSpatialSettling();
    const float autoHearingRadiusUnits = SpatialSnapshotManager::GetAutoHearingRadiusUnits();
    const float displayHardLimit = std::max(spatialSettings.maxAirDistance, spatialSettings.exteriorMaxDistance) * 2.0f;

    AIAgentManager& aiam = AIAgentManager::getInstance();
    for (const auto& agent : aiam.getAgents()) {
        if (!agent) {
            continue;
        }

        auto* actor = agent->getActor();
        if (!actor) {
            actor = agent->getActorByFormId();
        }
        if (!IsPresentDisplayCandidate(agent, actor, player)) {
            continue;
        }

        auto* actorCell = actor->GetParentCell();
        if (!actorCell || !actorCell->IsAttached()) {
            continue;
        }

        const bool actorInterior = actorCell->IsInteriorCell();
        if (playerInterior != actorInterior) {
            continue;
        }
        if (playerInterior && actorCell != playerCell) {
            continue;
        }

        const auto actorPosition = actor->GetPosition();
        const float verticalDelta = actorPosition.z - playerPosition.z;
        const float airDistance = playerPosition.GetDistance(actorPosition);
        if (!std::isfinite(airDistance) || (displayHardLimit > 0.0f && airDistance > displayHardLimit)) {
            continue;
        }
        const bool withinAutoHearingRadius =
            airDistance <= autoHearingRadiusUnits;

        PlayerSpatialCandidate target{};
        target.agent = std::const_pointer_cast<AIAgent>(agent);
        target.actor = actor;
        target.formId = actor->GetFormID();
        target.name = agent->getActorName().empty() ? ActorLabel(actor) : agent->getActorName();
        target.airDistance = airDistance;
        target.distanceMeters = airDistance * kSkyrimUnitsToMeters;
        target.source = "managed";

        const bool isLookTarget = lookTarget.hasTarget && lookTarget.formId == target.formId;
        const auto* evaluated = spatialSnapshot.FindEvaluated(target.formId);
        if (evaluated) {
            target.name = evaluated->label.empty() ? target.name : evaluated->label;
            target.reason = evaluated->reason;
            target.targetable = evaluated->canCommunicate;
            target.status = FormatTargetStatus(target.reason, target.targetable, verticalDelta);
            target.source = "snapshot";
            target.sortBucket = target.targetable ? 1 : 3;
        } else if (isLookTarget) {
            target.airDistance = lookTarget.airDistance;
            target.distanceMeters = lookTarget.distanceMeters;
            target.reason = lookTarget.reason;
            target.status = lookTarget.status.empty()
                ? FormatTargetStatus(lookTarget.reason, lookTarget.targetable, verticalDelta)
                : lookTarget.status;
            target.targetable = lookTarget.targetable;
            target.source = lookTarget.source;
            target.sortBucket = lookTarget.targetable ? 0 : 2;
        } else {
            const auto cheapSpatial = EvaluateCheapPlayerSpatial(player, actor, spatialSettings);
            target.reason = cheapSpatial.reason;
            target.targetable = cheapSpatial.canCommunicate;
            // Cheap air/cell eligibility stays live during settle windows. Only
            // the expensive LOS/navmesh refinement queue is paused below.
            target.status = FormatTargetStatus(target.reason, target.targetable, verticalDelta);
            target.source = target.targetable ? "distance_cell" : "distance_cell_blocked";
            target.sortBucket = target.targetable ? 1 : (target.reason == "too_far" ? 4 : 3);
        }

        target.lookTarget = isLookTarget;
        if (isLookTarget) {
            target.sortBucket = target.targetable ? 0 : 2;
            target.source = lookTarget.source.empty() ? target.source : lookTarget.source;
            if (!lookTarget.status.empty()) {
                target.status = lookTarget.status;
            }
        }

        if (withinAutoHearingRadius) {
            target.targetable = true;
            target.reason = "immediate_proximity";
            target.status = "Can hear you";
            if (!target.lookTarget) {
                target.source = "auto_hearing_radius";
                target.sortBucket = 1;
            }
        }

        std::string blockedStatus;
        if (target.targetable && IsAutoBlocked(target.agent, target.actor, player, blockedStatus)) {
            target.autoEligible = false;
            target.status = blockedStatus;
            target.sortBucket = std::max(target.sortBucket, 2);
        } else {
            target.autoEligible = target.targetable && withinAutoHearingRadius;
        }

        ApplyBehindPlayerClearLosStatus(player, actorPosition, target);

        if (target.targetable || includeUnavailable) {
            targets.push_back(std::move(target));
        }
    }

    std::sort(targets.begin(), targets.end(), [](const PlayerSpatialCandidate& lhs,
                                                 const PlayerSpatialCandidate& rhs) {
        if (lhs.sortBucket != rhs.sortBucket) {
            return lhs.sortBucket < rhs.sortBucket;
        }
        if (lhs.lookTarget != rhs.lookTarget) {
            return lhs.lookTarget;
        }
        if (std::abs(lhs.distanceMeters - rhs.distanceMeters) > 0.001f) {
            return lhs.distanceMeters < rhs.distanceMeters;
        }
        return lhs.name < rhs.name;
    });

    bool nearestAutoEligibleSeen = false;
    for (auto& target : targets) {
        if (!target.actor) {
            continue;
        }
        if (!target.targetable && target.reason != "vertical_separation" && target.reason != "pending_spatial") {
            continue;
        }

        const float currentAirDistance = player->GetPosition().GetDistance(target.actor->GetPosition());
        const bool withinAutoHearingRadius =
            std::isfinite(currentAirDistance) &&
            currentAirDistance <= autoHearingRadiusUnits;
        if (withinAutoHearingRadius) {
            if (!target.lookTarget && target.autoEligible && !nearestAutoEligibleSeen) {
                nearestAutoEligibleSeen = true;
            }
            continue;
        }

        SpatialAwareness::Result cachedRefinement{};
        if (TryGetCachedTargetRefinement(player, target.actor, cachedRefinement, now)) {
            const auto& spatial = cachedRefinement;
            const float verticalDelta = target.actor->GetPosition().z - playerPosition.z;
            target.reason = spatial.reason;
            target.targetable = spatial.canCommunicate;
            target.status = FormatTargetStatus(target.reason, target.targetable, verticalDelta);
            target.source = spatial.navmeshPathUsed ? "refined_path" :
                            (spatial.losFallbackUsed ? "refined_los" : target.source);
            target.sortBucket = target.targetable ? (target.lookTarget ? 0 : 1) : 3;
            target.autoEligible = target.targetable && withinAutoHearingRadius;
            ApplyBehindPlayerClearLosStatus(player, target.actor->GetPosition(), target);
            if (!target.lookTarget && target.autoEligible && !nearestAutoEligibleSeen) {
                nearestAutoEligibleSeen = true;
            }
            continue;
        } else if (!spatialRefinementSettling) {
            auto spatial = EvaluateCheapPlayerSpatial(player, target.actor, spatialSettings);
            const bool nearestAutoEligible = !target.lookTarget && target.autoEligible && !nearestAutoEligibleSeen;
            if (!target.lookTarget && target.autoEligible) {
                nearestAutoEligibleSeen = true;
            }
            QueueTargetRefinement(player, target.actor, spatialSettings, spatial, reason,
                                  target.lookTarget || nearestAutoEligible);
        }
        break;
    }

    std::sort(targets.begin(), targets.end(), [](const PlayerSpatialCandidate& lhs,
                                                 const PlayerSpatialCandidate& rhs) {
        if (lhs.sortBucket != rhs.sortBucket) {
            return lhs.sortBucket < rhs.sortBucket;
        }
        if (lhs.lookTarget != rhs.lookTarget) {
            return lhs.lookTarget;
        }
        if (std::abs(lhs.distanceMeters - rhs.distanceMeters) > 0.001f) {
            return lhs.distanceMeters < rhs.distanceMeters;
        }
        return lhs.name < rhs.name;
    });

    {
        std::lock_guard<std::mutex> lock(g_playerConversationTargetsMutex);
        g_playerConversationTargetsCache = targets;
        g_playerConversationTargetsAt = now;
        g_playerConversationTargetsCellFormId = playerCell->GetFormID();
        g_playerConversationTargetsPlayerPosition = playerPosition;
        g_playerConversationTargetsIncludeUnavailable = includeUnavailable;
        g_playerConversationTargetsCacheValid = true;
    }

    return targets;
}

bool SpatialSnapshotManager::IsValidPlayerSpeechTarget(
    const PlayerSpatialCandidate& target, PlayerSpeechTargetMode mode)
{
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!target.agent || target.agent->isNarrator() || !target.actor || !player) {
        return false;
    }

    if (!IsPresentDisplayCandidate(target.agent, target.actor, player)) {
        return false;
    }

    auto* playerCell = player->GetParentCell();
    auto* targetCell = target.actor->GetParentCell();
    if (!playerCell || !playerCell->IsAttached() || !targetCell || !targetCell->IsAttached()) {
        return false;
    }

    const bool playerInterior = playerCell->IsInteriorCell();
    const bool targetInterior = targetCell->IsInteriorCell();
    if (playerInterior != targetInterior || (playerInterior && targetCell != playerCell)) {
        return false;
    }

    const auto playerPosition = SpatialAwareness::GetEffectiveActorPosition(player); // VR: HMD, not the parked ref - kept scene partners "untargetable" and rerouted speech to the Narrator (fix 2026-07-01)
    const auto targetPosition = target.actor->GetPosition();
    const float airDistance = playerPosition.GetDistance(targetPosition);
    if (!std::isfinite(airDistance)) {
        return false;
    }

    if (mode == PlayerSpeechTargetMode::AutoHearing &&
        airDistance > SpatialSnapshotManager::GetAutoHearingRadiusUnits()) {
        return false;
    }
    if (mode == PlayerSpeechTargetMode::Manual && !target.targetable) {
        return false;
    }

    if (mode == PlayerSpeechTargetMode::Manual) {
        return true;
    }

    std::string blockedStatus;
    return !IsAutoBlocked(target.agent, target.actor, player, blockedStatus);
}

std::vector<PlayerSpatialCandidate> SpatialSnapshotManager::GetValidPlayerSpeechTargets(
    const std::string& reason, bool requireComplete, PlayerSpeechTargetMode mode)
{
    const auto rawTargets = GetPlayerConversationTargets(reason, true);
    std::vector<PlayerSpatialCandidate> targets;
    targets.reserve(rawTargets.size());

    auto* player = RE::PlayerCharacter::GetSingleton();
    for (auto target : rawTargets) {
        if (!SpatialSnapshotManager::IsValidPlayerSpeechTarget(target, mode)) {
            continue;
        }

        if (player && target.actor) {
            const float airDistance = player->GetPosition().GetDistance(target.actor->GetPosition());
            if (std::isfinite(airDistance)) {
                target.airDistance = airDistance;
                target.distanceMeters = airDistance * kSkyrimUnitsToMeters;
            }
        }

        target.targetable = true;
        if (mode == PlayerSpeechTargetMode::AutoHearing) {
            target.autoEligible = true;
            target.reason = "close_managed";
            target.status = "Can hear you";
            if (!target.lookTarget) {
                target.source = "close_managed";
            }
        }
        target.sortBucket = target.lookTarget ? 0 : 1;
        targets.push_back(std::move(target));
    }

    std::sort(targets.begin(), targets.end(), [](const PlayerSpatialCandidate& lhs,
                                                 const PlayerSpatialCandidate& rhs) {
        if (lhs.sortBucket != rhs.sortBucket) {
            return lhs.sortBucket < rhs.sortBucket;
        }
        if (lhs.lookTarget != rhs.lookTarget) {
            return lhs.lookTarget;
        }
        if (std::abs(lhs.distanceMeters - rhs.distanceMeters) > 0.001f) {
            return lhs.distanceMeters < rhs.distanceMeters;
        }
        return lhs.name < rhs.name;
    });

    return targets;
}

bool SpatialSnapshotManager::IsPlayerSpatialSettling()
{
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* cell = player ? player->GetParentCell() : nullptr;
    if (!cell || !cell->IsAttached()) {
        return true;
    }

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_playerSnapshotMutex);
    if (now < g_environmentSpatialSettleUntil) {
        return true;
    }
    if (g_observedPlayerCellFormId == cell->GetFormID() &&
        now - g_observedPlayerCellAt < kCellEntrySpatialSettleTime) {
        return true;
    }

    return false;
}

void SpatialSnapshotManager::InvalidatePlayerSnapshot()
{
    {
        std::lock_guard<std::mutex> lock(g_playerSnapshotMutex);
        g_playerSnapshot = {};
        g_observedPlayerCellFormId = 0;
        g_observedPlayerCellAt = {};
        g_incrementalCandidates.clear();
        g_incrementalCandidateIndex = 0;
        g_incrementalScanComplete = true;
        g_incrementalAnchorEffectiveVisionRange = 0.0f;
    }
    {
        std::lock_guard<std::mutex> lock(g_playerCrosshairTargetMutex);
        g_playerCrosshairTarget = {};
        g_playerCrosshairTargetFormId = 0;
        g_playerCrosshairTargetAt = {};
    }
    {
        std::lock_guard<std::mutex> lock(g_playerConversationTargetsMutex);
        g_playerConversationTargetsCache.clear();
        g_playerConversationTargetsAt = {};
        g_playerConversationTargetsCellFormId = 0;
        g_playerConversationTargetsPlayerPosition = {};
        g_playerConversationTargetsCacheValid = false;
    }
    {
        std::lock_guard<std::mutex> lock(g_targetRefinementMutex);
        g_targetRefinementCache.clear();
        g_targetRefinementTaskQueued = false;
        g_nextPriorityLosRefinementAt = {};
        g_nextBackgroundLosRefinementAt = {};
        g_nextPriorityPathFallbackAt = {};
        g_nextPathFallbackAt = {};
    }
}

void SpatialSnapshotManager::InvalidateDynamicSpatialState()
{
    {
        std::lock_guard<std::mutex> lock(g_playerSnapshotMutex);
        // Keep the cheap air-distance population list warm. Dynamic barriers only
        // invalidate refined audibility status, not who exists near the player.
        g_playerSnapshot.evaluatedActors.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_playerCrosshairTargetMutex);
        g_playerCrosshairTarget = {};
        g_playerCrosshairTargetFormId = 0;
        g_playerCrosshairTargetAt = {};
    }
    {
        std::lock_guard<std::mutex> lock(g_playerConversationTargetsMutex);
        g_playerConversationTargetsCache.clear();
        g_playerConversationTargetsAt = {};
        g_playerConversationTargetsCellFormId = 0;
        g_playerConversationTargetsPlayerPosition = {};
        g_playerConversationTargetsCacheValid = false;
    }
    {
        std::lock_guard<std::mutex> lock(g_targetRefinementMutex);
        g_targetRefinementCache.clear();
        g_targetRefinementTaskQueued = false;
        g_nextPriorityLosRefinementAt = {};
        g_nextBackgroundLosRefinementAt = {};
        g_nextPriorityPathFallbackAt = {};
        g_nextPathFallbackAt = {};
    }
}

void SpatialSnapshotManager::InvalidateForEnvironmentChange(std::chrono::milliseconds settleDuration)
{
    const auto now = std::chrono::steady_clock::now();
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* cell = player ? player->GetParentCell() : nullptr;

    {
        std::lock_guard<std::mutex> lock(g_playerSnapshotMutex);
        g_playerSnapshot = {};
        g_incrementalCandidates.clear();
        g_incrementalCandidateIndex = 0;
        g_incrementalScanComplete = true;
        g_incrementalAnchorPosition = {};
        g_incrementalAnchorCellFormId = cell ? cell->GetFormID() : 0;
        g_incrementalAnchorEffectiveVisionRange = 0.0f;
        if (cell && cell->IsAttached()) {
            g_observedPlayerCellFormId = cell->GetFormID();
            g_observedPlayerCellAt = now - kCellEntrySpatialSettleTime;
        }
        g_environmentSpatialSettleUntil = std::max(g_environmentSpatialSettleUntil, now + settleDuration);
    }
    {
        std::lock_guard<std::mutex> lock(g_playerCrosshairTargetMutex);
        g_playerCrosshairTarget = {};
        g_playerCrosshairTargetFormId = 0;
        g_playerCrosshairTargetAt = {};
    }
    {
        std::lock_guard<std::mutex> lock(g_playerConversationTargetsMutex);
        g_playerConversationTargetsCache.clear();
        g_playerConversationTargetsAt = {};
        g_playerConversationTargetsCellFormId = 0;
        g_playerConversationTargetsPlayerPosition = {};
        g_playerConversationTargetsCacheValid = false;
    }
    {
        std::lock_guard<std::mutex> lock(g_targetRefinementMutex);
        g_targetRefinementCache.clear();
        g_targetRefinementTaskQueued = false;
        g_nextPriorityLosRefinementAt = {};
        g_nextBackgroundLosRefinementAt = {};
        g_nextPriorityPathFallbackAt = {};
        g_nextPathFallbackAt = {};
    }
}
