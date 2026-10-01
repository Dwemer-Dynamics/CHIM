#pragma once 

#include <cstdio>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include "json.hpp"
#include "PlayerConversationRouter.h"
#include "EventIdentityUtils.h"

// Track last event type for narration detection
extern std::string lastEventType;

namespace HTTPManager {
    void ShowPlaythroughNotices();
    nlohmann::json requestPlaythroughSession(const nlohmann::json& data);

    std::string base64_encode(const char* bytes_to_encode, size_t in_len);
    std::string base64_decode(const std::string& encodedString);

    void log(std::string msg);
    void stream(std::string msg);
    void stream(std::string msg, int rechatDepth);
    void streamPlayer(std::string msg, const PlayerConversationRoutingContext& context);
    
    void log(std::string msg, RE::Actor *actor);
    // Sends a single-data-field request with an identity captured when the event happened. The identity is
    // serialized into the request before queuing, so later game state cannot change it.
    void log(std::string msg, const EventIdentityUtils::EventIdentity& identity);
    // A world event about `target` (a victim, a reanimated corpse, a spell's target): the audience is the same
    // player hearing scope log(msg) captures, `target` is only the target_key role, never a witness or the
    // speaker, and the reply is not forced onto it.
    void logEventTarget(std::string msg, RE::Actor* target);

    // Call once from SKSE plugin load, which runs on the game thread.
    void RegisterGameThread();
    bool OnGameThread();

    // Game thread only. Who can hear `source` under the existing NPC speech hearing rules: managed actors near
    // the player while `source` is inside the auto hearing radius, then actors inside the MCM hearing
    // distance that SpatialAwareness lets communicate. `source` itself is not added; roles never add seats.
    struct HearingAudience {
        std::vector<std::string> names;
        EventIdentityUtils::EventIdentity identity;
        std::size_t playerNearbyCount = 0;
        bool sourceWithinAutoHearingRadius = false;
        void Add(const std::string& name, RE::Actor* actor);
    };
    HearingAudience CaptureHearingAudience(RE::Actor* source);
    // A nearby roster (infonpc_close) lists actors around the player: each listed actor plus the player.
    EventIdentityUtils::EventIdentity CaptureRoster(const std::vector<std::pair<std::string, RE::Actor*>>& actors);
    void log(std::string msg, std::string forcedActor);
    bool requestPlayerMenuTtsPlay(std::string msg);
    bool requestPlayerMenuTtsPlay(std::string msg, RE::Actor* actor);
    bool requestPlayerMenuTtsPlay(std::string msg, std::string forcedActor);
    std::string requestPlayerMenuTtsPlayResponse(std::string msg, std::string forcedActor);
    void stream(std::string msg, RE::Actor *actor);
    // rechatKey: the CapturedSpeaker::RechatKey a rechat stream completes under (the key its caller began).
    void stream(std::string msg, RE::Actor *actor, int rechatDepth, std::string rechatKey = {});

    // A combat bark keeps the epochs of the pass that scheduled it. Interaction stops and loads advance the
    // dialogue stop generation; RetireCombatBarks() advances the combat generation when combat ends.
    struct CombatBarkTicket {
        std::uint64_t dialogueStopGeneration = 0;
        std::uint64_t combatGeneration = 0;
        // Game-thread recheck run with the final eligibility snapshot.
        std::function<bool(RE::Actor*)> speakerEligible;
    };
    CombatBarkTicket CurrentCombatBarkTicket();
    bool CombatBarkTicketCurrent(const CombatBarkTicket& ticket);
    void RetireCombatBarks();
    // Game thread only: alive, loaded and in combat without searching (kSearchingInCombat) for its target.
    bool CombatBarkSpeakerInCombat(RE::Actor* actor);

    // Returns whether a targeted request was accepted for asynchronous delivery.
    bool streamForActor(std::string msg, RE::Actor* actor,
                        PlayerConversationRoutingPolicy::RequestEligibility eligibility, int rechatDepth = 0,
                        const CombatBarkTicket* combatBark = nullptr, std::string rechatKey = {});

    void postGameData(const std::string& endpoint, const nlohmann::json& data);
    // Completion runs after the queued request receives a success or failure result.
    void postGameData(const std::string& endpoint, const nlohmann::json& data,
                      std::function<void(bool)> completion);
    bool postGameDataSync(const std::string& endpoint, const nlohmann::json& data);
    std::string postGameDataResponse(const std::string& endpoint, const nlohmann::json& data, int timeoutMs = 5000);
    nlohmann::json postGameDataJson(const std::string& endpoint, const nlohmann::json& data, int timeoutMs = 5000);
    std::string getServerVersionRaw();

}
