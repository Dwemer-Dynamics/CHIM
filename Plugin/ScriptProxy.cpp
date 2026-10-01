#pragma once
#include "ChimInteraction.h"
#include <string>

#include "RE/Skyrim.h"
#include "json.hpp"
#include "EventIdentityUtils.h"

using json = nlohmann::json;

// Send a JSON string to Papyrus with a single cmdID
void SendAICommand(int cmdID, std::string jsonStr) {
    if (!ChimInteraction::Enabled()) return;
    auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
    if (!vm) return;

    std::string localCopy = (jsonStr);
    // Create FunctionArguments: single string parameter
    // Encode both cmdID and JSON if needed in JSONStr itself
    auto args = RE::MakeFunctionArguments(std::move(cmdID), std::move(localCopy));
    
   RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;

   SKSE::log::info("About to call AIAgentScriptProxy");
    // Dispatch static call to Papyrus
   vm->DispatchStaticCall("AIAgentScriptProxy",  // Papyrus script
                           "ExecuteCommand",      // Function name
                           args, callback);
}

// Parses a ScriptProxy command; strips any binding token it carries so only native dispatch can set one.
static bool ScriptProxyParse(const std::string& jsonStr, json& j) {
    try {
        j = json::parse(jsonStr);
    } catch (std::exception& e) {
        SKSE::log::error("Failed to parse AI JSON: {}", e.what());
        return false;
    }
    if (!j.is_object()) {
        SKSE::log::error("ScriptProxy JSON is not an object");
        return false;
    }
    j.erase(std::string(EventIdentityUtils::ScriptProxyBindingKey));
    return true;
}

// A queued server command whose actor parameters were bound on the game thread when it was queued.
void ScriptProxyRunBound(const std::string& jsonStr, std::uint64_t bindingToken) {
    json j;
    if (!ScriptProxyParse(jsonStr, j)) return;
    int cmdID = j.value("cmdID", 0);
    if (cmdID == 0) {
        SKSE::log::error("JSON missing cmdID");
        return;
    }
    j[std::string(EventIdentityUtils::ScriptProxyBindingKey)] = bindingToken;
    SendAICommand(cmdID, j.dump());
}

// --- Optional wrapper if AI sends full JSON with cmdID inside ---
// Unqueued callers (local UI) keep legacy resolution; declared actor identity cannot be honoured without the
// queue's game-thread binding, so such a command is refused rather than run unbound.
void ScriptProxyRun(const std::string& jsonStr) {
    json j;
    if (!ScriptProxyParse(jsonStr, j)) return;
    if (EventIdentityUtils::ParseScriptProxyIdentity(j).declared) {
        SKSE::log::warn("[SCRIPTPROXY_IDENTITY] Refusing unbound ScriptProxy command with declared actor identity");
        return;
    }

    // Extract cmdID from JSON (optional)
    int cmdID = j.value("cmdID", 0);
    if (cmdID == 0) {
        SKSE::log::error("JSON missing cmdID");
        return;
    }

    // Convert the JSON back to string to send to Papyrus
    std::string jsonToPapyrus = j.dump();

    // Send JSON string only
    SendAICommand(cmdID, jsonToPapyrus);
}
