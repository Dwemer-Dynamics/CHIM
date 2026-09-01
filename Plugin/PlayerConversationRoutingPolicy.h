#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace PlayerConversationRoutingPolicy
{
    inline constexpr float kFieldOfViewCosine = 0.86f;

    // Distinguish direct player speech from autonomous events before applying scene safety.
    inline bool IsPlayerInitiatedRequest(std::string_view message)
    {
        const auto separator = message.find('|');
        const std::string_view eventType = message.substr(0, separator);
        return eventType == "inputtext" || eventType == "inputtext_s" ||
               eventType == "ginputtext" || eventType == "ginputtext_s" ||
               eventType == "narrator_inputtext";
    }

    struct AutomaticEligibilityFacts
    {
        bool conversationCooldown = false;
        bool hostile = false;
        bool autoAddHostile = false;
        bool inCombat = false;
        bool combatDialogueEnabled = false;
        bool restrained = false;
        bool unconscious = false;
        bool sleeping = false;
        bool inScene = false;
        bool sceneDialogueEnabled = false;
    };

    inline std::string_view GetAutomaticBlockReason(const AutomaticEligibilityFacts& facts)
    {
        if (facts.conversationCooldown) {
            return "cooldown";
        }
        if (facts.hostile && !facts.autoAddHostile) {
            return "hostile";
        }
        if (facts.inCombat && !facts.combatDialogueEnabled) {
            return "combat";
        }
        if (facts.restrained) {
            return "restrained";
        }
        if (facts.unconscious) {
            return "unconscious";
        }
        if (facts.sleeping) {
            return "sleeping";
        }
        if (facts.inScene && !facts.sceneDialogueEnabled) {
            return "scene";
        }
        return {};
    }

    enum class SelectionKind
    {
        Candidate,
        Narrator,
        // The player addressed a specific actor that must not be reached in this mode.
        Rejected,
        None
    };

    struct Candidate
    {
        std::uint32_t formId = 0;
        std::string name;
        float distance = 0.0f;
        float facingDot = -1.0f;
        bool hardEligible = false;
        bool directEligible = true;
        bool autoEligible = false;
        bool audible = false;
        bool trueCrosshair = false;
        bool sleeping = false;
    };

    // Hearing is independent of which eligible NPC is selected to reply.
    inline bool IsAudienceMember(const Candidate& candidate, bool selected, float radius)
    {
        return selected || (candidate.hardEligible && candidate.audible && candidate.distance <= radius);
    }

    struct Request
    {
        std::string utterance;
        std::uint32_t explicitTargetFormId = 0;
        std::string explicitTargetName;
        float directAddressRadius = 0.0f;
        float interactionRadius = 0.0f;
        bool narratorMode = false;
        bool everyoneMode = false;
        bool narratorGesture = false;
        // Shout is the only mode that can reach a sleeping direct target.
        bool blockSleepingDirectTarget = false;
    };

    struct Result
    {
        SelectionKind kind = SelectionKind::None;
        std::size_t candidateIndex = (std::numeric_limits<std::size_t>::max)();
        std::string reason;
        bool broadcast = false;
    };

    struct PresenceCandidate
    {
        std::uint32_t formId = 0;
        std::string name;
        float distance = 0.0f;
        bool hardEligible = false;
        bool dialogueRace = false;
        bool creature = false;
    };

    struct PresenceRequest
    {
        float radius = 0.0f;
        std::size_t limit = 32;
        bool includeAllRaces = false;
        bool includeIncidental = true;
    };

    inline std::string Normalize(std::string_view value)
    {
        std::string normalized;
        normalized.reserve(value.size());
        bool previousWasSpace = true;
        for (const unsigned char c : value) {
            if (std::isalnum(c)) {
                normalized.push_back(static_cast<char>(std::tolower(c)));
                previousWasSpace = false;
            } else if (!previousWasSpace) {
                normalized.push_back(' ');
                previousWasSpace = true;
            }
        }

        while (!normalized.empty() && normalized.back() == ' ') {
            normalized.pop_back();
        }
        return normalized;
    }

    inline std::string ExtractUtterance(std::string_view message)
    {
        std::string_view payload = message;
        std::size_t fieldStart = 0;
        for (int delimiter = 0; delimiter < 3; ++delimiter) {
            const auto separator = message.find('|', fieldStart);
            if (separator == std::string_view::npos) {
                payload = message;
                break;
            }
            fieldStart = separator + 1;
            if (delimiter == 2) {
                payload = message.substr(fieldStart);
            }
        }

        const auto speakerSeparator = payload.find(':');
        if (speakerSeparator != std::string_view::npos) {
            payload.remove_prefix(speakerSeparator + 1);
        }

        return Normalize(payload);
    }

    inline bool StartsWithToken(const std::string& text, const std::string& token)
    {
        if (!text.starts_with(token)) {
            return false;
        }
        return text.size() == token.size() || text[token.size()] == ' ';
    }

    inline bool WithinRadius(float distance, float radius)
    {
        return radius <= 0.0f || distance <= radius;
    }

    // Direct address bypasses soft eligibility, but only Shout can reach a sleeper.
    inline bool IsSleepingDirectTarget(const Request& request, const Candidate& candidate)
    {
        return request.blockSleepingDirectTarget && candidate.sleeping;
    }

    inline bool IsCloserDirectMatch(const std::vector<Candidate>& candidates, std::size_t index,
                                    std::size_t incumbent)
    {
        if (incumbent == (std::numeric_limits<std::size_t>::max)()) {
            return true;
        }
        const auto& lhs = candidates[index];
        const auto& rhs = candidates[incumbent];
        if (lhs.distance != rhs.distance) {
            return lhs.distance < rhs.distance;
        }
        return lhs.formId < rhs.formId;
    }

    inline std::vector<std::size_t> SelectPresent(
        const PresenceRequest& request, const std::vector<PresenceCandidate>& candidates)
    {
        if (!request.includeIncidental || request.limit == 0) {
            return {};
        }

        std::vector<std::size_t> eligible;
        eligible.reserve(candidates.size());
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            const auto& candidate = candidates[index];
            if (!candidate.hardEligible || Normalize(candidate.name).empty() ||
                !WithinRadius(candidate.distance, request.radius)) {
                continue;
            }
            if (!request.includeAllRaces && (!candidate.dialogueRace || candidate.creature)) {
                continue;
            }
            eligible.push_back(index);
        }

        std::sort(eligible.begin(), eligible.end(),
            [&](std::size_t left, std::size_t right) {
                const auto& lhs = candidates[left];
                const auto& rhs = candidates[right];
                if (lhs.distance != rhs.distance) {
                    return lhs.distance < rhs.distance;
                }
                return lhs.formId < rhs.formId;
            });

        std::vector<std::size_t> selected;
        selected.reserve(std::min(request.limit, eligible.size()));
        std::unordered_set<std::string> seenNames;
        for (const auto index : eligible) {
            const auto normalizedName = Normalize(candidates[index].name);
            if (!seenNames.insert(normalizedName).second) {
                continue;
            }
            selected.push_back(index);
            if (selected.size() >= request.limit) {
                break;
            }
        }
        return selected;
    }

    inline Result Select(const Request& request, const std::vector<Candidate>& candidates)
    {
        Result result{};
        result.broadcast = request.everyoneMode;

        const std::string utterance = Normalize(request.utterance);
        const bool beginsWithHey = StartsWithToken(utterance, "hey");
        const std::string addressedText =
            beginsWithHey && utterance.size() > 3 ? Normalize(std::string_view(utterance).substr(3)) : "";

        if (request.narratorMode || (beginsWithHey && StartsWithToken(addressedText, "narrator"))) {
            result.kind = SelectionKind::Narrator;
            result.reason = request.narratorMode ? "explicit_narrator_mode" : "explicit_narrator_name";
            result.broadcast = false;
            return result;
        }

        constexpr std::size_t noMatch = (std::numeric_limits<std::size_t>::max)();
        const auto reject = [&result](std::size_t index) {
            result.kind = SelectionKind::Rejected;
            result.candidateIndex = index;
            result.reason = "direct_target_sleeping";
            result.broadcast = false;
            return result;
        };

        if (request.explicitTargetFormId != 0 || !request.explicitTargetName.empty()) {
            std::size_t formMatch = noMatch;
            if (request.explicitTargetFormId != 0) {
                for (std::size_t index = 0; index < candidates.size(); ++index) {
                    const auto& candidate = candidates[index];
                    if (candidate.formId == request.explicitTargetFormId &&
                        candidate.hardEligible && candidate.directEligible &&
                        WithinRadius(candidate.distance, request.directAddressRadius)) {
                        formMatch = index;
                        break;
                    }
                }
            }

            const std::string normalizedExplicitName = Normalize(request.explicitTargetName);
            std::size_t fallbackMatch = noMatch;
            std::size_t sleepingFallbackMatch = noMatch;
            if (!normalizedExplicitName.empty()) {
                for (std::size_t index = 0; index < candidates.size(); ++index) {
                    const auto& candidate = candidates[index];
                    if (!candidate.hardEligible || !candidate.directEligible ||
                        !WithinRadius(candidate.distance, request.directAddressRadius) ||
                        Normalize(candidate.name) != normalizedExplicitName) {
                        continue;
                    }

                    auto& match = IsSleepingDirectTarget(request, candidate) ? sleepingFallbackMatch
                                                                            : fallbackMatch;
                    if (IsCloserDirectMatch(candidates, index, match)) {
                        match = index;
                    }
                }
            }

            if (formMatch != noMatch) {
                if (IsSleepingDirectTarget(request, candidates[formMatch])) {
                    return reject(formMatch);
                }
                result.kind = SelectionKind::Candidate;
                result.candidateIndex = formMatch;
                result.reason = "explicit_ui_target";
                return result;
            }

            // A name-only target may still resolve to an eligible same-name actor.
            if (fallbackMatch != noMatch) {
                result.kind = SelectionKind::Candidate;
                result.candidateIndex = fallbackMatch;
                result.reason = "explicit_ui_target";
                return result;
            }

            // Every actor matching the name-only direct intent is asleep.
            if (sleepingFallbackMatch != noMatch) {
                return reject(sleepingFallbackMatch);
            }
        }

        std::size_t namedMatch = noMatch;
        std::size_t namedMatchLength = 0;
        bool namedMatchSleeping = false;
        if (beginsWithHey && !addressedText.empty()) {
            for (std::size_t index = 0; index < candidates.size(); ++index) {
                const auto& candidate = candidates[index];
                const std::string normalizedName = Normalize(candidate.name);
                if (!candidate.hardEligible || !candidate.directEligible || normalizedName.empty() ||
                    !WithinRadius(candidate.distance, request.directAddressRadius) ||
                    !StartsWithToken(addressedText, normalizedName)) {
                    continue;
                }

                // The addressed name decides who was meant; sleep only breaks same-name ties.
                const bool sleeping = IsSleepingDirectTarget(request, candidate);
                const bool longerName = normalizedName.size() > namedMatchLength;
                const bool awakeDuplicate = normalizedName.size() == namedMatchLength &&
                    namedMatchSleeping && !sleeping;
                const bool closerDuplicate = normalizedName.size() == namedMatchLength &&
                    namedMatchSleeping == sleeping &&
                    IsCloserDirectMatch(candidates, index, namedMatch);
                if (longerName || awakeDuplicate || closerDuplicate) {
                    namedMatch = index;
                    namedMatchLength = normalizedName.size();
                    namedMatchSleeping = sleeping;
                }
            }
        }

        if (namedMatch != noMatch) {
            if (namedMatchSleeping) {
                return reject(namedMatch);
            }
            result.kind = SelectionKind::Candidate;
            result.candidateIndex = namedMatch;
            result.reason = "explicit_npc_name";
            return result;
        }

        for (std::size_t index = 0; index < candidates.size(); ++index) {
            const auto& candidate = candidates[index];
            if (candidate.trueCrosshair && candidate.hardEligible && candidate.directEligible &&
                WithinRadius(candidate.distance, request.directAddressRadius)) {
                if (IsSleepingDirectTarget(request, candidate)) {
                    return reject(index);
                }
                result.kind = SelectionKind::Candidate;
                result.candidateIndex = index;
                result.reason = "true_crosshair";
                return result;
            }
        }

        if (request.narratorGesture) {
            result.kind = SelectionKind::Narrator;
            result.reason = "narrator_camera_gesture";
            result.broadcast = false;
            return result;
        }

        const bool bareHey = utterance == "hey";
        if (bareHey) {
            std::size_t best = (std::numeric_limits<std::size_t>::max)();
            float bestScore = -(std::numeric_limits<float>::max)();
            for (std::size_t index = 0; index < candidates.size(); ++index) {
                const auto& candidate = candidates[index];
                if (!candidate.autoEligible || !candidate.audible ||
                    candidate.facingDot < kFieldOfViewCosine ||
                    !WithinRadius(candidate.distance, request.interactionRadius)) {
                    continue;
                }

                const float radius = std::max(request.interactionRadius, 1.0f);
                const float score = (candidate.facingDot * 2.0f) - (candidate.distance / radius);
                if (best == (std::numeric_limits<std::size_t>::max)() || score > bestScore ||
                    (score == bestScore && candidate.formId < candidates[best].formId)) {
                    best = index;
                    bestScore = score;
                }
            }

            if (best != (std::numeric_limits<std::size_t>::max)()) {
                result.kind = SelectionKind::Candidate;
                result.candidateIndex = best;
                result.reason = "bare_hey_fov";
                return result;
            }
        }

        std::size_t nearest = (std::numeric_limits<std::size_t>::max)();
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            const auto& candidate = candidates[index];
            if (!candidate.autoEligible || !candidate.audible ||
                !WithinRadius(candidate.distance, request.interactionRadius)) {
                continue;
            }
            if (nearest == (std::numeric_limits<std::size_t>::max)() ||
                candidate.distance < candidates[nearest].distance ||
                (candidate.distance == candidates[nearest].distance &&
                 candidate.formId < candidates[nearest].formId)) {
                nearest = index;
            }
        }

        if (nearest != (std::numeric_limits<std::size_t>::max)()) {
            result.kind = SelectionKind::Candidate;
            result.candidateIndex = nearest;
            result.reason = "nearest_eligible";
            return result;
        }

        result.kind = SelectionKind::Narrator;
        result.reason = "no_eligible_npc";
        result.broadcast = false;
        return result;
    }
}
