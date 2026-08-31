#include "PlayerConversationRouter.h"

#include "Misc.h"
#include "PlayerConversationRoutingPolicy.h"
#include "SpatialAwareness.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace logger = SKSE::log;

namespace
{
    struct RuntimeCandidate
    {
        PlayerConversationRoutingPolicy::Candidate policy;
        std::shared_ptr<AIAgent> agent;
        RE::Actor* actor = nullptr;
        std::string automaticBlockReason;
        std::string spatialReason;
    };

    const char* SourceName(PlayerConversationInputSource source)
    {
        switch (source) {
        case PlayerConversationInputSource::PrismaText:
            return "prisma";
        case PlayerConversationInputSource::Voice:
            return "voice";
        case PlayerConversationInputSource::LegacyText:
        default:
            return "legacy_text";
        }
    }

    const char* ModeName(PlayerConversationSpeechMode mode)
    {
        switch (mode) {
        case PlayerConversationSpeechMode::Whisper:
            return "whisper";
        case PlayerConversationSpeechMode::Shout:
            return "shout";
        case PlayerConversationSpeechMode::Close:
            return "close";
        case PlayerConversationSpeechMode::Standard:
        default:
            return "standard";
        }
    }

    bool IsHardEligible(RE::Actor* actor, RE::Actor* player, std::string& reason)
    {
        if (!actor || !player) {
            reason = "invalid_actor";
            return false;
        }
        if (actor->GetFormID() == player->GetFormID()) {
            reason = "player";
            return false;
        }
        if (actor->IsDead()) {
            reason = "dead";
            return false;
        }
        if (actor->IsDeleted() || actor->IsDisabled()) {
            reason = "disabled";
            return false;
        }
        if (!actor->GetActorRuntimeData().currentProcess || !actor->Is3DLoaded()) {
            reason = "not_loaded";
            return false;
        }

        auto* playerCell = player->GetParentCell();
        auto* actorCell = actor->GetParentCell();
        if (!playerCell || !playerCell->IsAttached() || !actorCell || !actorCell->IsAttached()) {
            reason = "cell_unavailable";
            return false;
        }
        const bool playerInterior = playerCell->IsInteriorCell();
        const bool actorInterior = actorCell->IsInteriorCell();
        if (playerInterior != actorInterior || (playerInterior && playerCell != actorCell)) {
            reason = "different_loaded_area";
            return false;
        }

        reason.clear();
        return true;
    }

    RE::FormID GetTrueCrosshairActorFormId()
    {
        auto* crosshair = RE::CrosshairPickData::GetSingleton();
        if (!crosshair || !crosshair->target) {
            return 0;
        }
        auto reference = crosshair->target.get();
        auto* actor = reference ? reference->As<RE::Actor>() : nullptr;
        return actor ? actor->GetFormID() : 0;
    }

    RE::NiPoint3 RotateVector(const RE::NiQuaternion& quaternion, const RE::NiPoint3& vector)
    {
        const RE::NiPoint3 axis(quaternion.x, quaternion.y, quaternion.z);
        const RE::NiPoint3 firstCross(
            axis.y * vector.z - axis.z * vector.y,
            axis.z * vector.x - axis.x * vector.z,
            axis.x * vector.y - axis.y * vector.x);
        const RE::NiPoint3 secondInput(
            firstCross.x + (quaternion.w * vector.x),
            firstCross.y + (quaternion.w * vector.y),
            firstCross.z + (quaternion.w * vector.z));
        const RE::NiPoint3 secondCross(
            axis.y * secondInput.z - axis.z * secondInput.y,
            axis.z * secondInput.x - axis.x * secondInput.z,
            axis.x * secondInput.y - axis.y * secondInput.x);
        return vector + (secondCross * 2.0f);
    }

    bool GetCameraForward(RE::Actor* player, RE::NiPoint3& forward)
    {
        if (!player) {
            return false;
        }

        if (REL::Module::IsVR()) {
            auto* playerCharacter = RE::PlayerCharacter::GetSingleton();
            auto* nodeData = playerCharacter ? playerCharacter->GetVRNodeData() : nullptr;
            auto* hmd = (nodeData && nodeData->UprightHmdNode) ? nodeData->UprightHmdNode.get() : nullptr;
            if (hmd) {
                forward = hmd->world.rotate * RE::NiPoint3(0.0f, 1.0f, 0.0f);
            }
        } else {
            auto* camera = RE::PlayerCamera::GetSingleton();
            auto* state = camera ? camera->currentState.get() : nullptr;
            if (state) {
                RE::NiQuaternion rotation{};
                state->GetRotation(rotation);
                forward = RotateVector(rotation, RE::NiPoint3(0.0f, 1.0f, 0.0f));
            }
        }

        float length = forward.Length();
        if (!std::isfinite(length) || length <= 0.001f) {
            const float yaw = player->GetAngleZ();
            forward = RE::NiPoint3(std::sin(yaw), std::cos(yaw), 0.0f);
            length = forward.Length();
        }
        if (!std::isfinite(length) || length <= 0.001f) {
            return false;
        }
        forward /= length;
        return true;
    }

    bool IsNarratorGesture(RE::Actor* player)
    {
        if (!player) {
            return false;
        }
        if (REL::Module::IsVR()) {
            auto* playerCharacter = RE::PlayerCharacter::GetSingleton();
            auto* nodeData = playerCharacter ? playerCharacter->GetVRNodeData() : nullptr;
            auto* hmd = (nodeData && nodeData->UprightHmdNode) ? nodeData->UprightHmdNode.get() : nullptr;
            if (!hmd) {
                return false;
            }
            const float forwardZ = std::clamp(hmd->world.rotate.entry[2][1], -1.0f, 1.0f);
            const float pitchDegrees = -std::asin(forwardZ) * (180.0f / 3.141592654f);
            return std::isfinite(pitchDegrees) && pitchDegrees < -85.0f;
        }

        auto* camera = RE::PlayerCamera::GetSingleton();
        auto* state = camera ? camera->currentState.get() : nullptr;
        if (!state) {
            return false;
        }
        RE::NiQuaternion rotation{};
        state->GetRotation(rotation);
        const float pitchDegrees = GetPitchFromQuaternion(rotation) * (180.0f / 3.141592654f);
        return std::isfinite(pitchDegrees) && pitchDegrees < -85.0f;
    }

    float ModeMultiplier(PlayerConversationSpeechMode mode)
    {
        switch (mode) {
        case PlayerConversationSpeechMode::Whisper:
            return 0.35f;
        case PlayerConversationSpeechMode::Shout:
            return 2.0f;
        case PlayerConversationSpeechMode::Standard:
        case PlayerConversationSpeechMode::Close:
        default:
            return 1.0f;
        }
    }

    void AddUniqueAudience(std::vector<std::string>& audience, std::unordered_set<std::string>& seen,
                           const std::string& name)
    {
        if (!name.empty() && seen.insert(name).second) {
            audience.push_back(name);
        }
    }

    void CollectPresentActors(PlayerConversationRoutingResult& result, RE::Actor* player,
                              const std::unordered_set<RE::FormID>& managedFormIds,
                              PlayerConversationSpeechMode mode)
    {
        PlayerConversationRoutingPolicy::PresenceRequest request{};
        request.radius = result.audienceRadiusUnits;
        request.includeAllRaces = AutoAddAllRaces;
        request.includeIncidental =
            !result.narrator && mode != PlayerConversationSpeechMode::Whisper &&
            mode != PlayerConversationSpeechMode::Close;
        if (!request.includeIncidental || !player) {
            return;
        }

        auto* processLists = RE::ProcessLists::GetSingleton();
        auto* playerCell = player->GetParentCell();
        if (!processLists || !playerCell) {
            return;
        }

        const bool playerInterior = playerCell->IsInteriorCell();
        auto* playerWorldspace = player->GetWorldspace();
        const auto playerPosition = SpatialAwareness::GetEffectiveActorPosition(player);
        std::vector<PlayerConversationRoutingPolicy::PresenceCandidate> candidates;
        candidates.reserve(processLists->highActorHandles.size());

        for (auto& actorHandle : processLists->highActorHandles) {
            auto actorPointer = actorHandle.get();
            auto* actor = actorPointer ? actorPointer.get() : nullptr;
            std::string hardReason;
            if (!IsHardEligible(actor, player, hardReason)) {
                continue;
            }
            if (!playerInterior && actor->GetWorldspace() != playerWorldspace) {
                continue;
            }

            auto* race = actor->GetRace();
            if (!race) {
                continue;
            }
            const std::string name = actor->GetDisplayFullName();
            const float distance =
                playerPosition.GetDistance(SpatialAwareness::GetEffectiveActorPosition(actor));
            if (!std::isfinite(distance)) {
                continue;
            }

            PlayerConversationRoutingPolicy::PresenceCandidate candidate{};
            candidate.formId = actor->GetFormID();
            candidate.name = name;
            candidate.distance = distance;
            candidate.hardEligible = true;
            candidate.dialogueRace = race->AllowsPCDialogue();
            candidate.creature = race->HasKeywordString("ActorTypeCreature");
            candidates.push_back(std::move(candidate));
        }

        const auto selected = PlayerConversationRoutingPolicy::SelectPresent(request, candidates);
        result.presentActors.reserve(selected.size());
        for (const auto index : selected) {
            const auto& candidate = candidates[index];
            PlayerConversationPresentActor present{};
            present.formId = candidate.formId;
            present.name = candidate.name;
            present.distance = candidate.distance;
            present.managed = managedFormIds.contains(candidate.formId);
            present.creature = candidate.creature;
            result.presentActors.push_back(std::move(present));
        }
    }
}

bool PlayerConversationRouter::IsActorSleeping(RE::Actor* actor)
{
    if (!actor) {
        return false;
    }

    auto* actorState = actor->AsActorState();
    return actorState &&
        actorState->GetSitSleepState() == RE::SIT_SLEEP_STATE::kIsSleeping;
}

std::string PlayerConversationRouter::GetAutomaticBlockReason(
    const std::shared_ptr<AIAgent>& agent, RE::Actor* actor, RE::Actor* player,
    bool ignoreSleeping)
{
    if (!agent || !actor || !player) {
        return "invalid_actor";
    }

    PlayerConversationRoutingPolicy::AutomaticEligibilityFacts facts{};
    facts.conversationCooldown = agent->hasConversationCooldown();
    facts.hostile = actor->IsHostileToActor(player);
    facts.autoAddHostile = AutoAddHostile;
    facts.inCombat = actor->IsInCombat() || actor->IsAttacking() || actor->IsInKillMove();
    facts.combatDialogueEnabled = CombatDialogueEnabled;
    facts.restrained =
        actor->AsActorState()->GetLifeState() == RE::ACTOR_LIFE_STATE::kRestrained;
    facts.unconscious = actor->AsActorState()->IsUnconscious();
    facts.sleeping = IsActorSleeping(actor);
    facts.inScene = actor->GetCurrentScene() != nullptr;
    facts.sceneDialogueEnabled = AllowActorsOnScene;
    return std::string(PlayerConversationRoutingPolicy::GetAutomaticBlockReason(facts, ignoreSleeping));
}

PlayerConversationSpeechMode PlayerConversationRouter::ParseSpeechMode(std::string_view mode)
{
    std::string normalized(mode);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char value) { return static_cast<char>(std::toupper(value)); });

    if (normalized == "WHISPER") {
        return PlayerConversationSpeechMode::Whisper;
    }
    if (normalized == "SHOUT") {
        return PlayerConversationSpeechMode::Shout;
    }
    if (normalized == "CLOSE") {
        return PlayerConversationSpeechMode::Close;
    }
    return PlayerConversationSpeechMode::Standard;
}

float PlayerConversationRouter::GetCloseRadiusUnits(bool sneaking)
{
    return sneaking ? kCloseRadiusUnits * 0.5f : kCloseRadiusUnits;
}

PlayerConversationRoutingResult PlayerConversationRouter::Resolve(
    const std::string& wireMessage, const PlayerConversationRoutingContext& context)
{
    PlayerConversationRoutingResult result{};
    result.modeName = ModeName(context.mode);

    auto* player = RE::PlayerCharacter::GetSingleton();
    auto& manager = AIAgentManager::getInstance();
    auto narrator = manager.getAgentByName(NARRATOR_NAME);
    if (!player) {
        logger::warn("[PLAYER-ROUTING] No player singleton");
        return result;
    }

    const auto baseSettings = SpatialAwareness::GetSettings();
    const bool playerInterior = player->GetParentCell() && player->GetParentCell()->IsInteriorCell();
    float modifier = ModeMultiplier(context.mode);
    if (player->IsSneaking()) {
        modifier *= 0.5f;
    }

    const float baseListenerRadius =
        baseSettings.autoHearingDistance > 0.0f
            ? baseSettings.autoHearingDistance
            : SpatialAwareness::kAutoHearingDistance;
    const float baseDirectRadius =
        playerInterior ? baseSettings.interiorMaxDistance : baseSettings.exteriorMaxDistance;
    result.listenerRadiusUnits = context.mode == PlayerConversationSpeechMode::Close
        ? GetCloseRadiusUnits(player->IsSneaking())
        : baseListenerRadius * modifier;
    result.audienceRadiusUnits = result.listenerRadiusUnits;
    const float directAddressRadius =
        context.mode == PlayerConversationSpeechMode::Close
            ? result.listenerRadiusUnits
            : std::max(result.listenerRadiusUnits, baseDirectRadius * modifier);

    SpatialAwareness::Settings audienceSettings = baseSettings;
    audienceSettings.maxAirDistance = result.audienceRadiusUnits;
    audienceSettings.interiorMaxDistance = result.audienceRadiusUnits;
    audienceSettings.exteriorMaxDistance = result.audienceRadiusUnits;
    audienceSettings.autoHearingDistance = 0.0f;
    audienceSettings.immediateDistance = 0.0f;
    SpatialAwareness::InvalidateCache();

    const auto playerPosition = SpatialAwareness::GetEffectiveActorPosition(player);
    RE::NiPoint3 cameraForward{};
    const bool hasCameraForward = GetCameraForward(player, cameraForward);
    const RE::FormID crosshairFormId = GetTrueCrosshairActorFormId();

    std::vector<RuntimeCandidate> runtimeCandidates;
    std::unordered_set<RE::FormID> managedFormIds;
    for (const auto& agentValue : manager.getAgents()) {
        if (!agentValue || agentValue->isNarrator()) {
            continue;
        }

        auto agent = agentValue;
        auto* actor = agent->getActor();
        if (!actor) {
            actor = agent->getActorByFormId();
        }

        RuntimeCandidate candidate{};
        candidate.agent = std::move(agent);
        candidate.actor = actor;
        candidate.policy.formId = actor ? actor->GetFormID() : agentValue->GetFormId();
        if (candidate.policy.formId != 0) {
            managedFormIds.insert(candidate.policy.formId);
        }
        candidate.policy.name =
            !agentValue->getActorName().empty()
                ? agentValue->getActorName()
                : (actor ? std::string(actor->GetDisplayFullName()) : "");
        candidate.policy.trueCrosshair =
            crosshairFormId != 0 && candidate.policy.formId == crosshairFormId;
        candidate.policy.sleeping = IsActorSleeping(actor);

        std::string hardReason;
        candidate.policy.hardEligible = IsHardEligible(actor, player, hardReason);
        if (!candidate.policy.hardEligible) {
            candidate.automaticBlockReason = std::move(hardReason);
            runtimeCandidates.push_back(std::move(candidate));
            continue;
        }
        const auto actorPosition = actor->GetPosition();
        candidate.policy.distance = playerPosition.GetDistance(actorPosition);
        if (!std::isfinite(candidate.policy.distance)) {
            candidate.policy.hardEligible = false;
            candidate.automaticBlockReason = "invalid_distance";
            runtimeCandidates.push_back(std::move(candidate));
            continue;
        }

        if (hasCameraForward && candidate.policy.distance > 0.001f) {
            RE::NiPoint3 toActor = actorPosition - playerPosition;
            toActor /= candidate.policy.distance;
            candidate.policy.facingDot = cameraForward.Dot(toActor);
        }

        candidate.automaticBlockReason =
            PlayerConversationRouter::GetAutomaticBlockReason(candidate.agent, actor, player);
        candidate.policy.autoEligible = candidate.automaticBlockReason.empty();
        if (candidate.policy.distance <= result.audienceRadiusUnits) {
            const auto spatial = SpatialAwareness::Evaluate(player, actor, audienceSettings);
            candidate.policy.audible = spatial.canCommunicate;
            candidate.spatialReason = spatial.reason;
        } else {
            candidate.spatialReason = "outside_effective_radius";
        }
        if (actor->IsPlayerTeammate() && candidate.policy.autoEligible &&
            candidate.policy.audible) {
            result.presentPartyActors.push_back(actor);
        }

        runtimeCandidates.push_back(std::move(candidate));
    }

    std::vector<PlayerConversationRoutingPolicy::Candidate> policyCandidates;
    policyCandidates.reserve(runtimeCandidates.size());
    for (const auto& candidate : runtimeCandidates) {
        policyCandidates.push_back(candidate.policy);
    }

    PlayerConversationRoutingPolicy::Request policyRequest{};
    policyRequest.utterance = context.routingMessage.empty()
        ? PlayerConversationRoutingPolicy::ExtractUtterance(wireMessage)
        : PlayerConversationRoutingPolicy::Normalize(context.routingMessage);
    policyRequest.explicitTargetFormId = context.explicitTargetFormId;
    policyRequest.explicitTargetName = context.explicitTargetName;
    policyRequest.directAddressRadius = directAddressRadius;
    policyRequest.interactionRadius = result.listenerRadiusUnits;
    policyRequest.narratorMode = context.narratorMode;
    policyRequest.everyoneMode =
        context.everyoneMode &&
        context.mode != PlayerConversationSpeechMode::Whisper &&
        context.mode != PlayerConversationSpeechMode::Close;
    policyRequest.narratorGesture = IsNarratorGesture(player);
    policyRequest.blockSleepingDirectTarget = context.mode != PlayerConversationSpeechMode::Shout;

    const auto selection = PlayerConversationRoutingPolicy::Select(policyRequest, policyCandidates);
    result.reason = selection.reason;
    result.broadcast = selection.broadcast;

    if (selection.kind == PlayerConversationRoutingPolicy::SelectionKind::Rejected) {
        result.rejected = true;
        if (selection.candidateIndex < runtimeCandidates.size()) {
            result.rejectedTargetName = runtimeCandidates[selection.candidateIndex].policy.name;
        }
        logger::info(
            "[PLAYER-ROUTING] source={} mode={} utterance='{}' rejected direct target '{}' reason={}",
            SourceName(context.source), result.modeName, policyRequest.utterance,
            result.rejectedTargetName, result.reason);
        return result;
    }

    std::size_t selectedIndex = (std::numeric_limits<std::size_t>::max)();
    if (selection.kind == PlayerConversationRoutingPolicy::SelectionKind::Candidate &&
        selection.candidateIndex < runtimeCandidates.size()) {
        selectedIndex = selection.candidateIndex;
        const auto& selected = runtimeCandidates[selectedIndex];
        result.responder = selected.agent;
        result.responderActor = selected.actor;
        result.responderName = selected.policy.name;
        result.direct =
            result.reason == "explicit_npc_name" || result.reason == "explicit_ui_target" ||
            result.reason == "true_crosshair";
    } else if (selection.kind == PlayerConversationRoutingPolicy::SelectionKind::Narrator && narrator) {
        result.responder = narrator;
        result.responderActor = narrator->getActor();
        result.responderName = NARRATOR_NAME;
        result.narrator = true;
        result.broadcast = false;
    }

    CollectPresentActors(result, player, managedFormIds, context.mode);

    std::vector<std::size_t> audienceOrder;
    audienceOrder.reserve(runtimeCandidates.size());
    for (std::size_t index = 0; index < runtimeCandidates.size(); ++index) {
        const auto& candidate = runtimeCandidates[index];
        if (PlayerConversationRoutingPolicy::IsAudienceMember(
                candidate.policy, index == selectedIndex, result.audienceRadiusUnits)) {
            audienceOrder.push_back(index);
        }
    }
    std::sort(audienceOrder.begin(), audienceOrder.end(),
        [&](std::size_t left, std::size_t right) {
            if (left == right) {
                return false;
            }
            if (left == selectedIndex || right == selectedIndex) {
                return left == selectedIndex;
            }
            const auto& lhs = runtimeCandidates[left].policy;
            const auto& rhs = runtimeCandidates[right].policy;
            if (lhs.distance != rhs.distance) {
                return lhs.distance < rhs.distance;
            }
            return lhs.formId < rhs.formId;
        });

    std::unordered_set<std::string> seenAudience;
    if (!result.narrator) {
        for (const auto index : audienceOrder) {
            AddUniqueAudience(result.audience, seenAudience, runtimeCandidates[index].policy.name);
        }
    }
    AddUniqueAudience(result.audience, seenAudience, player->GetName());

    std::string rejected;
    for (const auto& candidate : runtimeCandidates) {
        if (candidate.policy.hardEligible && candidate.policy.autoEligible && candidate.policy.audible) {
            continue;
        }
        if (!rejected.empty()) {
            rejected += ", ";
        }
        const std::string reason = !candidate.automaticBlockReason.empty()
            ? candidate.automaticBlockReason
            : candidate.spatialReason;
        rejected += std::format("{}({:08X}: {})", candidate.policy.name, candidate.policy.formId,
                                reason.empty() ? "not_eligible" : reason);
    }

    const auto inactivePresentCount = std::count_if(
        result.presentActors.begin(), result.presentActors.end(),
        [](const PlayerConversationPresentActor& actor) { return !actor.managed; });
    logger::info(
        "[PLAYER-ROUTING] source={} mode={} utterance='{}' crosshair={:08X} explicit={:08X} "
        "responder='{}' reason={} direct={} broadcast={} listener_radius={:.1f} audience_radius={:.1f} "
        "audience_count={} present_count={} inactive_present_count={} rejected=[{}]",
        SourceName(context.source), result.modeName, policyRequest.utterance, crosshairFormId,
        context.explicitTargetFormId, result.responderName, result.reason, result.direct ? 1 : 0,
        result.broadcast ? 1 : 0, result.listenerRadiusUnits, result.audienceRadiusUnits,
        result.audience.size(), result.presentActors.size(), inactivePresentCount, rejected);

    return result;
}
