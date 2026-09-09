#pragma once

#include <string>

#include "RE/Skyrim.h"

namespace SpatialAwareness
{
    inline constexpr float kSkyrimUnitsPerMeter = 70.0f;
    inline constexpr float kAutoHearingRadiusMeters = 10.0f;
    inline constexpr float kMinAutoHearingRadiusMeters = 1.0f;
    inline constexpr float kMaxAutoHearingRadiusMeters = 20.0f;
    inline constexpr float kAutoHearingDistance =
        kAutoHearingRadiusMeters * kSkyrimUnitsPerMeter;

    // Where the actor's perception actually is. VR player: the headset position (the ref can park far from
    // the real body - OStim pins it mid-scene and playspace drift is never written back, so scene partners
    // read as "too far" at arm's length). NPCs: the ref position (the engine keeps it under their skeleton).
    RE::NiPoint3 GetEffectiveActorPosition(RE::Actor* actor);

    // Refresh the per-frame VR camera position snapshot (paced by the Present hook, captured on the game
    // thread). Audio worker threads must not read camRoot->world.translate directly: off-thread reads race
    // the renderer and return torn positions, heard as voices drifting away mid-scene.
    void UpdatePlayerCameraSnapshot();

    // HMD gaze ray (position + normalized forward) from the same per-frame snapshot. Returns false when
    // no valid pose is available yet. Off-game-thread callers (SHARMAT gaze dwell polling) must use this,
    // never a live camRoot read - same torn-transform hazard as the position above.
    bool GetPlayerCameraGaze(RE::NiPoint3& a_outOrigin, RE::NiPoint3& a_outForward);

    struct Settings
    {
        float maxAirDistance = 4000.0f;
        float immediateDistance = 150.0f;
        float autoHearingDistance = kAutoHearingDistance;
        // Range ceilings in Skyrim units; attenuation can make speech inaudible sooner.
        float interiorMaxDistance = 1000.0f;
        float exteriorMaxDistance = 1800.0f;
        float minDistanceFactor = 0.1f;
        float interiorBaseModifier = 1.0f;
        float exteriorBaseModifier = 0.7f;
        float openDoorPenaltyBase = 0.85f;
        float aroundCornerPenalty = 0.60f;
        float minimumAudibleVolume = 0.15f;
        float doorTriangulationAbsoluteTolerance = 80.0f;
        float pathRatioReject = 4.0f;
        float pathRatioDistanceReject = 2.5f;
        float pathRatioDistanceRejectMinAir = 500.0f;
        float losConfirmPathRatio = 2.0f;
        float losConfirmMinAirDistance = 400.0f;
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
        RE::FormID closedDoorCandidateFormId = 0;
        bool navmeshPathUsed = false;
        bool navmeshPathFound = false;
        bool losFallbackUsed = false;
        bool losQueryOk = false;
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
    void SetAutoHearingRadiusMeters(float meters);
    float GetAutoHearingRadiusMeters();
    void InvalidateCache();

    void SetDoorStateCell(RE::FormID cellId);
    void ResetDoorStates();
    void ForgetDoorState(RE::FormID doorId);
    void RecordDoorState(RE::TESObjectREFR* door, bool opened);
    RE::BGSOpenCloseForm::OPEN_STATE GetDoorState(RE::TESObjectREFR* door);

    PathResult EvaluatePath(RE::Actor* speaker, RE::Actor* listener, const Settings& settings = GetSettings());
    Result Evaluate(RE::Actor* speaker, RE::Actor* listener, const Settings& settings = GetSettings());
}
