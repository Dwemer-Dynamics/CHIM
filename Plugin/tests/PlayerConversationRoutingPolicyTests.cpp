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
                            bool crosshair = false)
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

    std::vector<Candidate> distantNamedTarget{
        MakeCandidate(0x80, "Lydia", 1500.0f)
    };
    result = Select("Hey Lydia", distantNamedTarget);
    Check(result.kind == SelectionKind::Narrator,
          "Named target outside direct-address radius was incorrectly selected");

    return 0;
}
