#pragma once 

#include <cstdio>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include "json.hpp"
#include "PlayerConversationRouter.h"

// Track last event type for narration detection
extern std::string lastEventType;

namespace HTTPManager {

    std::string base64_encode(const char* bytes_to_encode, size_t in_len);
    std::string base64_decode(const std::string& encodedString);

    void log(std::string msg);
    void stream(std::string msg);
    void stream(std::string msg, int rechatDepth);
    void streamPlayer(std::string msg, const PlayerConversationRoutingContext& context);
    
    void log(std::string msg, RE::Actor *actor);
    void log(std::string msg, std::string forcedActor);
    bool requestPlayerMenuTtsPlay(std::string msg);
    bool requestPlayerMenuTtsPlay(std::string msg, RE::Actor* actor);
    bool requestPlayerMenuTtsPlay(std::string msg, std::string forcedActor);
    std::string requestPlayerMenuTtsPlayResponse(std::string msg, std::string forcedActor);
    void stream(std::string msg, RE::Actor *actor);
    void stream(std::string msg, RE::Actor *actor, int rechatDepth);

    void postGameData(const std::string& endpoint, const nlohmann::json& data);
    // Completion runs after the queued request receives a success or failure result.
    void postGameData(const std::string& endpoint, const nlohmann::json& data,
                      std::function<void(bool)> completion);
    bool postGameDataSync(const std::string& endpoint, const nlohmann::json& data);
    std::string postGameDataResponse(const std::string& endpoint, const nlohmann::json& data, int timeoutMs = 5000);
    nlohmann::json postGameDataJson(const std::string& endpoint, const nlohmann::json& data, int timeoutMs = 5000);
    std::string getServerVersionRaw();

}
