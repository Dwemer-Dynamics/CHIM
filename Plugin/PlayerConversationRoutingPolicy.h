#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace PlayerConversationRoutingPolicy
{
    inline constexpr float kFieldOfViewCosine = 0.86f;

    enum class SelectionKind
    {
        Candidate,
        Narrator,
        None
    };

    struct Candidate
    {
        std::uint32_t formId = 0;
        std::string name;
        float distance = 0.0f;
        float facingDot = -1.0f;
        bool hardEligible = false;
        bool autoEligible = false;
        bool audible = false;
        bool trueCrosshair = false;
    };

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
    };

    struct Result
    {
        SelectionKind kind = SelectionKind::None;
        std::size_t candidateIndex = (std::numeric_limits<std::size_t>::max)();
        std::string reason;
        bool broadcast = false;
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

        std::size_t namedMatch = (std::numeric_limits<std::size_t>::max)();
        std::size_t namedMatchLength = 0;
        if (beginsWithHey && !addressedText.empty()) {
            for (std::size_t index = 0; index < candidates.size(); ++index) {
                const auto& candidate = candidates[index];
                const std::string normalizedName = Normalize(candidate.name);
                if (!candidate.hardEligible || normalizedName.empty() ||
                    !WithinRadius(candidate.distance, request.directAddressRadius) ||
                    !StartsWithToken(addressedText, normalizedName)) {
                    continue;
                }

                const bool longerName = normalizedName.size() > namedMatchLength;
                const bool closerDuplicate = normalizedName.size() == namedMatchLength &&
                    (namedMatch == (std::numeric_limits<std::size_t>::max)() ||
                     candidate.distance < candidates[namedMatch].distance ||
                     (candidate.distance == candidates[namedMatch].distance &&
                      candidate.formId < candidates[namedMatch].formId));
                if (longerName || closerDuplicate) {
                    namedMatch = index;
                    namedMatchLength = normalizedName.size();
                }
            }
        }

        if (namedMatch != (std::numeric_limits<std::size_t>::max)()) {
            result.kind = SelectionKind::Candidate;
            result.candidateIndex = namedMatch;
            result.reason = "explicit_npc_name";
            return result;
        }

        if (request.explicitTargetFormId != 0 || !request.explicitTargetName.empty()) {
            const std::string normalizedExplicitName = Normalize(request.explicitTargetName);
            for (std::size_t index = 0; index < candidates.size(); ++index) {
                const auto& candidate = candidates[index];
                const bool formMatch =
                    request.explicitTargetFormId != 0 && candidate.formId == request.explicitTargetFormId;
                const bool nameMatch =
                    request.explicitTargetFormId == 0 && !normalizedExplicitName.empty() &&
                    Normalize(candidate.name) == normalizedExplicitName;
                if ((formMatch || nameMatch) && candidate.hardEligible &&
                    WithinRadius(candidate.distance, request.directAddressRadius)) {
                    result.kind = SelectionKind::Candidate;
                    result.candidateIndex = index;
                    result.reason = "explicit_ui_target";
                    return result;
                }
            }
        }

        for (std::size_t index = 0; index < candidates.size(); ++index) {
            const auto& candidate = candidates[index];
            if (candidate.trueCrosshair && candidate.hardEligible &&
                WithinRadius(candidate.distance, request.directAddressRadius)) {
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
