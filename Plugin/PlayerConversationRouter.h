#pragma once

#include "Globals.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

enum class PlayerConversationInputSource : std::uint8_t
{
    LegacyText,
    PrismaText,
    Voice
};

enum class PlayerConversationSpeechMode : std::uint8_t
{
    Standard,
    Whisper,
    Shout,
    Close
};

struct PlayerConversationRoutingContext
{
    PlayerConversationInputSource source = PlayerConversationInputSource::LegacyText;
    PlayerConversationSpeechMode mode = PlayerConversationSpeechMode::Standard;
    RE::FormID explicitTargetFormId = 0;
    std::string explicitTargetName;
    bool everyoneMode = false;
    bool narratorMode = false;
};

struct PlayerConversationPresentActor
{
    RE::FormID formId = 0;
    std::string name;
    float distance = 0.0f;
    bool managed = false;
    bool creature = false;
};

struct PlayerConversationRoutingResult
{
    std::shared_ptr<AIAgent> responder;
    RE::Actor* responderActor = nullptr;
    std::vector<RE::Actor*> presentPartyActors;
    std::vector<PlayerConversationPresentActor> presentActors;
    std::string responderName;
    std::vector<std::string> audience;
    std::string reason;
    std::string modeName;
    float listenerRadiusUnits = 0.0f;
    float audienceRadiusUnits = 0.0f;
    bool narrator = false;
    bool broadcast = false;
    bool direct = false;
};

namespace PlayerConversationRouter
{
    inline constexpr float kCloseRadiusUnits = 200.0f;

    PlayerConversationSpeechMode ParseSpeechMode(std::string_view mode);
    float GetCloseRadiusUnits(bool sneaking);
    PlayerConversationRoutingResult Resolve(const std::string& wireMessage,
                                            const PlayerConversationRoutingContext& context);
}
