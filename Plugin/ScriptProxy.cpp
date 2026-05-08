#pragma once
#include <string>

#include "RE/Skyrim.h"
#include "json.hpp"

using json = nlohmann::json;

// Send a JSON string to Papyrus with a single cmdID
void SendAICommand(int cmdID, std::string jsonStr) {
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

// --- Optional wrapper if AI sends full JSON with cmdID inside ---
void ScriptProxyRun(const std::string& jsonStr) {
    json j;
    try {
        j = json::parse(jsonStr);
    } catch (std::exception& e) {
        SKSE::log::error("Failed to parse AI JSON: {}", e.what());
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
