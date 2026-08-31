#include "PlayerConversationRoutingPolicy.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    using Candidate = PlayerConversationRoutingPolicy::Candidate;
    using Request = PlayerConversationRoutingPolicy::Request;
    using Result = PlayerConversationRoutingPolicy::Result;
    using SelectionKind = PlayerConversationRoutingPolicy::SelectionKind;

    void Check(bool condition, const char* message)
    {
        if (!condition) {
            std::cerr << message << std::endl;
            std::exit(1);
        }
    }

    Candidate MakeCandidate(std::uint32_t formId, std::string name, float distance,
                            bool hardEligible = true, bool autoEligible = true,
                            bool audible = true, float facingDot = 1.0f,
                            bool crosshair = false, bool sleeping = false)
    {
        Candidate candidate{};
        candidate.formId = formId;
        candidate.name = std::move(name);
        candidate.distance = distance;
        candidate.hardEligible = hardEligible;
        candidate.autoEligible = autoEligible;
        candidate.audible = audible;
        candidate.facingDot = facingDot;
        candidate.trueCrosshair = crosshair;
        candidate.sleeping = sleeping;
        return candidate;
    }

    Result Select(std::string utterance, const std::vector<Candidate>& candidates)
    {
        Request request{};
        request.utterance = std::move(utterance);
        request.directAddressRadius = 1000.0f;
        request.interactionRadius = 560.0f;
        return PlayerConversationRoutingPolicy::Select(request, candidates);
    }
}

int main()
{
    using namespace PlayerConversationRoutingPolicy;

    Check(IsPlayerInitiatedRequest("inputtext|1|date|Player: hello"),
          "Direct player speech was not recognized");
    Check(IsPlayerInitiatedRequest("ginputtext_s|1|date|Player: hello"),
          "Direct group speech was not recognized");
    Check(!IsPlayerInitiatedRequest("rpg_word|1|date|context"),
          "Automatic Word of Power event was recognized as player speech");
    Check(IsDiaryRequest("diary|1|date|Player: update your diary"),
          "Diary event was not recognized");
    Check(!IsDiaryRequest("diary_nearby|1|date|Player: update your diary"),
          "Nearby diary event was recognized as a targeted diary request");
    Check(!IsPlayerInitiatedRequest("diary|1|date|Player: update your diary"),
          "Diary event was recognized as direct player speech");

    AutomaticEligibilityFacts eligibility{};
    Check(GetAutomaticBlockReason(eligibility).empty(),
          "Eligible actor received an automatic block reason");
    eligibility.conversationCooldown = true;
    Check(GetAutomaticBlockReason(eligibility) == "cooldown", "Cooldown was not enforced");
    eligibility = {};
    eligibility.hostile = true;
    Check(GetAutomaticBlockReason(eligibility) == "hostile", "Hostility was not enforced");
    eligibility.autoAddHostile = true;
    Check(GetAutomaticBlockReason(eligibility).empty(), "Enabled hostile auto-add was ignored");
    eligibility = {};
    eligibility.inCombat = true;
    Check(GetAutomaticBlockReason(eligibility) == "combat", "Combat was not enforced");
    eligibility.combatDialogueEnabled = true;
    Check(GetAutomaticBlockReason(eligibility).empty(), "Enabled combat dialogue was ignored");
    eligibility = {};
    eligibility.restrained = true;
    Check(GetAutomaticBlockReason(eligibility) == "restrained", "Restraint was not enforced");
    eligibility = {};
    eligibility.unconscious = true;
    Check(GetAutomaticBlockReason(eligibility) == "unconscious", "Unconscious state was not enforced");
    eligibility = {};
    eligibility.sleeping = true;
    Check(GetAutomaticBlockReason(eligibility) == "sleeping", "Sleeping was not enforced");
    Check(GetAutomaticBlockReason(eligibility, true).empty(),
          "Sleeping-only eligibility was not restored when sleep was ignored");
    eligibility.inScene = true;
    Check(GetAutomaticBlockReason(eligibility, true) == "scene",
          "Ignoring sleep also bypassed Scene Safety");
    eligibility = {};
    eligibility.inScene = true;
    Check(GetAutomaticBlockReason(eligibility) == "scene", "Scene Safety was not enforced");
    eligibility.sceneDialogueEnabled = true;
    Check(GetAutomaticBlockReason(eligibility).empty(), "Enabled scene dialogue was ignored");

    Check(ExtractUtterance("inputtext|1|date|Rangroo: Hey Lydia, come here") ==
              "hey lydia come here",
          "Wire utterance extraction failed");

    std::vector<Candidate> candidates{
        MakeCandidate(0x10, "Camilla Valerius", 100.0f, true, false, true, 1.0f, true),
        MakeCandidate(0x20, "Lydia", 200.0f),
        MakeCandidate(0x30, "Lydia Snow-Born", 150.0f),
        MakeCandidate(0x40, "Lucan Valerius", 80.0f, true, true, true, 0.9f)
    };

    auto result = Select("Hey Lydia Snow-Born, listen", candidates);
    Check(result.kind == SelectionKind::Candidate && result.candidateIndex == 2,
          "Longest exact NPC name did not win");
    Check(result.reason == "explicit_npc_name", "Named target reason changed");

    result = Select("Normal speech", candidates);
    Check(result.candidateIndex == 0 && result.reason == "true_crosshair",
          "True crosshair did not bypass soft eligibility");

    Request cooldownTargetRequest{};
    cooldownTargetRequest.utterance = "Normal speech";
    cooldownTargetRequest.explicitTargetFormId = 0x10;
    cooldownTargetRequest.explicitTargetName = "Camilla Valerius";
    cooldownTargetRequest.directAddressRadius = 1000.0f;
    cooldownTargetRequest.interactionRadius = 560.0f;
    result = PlayerConversationRoutingPolicy::Select(cooldownTargetRequest, candidates);
    Check(result.candidateIndex == 0 && result.reason == "explicit_ui_target",
          "Explicit player input did not bypass conversation cooldown");

    result = Select("Hey Camilla Valerius", candidates);
    Check(result.candidateIndex == 0 && result.reason == "explicit_npc_name",
          "Named player input did not bypass conversation cooldown");

    candidates[0].trueCrosshair = false;
    result = Select("Normal speech", candidates);
    Check(result.candidateIndex == 3 && result.reason == "nearest_eligible",
          "Conversation cooldown did not block automatic selection");

    candidates[0].hardEligible = false;
    result = Select("Hey", candidates);
    Check(result.candidateIndex == 2 && result.reason == "bare_hey_fov",
          "Bare Hey did not choose the best eligible FOV target");

    candidates[3].facingDot = 0.2f;
    result = Select("Normal speech", candidates);
    Check(result.candidateIndex == 3 && result.reason == "nearest_eligible",
          "Nearest eligible target was not selected");

    candidates[1].autoEligible = false;
    candidates[2].autoEligible = false;
    candidates[3].autoEligible = false;
    result = Select("Normal speech", candidates);
    Check(result.kind == SelectionKind::Narrator && result.reason == "no_eligible_npc",
          "Narrator fallback did not occur");

    Request skyRequest{};
    skyRequest.utterance = "Normal speech";
    skyRequest.directAddressRadius = 1000.0f;
    skyRequest.interactionRadius = 560.0f;
    skyRequest.narratorGesture = true;
    result = PlayerConversationRoutingPolicy::Select(skyRequest, candidates);
    Check(result.kind == SelectionKind::Narrator && result.reason == "narrator_camera_gesture",
          "Narrator camera gesture did not beat proximity fallback");

    Request explicitRequest{};
    explicitRequest.utterance = "Normal speech";
    explicitRequest.explicitTargetFormId = 0x20;
    explicitRequest.directAddressRadius = 1000.0f;
    explicitRequest.interactionRadius = 560.0f;
    result = PlayerConversationRoutingPolicy::Select(explicitRequest, candidates);
    Check(result.kind == SelectionKind::Candidate && result.candidateIndex == 1 &&
              result.reason == "explicit_ui_target",
          "Explicit UI target was not selected");

    explicitRequest.explicitTargetName = "Lucan Valerius";
    result = PlayerConversationRoutingPolicy::Select(explicitRequest, candidates);
    Check(result.kind == SelectionKind::Candidate && result.candidateIndex == 1,
          "Explicit RefID did not take priority over its fallback name");

    explicitRequest.explicitTargetFormId = 0x99;
    result = PlayerConversationRoutingPolicy::Select(explicitRequest, candidates);
    Check(result.kind == SelectionKind::Candidate && result.candidateIndex == 3,
          "Missing explicit RefID did not fall back to the target name");

    explicitRequest.explicitTargetFormId = 0x20;
    explicitRequest.explicitTargetName = "Lydia";
    explicitRequest.utterance = "Hey Lucan Valerius";
    result = PlayerConversationRoutingPolicy::Select(explicitRequest, candidates);
    Check(result.kind == SelectionKind::Candidate && result.candidateIndex == 1,
          "Explicit RefID did not take priority over an utterance name");

    result = Select("Hey Narrator, explain this", candidates);
    Check(result.kind == SelectionKind::Narrator && result.reason == "explicit_narrator_name",
          "Explicit Narrator address was not selected");

    Request narratorModeRequest{};
    narratorModeRequest.utterance = "Normal speech";
    narratorModeRequest.directAddressRadius = 1000.0f;
    narratorModeRequest.interactionRadius = 560.0f;
    narratorModeRequest.narratorMode = true;
    result = PlayerConversationRoutingPolicy::Select(narratorModeRequest, candidates);
    Check(result.kind == SelectionKind::Narrator && result.reason == "explicit_narrator_mode",
          "Explicit Narrator mode was not selected");

    Request broadcastRequest{};
    broadcastRequest.utterance = "Normal speech";
    broadcastRequest.directAddressRadius = 1000.0f;
    broadcastRequest.interactionRadius = 560.0f;
    broadcastRequest.everyoneMode = true;
    candidates[3].autoEligible = true;
    result = PlayerConversationRoutingPolicy::Select(broadcastRequest, candidates);
    Check(result.kind == SelectionKind::Candidate && result.broadcast,
          "Everyone mode did not preserve broadcast routing");

    candidates[1].autoEligible = false;
    candidates[1].audible = false;
    Request softBypassRequest{};
    softBypassRequest.utterance = "Normal speech";
    softBypassRequest.explicitTargetFormId = 0x20;
    softBypassRequest.directAddressRadius = 1000.0f;
    softBypassRequest.interactionRadius = 560.0f;
    result = PlayerConversationRoutingPolicy::Select(softBypassRequest, candidates);
    Check(result.kind == SelectionKind::Candidate && result.candidateIndex == 1 &&
              result.reason == "explicit_ui_target",
          "Explicit target did not bypass soft eligibility");

    std::vector<Candidate> inaudibleCandidates{
        MakeCandidate(0x60, "Near But Blocked", 50.0f, true, true, false),
        MakeCandidate(0x61, "Far And Audible", 200.0f)
    };
    result = Select("Normal speech", inaudibleCandidates);
    Check(result.kind == SelectionKind::Candidate && result.candidateIndex == 1,
          "Automatic routing selected an inaudible candidate");

    std::vector<Candidate> distantCrosshair{
        MakeCandidate(0x70, "Distant Crosshair", 1200.0f, true, true, true, 1.0f, true)
    };
    result = Select("Normal speech", distantCrosshair);
    Check(result.kind == SelectionKind::Narrator,
          "Crosshair target outside direct-address radius was selected");

    candidates[1].autoEligible = true;
    candidates[1].audible = true;
    candidates[1].distance = 200.0f;
    candidates[2].hardEligible = true;
    candidates[3].hardEligible = true;
    candidates[3].autoEligible = true;
    result = Select("Hey there", candidates);
    Check(result.kind == SelectionKind::Candidate && result.candidateIndex == 3 &&
              result.reason == "nearest_eligible",
          "Non-bare Hey incorrectly used the bare-Hey FOV rule");

    std::vector<Candidate> duplicates{
        MakeCandidate(0x50, "Erik", 300.0f),
        MakeCandidate(0x51, "Erik", 100.0f)
    };
    result = Select("Hey Erik", duplicates);
    Check(result.candidateIndex == 1, "Closest duplicate full name was not selected deterministically");

    Request duplicateExplicitRequest{};
    duplicateExplicitRequest.utterance = "Normal speech";
    duplicateExplicitRequest.explicitTargetFormId = 0x50;
    duplicateExplicitRequest.explicitTargetName = "Erik";
    duplicateExplicitRequest.directAddressRadius = 1000.0f;
    duplicateExplicitRequest.interactionRadius = 560.0f;
    result = PlayerConversationRoutingPolicy::Select(duplicateExplicitRequest, duplicates);
    Check(result.candidateIndex == 0,
          "Explicit RefID did not beat a closer actor with the same name");

    duplicateExplicitRequest.explicitTargetFormId = 0x99;
    result = PlayerConversationRoutingPolicy::Select(duplicateExplicitRequest, duplicates);
    Check(result.candidateIndex == 1,
          "Stale explicit RefID did not use deterministic name fallback");

    std::vector<Candidate> distantNamedTarget{
        MakeCandidate(0x80, "Lydia", 1500.0f)
    };
    result = Select("Hey Lydia", distantNamedTarget);
    Check(result.kind == SelectionKind::Narrator,
          "Named target outside direct-address radius was incorrectly selected");

    std::vector<Candidate> closeCandidates{
        MakeCandidate(0x90, "Lydia", 80.0f, true, true, true, 1.0f, true),
        MakeCandidate(0x91, "Alvor", 100.0f, true, true, true, -1.0f),
        MakeCandidate(0x92, "Gerdur", 200.0f, true, false),
        MakeCandidate(0x93, "Distant NPC", 201.0f),
        MakeCandidate(0x94, "Blocked NPC", 50.0f, true, true, false),
        MakeCandidate(0x95, "Ineligible NPC", 40.0f, false),
    };
    Request closeRequest{};
    closeRequest.utterance = "Hello";
    closeRequest.directAddressRadius = 200.0f;
    closeRequest.interactionRadius = 200.0f;
    result = Select(closeRequest, closeCandidates);
    Check(result.kind == SelectionKind::Candidate && result.candidateIndex == 0,
          "Close speech did not retain the crosshair responder");

    std::vector<std::size_t> closeAudience;
    std::vector<std::size_t> sneakingAudience;
    for (std::size_t index = 0; index < closeCandidates.size(); ++index) {
        const bool selected = index == result.candidateIndex;
        if (IsAudienceMember(closeCandidates[index], selected, 200.0f)) {
            closeAudience.push_back(index);
        }
        if (IsAudienceMember(closeCandidates[index], selected, 100.0f)) {
            sneakingAudience.push_back(index);
        }
    }
    Check(closeAudience == std::vector<std::size_t>({0, 1, 2}),
          "Close audience lost nearby hearers or admitted distant, blocked or ineligible NPCs");
    Check(sneakingAudience == std::vector<std::size_t>({0, 1}),
          "Sneaking Close audience did not respect the reduced radius");

    // Sleeping direct targets: every mode except Shout rejects instead of rerouting.
    std::vector<Candidate> sleepingCandidates{
        MakeCandidate(0xA0, "Sleeping Erik", 100.0f, true, false, true, 1.0f, true, true),
        MakeCandidate(0xA1, "Awake Sven", 300.0f)
    };
    Request sleepingCrosshairRequest{};
    sleepingCrosshairRequest.utterance = "Normal speech";
    sleepingCrosshairRequest.directAddressRadius = 1000.0f;
    sleepingCrosshairRequest.interactionRadius = 560.0f;
    sleepingCrosshairRequest.blockSleepingDirectTarget = true;
    result = Select(sleepingCrosshairRequest, sleepingCandidates);
    Check(result.kind == SelectionKind::Rejected && result.candidateIndex == 0 &&
              result.reason == "direct_target_sleeping",
          "Sleeping crosshair target was not rejected");

    sleepingCrosshairRequest.blockSleepingDirectTarget = false;
    result = Select(sleepingCrosshairRequest, sleepingCandidates);
    Check(result.kind == SelectionKind::Candidate && result.candidateIndex == 0 &&
              result.reason == "true_crosshair",
          "Shout mode did not reach the sleeping crosshair target");

    Request sleepingExplicitRequest{};
    sleepingExplicitRequest.utterance = "Normal speech";
    sleepingExplicitRequest.explicitTargetFormId = 0xA0;
    sleepingExplicitRequest.directAddressRadius = 1000.0f;
    sleepingExplicitRequest.interactionRadius = 560.0f;
    sleepingExplicitRequest.blockSleepingDirectTarget = true;
    result = Select(sleepingExplicitRequest, sleepingCandidates);
    Check(result.kind == SelectionKind::Rejected && result.candidateIndex == 0,
          "Sleeping explicit RefID target was not rejected");

    sleepingExplicitRequest.blockSleepingDirectTarget = false;
    result = Select(sleepingExplicitRequest, sleepingCandidates);
    Check(result.kind == SelectionKind::Candidate && result.candidateIndex == 0 &&
              result.reason == "explicit_ui_target",
          "Shout mode did not reach the sleeping explicit RefID target");

    Request sleepingNamedRequest{};
    sleepingNamedRequest.utterance = "Hey Sleeping Erik";
    sleepingNamedRequest.directAddressRadius = 1000.0f;
    sleepingNamedRequest.interactionRadius = 560.0f;
    sleepingNamedRequest.blockSleepingDirectTarget = true;
    result = Select(sleepingNamedRequest, sleepingCandidates);
    Check(result.kind == SelectionKind::Rejected && result.candidateIndex == 0,
          "Sleeping named target was rerouted instead of rejected");

    // Automatic routing keeps excluding sleepers rather than rejecting the request.
    std::vector<Candidate> sleepingAutomatic{
        MakeCandidate(0xA2, "Sleeping Nazeem", 50.0f, true, false, true, 1.0f, false, true),
        MakeCandidate(0xA3, "Awake Amren", 300.0f)
    };
    Request sleepingAutomaticRequest{};
    sleepingAutomaticRequest.utterance = "Normal speech";
    sleepingAutomaticRequest.directAddressRadius = 1000.0f;
    sleepingAutomaticRequest.interactionRadius = 560.0f;
    sleepingAutomaticRequest.blockSleepingDirectTarget = true;
    result = Select(sleepingAutomaticRequest, sleepingAutomatic);
    Check(result.kind == SelectionKind::Candidate && result.candidateIndex == 1 &&
              result.reason == "nearest_eligible",
          "Untargeted routing did not skip the nearer sleeping NPC");

    // An exact FormID remains authoritative even when another actor shares the name.
    std::vector<Candidate> sameNameSleepers{
        MakeCandidate(0xB0, "Erik", 100.0f, true, false, true, 1.0f, false, true),
        MakeCandidate(0xB1, "Erik", 300.0f)
    };
    Request sameNameExplicitRequest{};
    sameNameExplicitRequest.utterance = "Normal speech";
    sameNameExplicitRequest.explicitTargetFormId = 0xB0;
    sameNameExplicitRequest.explicitTargetName = "Erik";
    sameNameExplicitRequest.directAddressRadius = 1000.0f;
    sameNameExplicitRequest.interactionRadius = 560.0f;
    sameNameExplicitRequest.blockSleepingDirectTarget = true;
    result = Select(sameNameExplicitRequest, sameNameSleepers);
    Check(result.kind == SelectionKind::Rejected && result.candidateIndex == 0,
          "Sleeping explicit RefID rerouted to an awake same-name actor");

    Request sameNameOnlyRequest = sameNameExplicitRequest;
    sameNameOnlyRequest.explicitTargetFormId = 0;
    result = Select(sameNameOnlyRequest, sameNameSleepers);
    Check(result.kind == SelectionKind::Candidate && result.candidateIndex == 1 &&
              result.reason == "explicit_ui_target",
          "Name-only direct target did not prefer the awake same-name actor");

    Request sameNameNamedRequest{};
    sameNameNamedRequest.utterance = "Hey Erik";
    sameNameNamedRequest.directAddressRadius = 1000.0f;
    sameNameNamedRequest.interactionRadius = 560.0f;
    sameNameNamedRequest.blockSleepingDirectTarget = true;
    result = Select(sameNameNamedRequest, sameNameSleepers);
    Check(result.kind == SelectionKind::Candidate && result.candidateIndex == 1 &&
              result.reason == "explicit_npc_name",
          "Named address did not prefer the awake same-name actor over the closer sleeper");

    sameNameSleepers[1].autoEligible = false;
    sameNameSleepers[1].sleeping = true;
    result = Select(sameNameOnlyRequest, sameNameSleepers);
    Check(result.kind == SelectionKind::Rejected && result.candidateIndex == 0,
          "Name-only target was not rejected when every same-name actor is asleep");
    result = Select(sameNameNamedRequest, sameNameSleepers);
    Check(result.kind == SelectionKind::Rejected && result.candidateIndex == 0,
          "Named address was not rejected when every same-name actor is asleep");

    // The addressed name still decides who was meant, even when that actor is asleep.
    std::vector<Candidate> longestSleepingName{
        MakeCandidate(0xC0, "Lydia", 100.0f),
        MakeCandidate(0xC1, "Lydia Snow-Born", 150.0f, true, false, true, 1.0f, false, true)
    };
    Request longestSleepingRequest{};
    longestSleepingRequest.utterance = "Hey Lydia Snow-Born, listen";
    longestSleepingRequest.directAddressRadius = 1000.0f;
    longestSleepingRequest.interactionRadius = 560.0f;
    longestSleepingRequest.blockSleepingDirectTarget = true;
    result = Select(longestSleepingRequest, longestSleepingName);
    Check(result.kind == SelectionKind::Rejected && result.candidateIndex == 1,
          "Sleeping longest-name match fell back to a shorter awake name");

    // Other direct-target soft blockers still bypass as before.
    std::vector<Candidate> blockedButAwake{
        MakeCandidate(0xD0, "Cooldown NPC", 100.0f, true, false, false)
    };
    Request blockedButAwakeRequest{};
    blockedButAwakeRequest.utterance = "Normal speech";
    blockedButAwakeRequest.explicitTargetFormId = 0xD0;
    blockedButAwakeRequest.directAddressRadius = 1000.0f;
    blockedButAwakeRequest.interactionRadius = 560.0f;
    blockedButAwakeRequest.blockSleepingDirectTarget = true;
    result = Select(blockedButAwakeRequest, blockedButAwake);
    Check(result.kind == SelectionKind::Candidate && result.candidateIndex == 0 &&
              result.reason == "explicit_ui_target",
          "Sleep gating changed other direct-target soft-blocker behavior");

    std::vector<PresenceCandidate> presentCandidates{
        { 0x100, "Alvor", 200.0f, true, true, false },
        { 0x101, "Chicken", 100.0f, true, false, true },
        { 0x102, "Chicken", 120.0f, true, false, true },
        { 0x103, "Disabled NPC", 50.0f, false, true, false },
        { 0x104, "Distant NPC", 800.0f, true, true, false },
    };
    PresenceRequest presenceRequest{};
    presenceRequest.radius = 560.0f;
    auto present = SelectPresent(presenceRequest, presentCandidates);
    Check(present.size() == 1 && present[0] == 0,
          "Default presence policy did not keep only eligible dialogue actors");

    presenceRequest.includeAllRaces = true;
    present = SelectPresent(presenceRequest, presentCandidates);
    Check(present.size() == 2 && present[0] == 1 && present[1] == 0,
          "All-races presence policy did not include and deduplicate generic creatures");

    presenceRequest.includeIncidental = false;
    present = SelectPresent(presenceRequest, presentCandidates);
    Check(present.empty(), "Private presence policy included incidental actors");

    presenceRequest.includeIncidental = true;
    presenceRequest.limit = 1;
    present = SelectPresent(presenceRequest, presentCandidates);
    Check(present.size() == 1 && present[0] == 1,
          "Presence policy did not apply its deterministic actor limit");

    return 0;
}
