#include "Misc.h"

#include "Commands.h"

#include "Globals.h"
#include "RE/Skyrim.h"
#include "Replacements.h"
#include "SPGResponse.h"
#include "json.hpp"
#include "HTTPManager.h"
#include "NonVR.h"
#include "HTTPUploader.h"
#include "ThreadPool.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>

using json = nlohmann::json;

namespace logger = SKSE::log;


std::list<RE::FormID> currentQuestsDetectedFormEditorID;
std::mutex currentQuestsDetectedFormEditorIDMutex;

std::uint32_t ConvertToTimestamp(const std::tm& time) {
    // Convert std::tm to std::time_t
    std::time_t timeValue = std::mktime(const_cast<std::tm*>(&time));

    // Convert std::time_t to std::uint32_t
    std::uint32_t timestamp = static_cast<std::uint32_t>(timeValue);

    return timestamp;
}

std::string getTimestampNanos() {
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch();
    auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    std::stringstream ss;
    ss << nanos;
    return ss.str();
}

std::string getCurrentTimeMillis() { return getTimestampNanos(); }

// Global variable to signal that a game has just been loaded
bool g_gameJustLoaded = false;

long long GetGameTimeStamp() {
    // Static variables to track timestamp history
    static long long lastTimestamp = 0;
    static long long continuityTimestamp = 0; // Used to maintain continuity when anomalies occur
    static long long lastGameTimestamp = 0;   // Last raw game timestamp for calculating relative changes
    static bool firstCall = true;
    static bool usingContinuityTimestamp = false;
    
    // Get the current game time from the Calendar
    auto calendar = RE::Calendar::GetSingleton();
    if (!calendar) {
        logger::warn("GetGameTimeStamp: Calendar singleton is null");
        return lastTimestamp > 0 ? lastTimestamp : 0;
    }
    
    // Calculate the current timestamp from the game
    long long currentGameTimestamp = calendar->GetCurrentGameTime() * 10000000;
    
    // Try to format the time string
    try {
        std::tm localTime = calendar->GetTime();
        
        // Validate tm structure values
        if (localTime.tm_year < 0 || localTime.tm_mon < 0 || localTime.tm_mon > 11 ||
            localTime.tm_mday < 1 || localTime.tm_mday > 31 ||
            localTime.tm_hour < 0 || localTime.tm_hour > 23 ||
            localTime.tm_min < 0 || localTime.tm_min > 59 ||
            localTime.tm_sec < 0 || localTime.tm_sec > 60) {
            logger::warn("GetGameTimeStamp: Invalid time values detected");
            return lastTimestamp > 0 ? lastTimestamp : 0;
        }
        
        char timeString[50];
        if (std::strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", &localTime) > 0) {
            //logger::debug("GetGameTimeStamp: Current game time: {}", timeString);
        }
    } catch (const std::exception& e) {
        logger::warn("GetGameTimeStamp: Error formatting time: {}", e.what());
    }
    
    // If this is the first call, initialize all timestamps
    if (firstCall) {
        lastTimestamp = currentGameTimestamp;
        lastGameTimestamp = currentGameTimestamp;
        continuityTimestamp = currentGameTimestamp;
        firstCall = false;
        logger::info("GetGameTimeStamp: First call, timestamp: {}", currentGameTimestamp);
        return currentGameTimestamp;
    }
    
    // If game was just loaded, allow any timestamp change and reset continuity
    if (g_gameJustLoaded) {
        logger::info("GetGameTimeStamp: Game loaded, allowing timestamp change from {} to {}", 
                    lastTimestamp, currentGameTimestamp);
        lastTimestamp = currentGameTimestamp;
        lastGameTimestamp = currentGameTimestamp;
        continuityTimestamp = currentGameTimestamp;
        usingContinuityTimestamp = false;
        g_gameJustLoaded = false;
        return currentGameTimestamp;
    }
    
    // Calculate the relative change in game timestamp
    long long gameTimestampDelta = currentGameTimestamp - lastGameTimestamp;
    
    // Check for anomalous changes
    long long absoluteDiff = std::abs(currentGameTimestamp - lastTimestamp);
    double percentChange = (lastTimestamp > 0) ? (static_cast<double>(absoluteDiff) / lastTimestamp) * 100.0 : 0.0;
    
    // Check if the timestamp goes backward (gameTimestampDelta < 0) without a game load
    if (gameTimestampDelta < 0) {
        if (!usingContinuityTimestamp) {
            logger::warn("GetGameTimeStamp: Anomalous timestamp change detected! Old: {}, New: {}, Diff: {}, Change: {:.2f}%", 
                        lastTimestamp, currentGameTimestamp, gameTimestampDelta, percentChange);
            logger::warn("GetGameTimeStamp: Switching to continuity timestamp mode");
            RE::DebugNotification("[CHIM] Timestamp changed unexpectedly. Using continuity timestamp mode.");
            usingContinuityTimestamp = true;
        }
        
        // Even in continuity mode, we want to incorporate the relative changes
        // If the game timestamp is increasing, we'll add that increase to our continuity timestamp
        if (gameTimestampDelta > 0) {
            // Use the smaller of the actual delta or 1000 to prevent huge jumps
            long long adjustedDelta = std::min(gameTimestampDelta, 1000LL);
            continuityTimestamp += adjustedDelta;
            logger::debug("GetGameTimeStamp: Continuity timestamp increased by {} to {}", adjustedDelta, continuityTimestamp);
        } else {
            // If the game timestamp decreased or stayed the same, increment by 100
            continuityTimestamp += 100;
            logger::debug("GetGameTimeStamp: Continuity timestamp increased by 100 to {} (game time fell behind)", continuityTimestamp);
        }
        
        // Update the last game timestamp for next delta calculation
        lastGameTimestamp = currentGameTimestamp;
        
        return continuityTimestamp;
    }
    
    // If we were using continuity timestamp but now the game's timestamp seems normal again
    if (usingContinuityTimestamp) {
        // Check if the current timestamp is now ahead of our continuity timestamp
        if (currentGameTimestamp > continuityTimestamp) {
            logger::info("GetGameTimeStamp: Returning to normal timestamp mode (current: {}, continuity: {})",
                        currentGameTimestamp, continuityTimestamp);
            usingContinuityTimestamp = false;
            lastTimestamp = currentGameTimestamp;
            lastGameTimestamp = currentGameTimestamp;
            continuityTimestamp = currentGameTimestamp;
            return currentGameTimestamp;
        } else {
            // Continue using continuity timestamp, but incorporate relative changes
            if (gameTimestampDelta > 0) {
                continuityTimestamp += gameTimestampDelta;
                logger::debug("GetGameTimeStamp: Continuity timestamp increased by {} to {}", gameTimestampDelta, continuityTimestamp);
            } else {
                // Use a more significant increment when game time falls behind
                continuityTimestamp += 100;
                logger::debug("GetGameTimeStamp: Continuity timestamp increased by 100 to {} (game time fell behind)", continuityTimestamp);
            }
            
            // Update the last game timestamp for next delta calculation
            lastGameTimestamp = currentGameTimestamp;
            
            return continuityTimestamp;
        }
    }
    
    // Normal case - update all timestamps and return the current game timestamp
    lastTimestamp = currentGameTimestamp;
    lastGameTimestamp = currentGameTimestamp;
    continuityTimestamp = currentGameTimestamp;
    return currentGameTimestamp;
}

// Function to be called when a game is loaded to allow timestamp changes
void ResetGameTimeStamp() {
    g_gameJustLoaded = true;
    logger::info("ResetGameTimeStamp: Game load flag set");
}

void replaceAll(std::string& str, const std::string& from, const std::string& to) {
    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();  // Handles case where 'to' is a substring of 'from'
    }
}

namespace {
    std::string ToLowerCopy(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }

    bool WeatherLooksFoggy(const RE::TESWeather* weatherData) {
        if (!weatherData) {
            return false;
        }

        const char* editorId = weatherData->GetFormEditorID();
        if (editorId && editorId[0] != '\0') {
            if (ToLowerCopy(editorId).find("fog") != std::string::npos) {
                return true;
            }
        }

        const char* displayName = weatherData->GetName();
        if (displayName && displayName[0] != '\0') {
            if (ToLowerCopy(displayName).find("fog") != std::string::npos) {
                return true;
            }
        }

        const auto& fogData = weatherData->fogData;
        const float maxFogPower = std::max(fogData.dayPower, fogData.nightPower);
        const float maxFogAmount = std::max(fogData.dayMax, fogData.nightMax);

        return maxFogAmount >= 0.6f && maxFogPower >= 2.0f;
    }
}

std::string GetCurrentWeatherDescription() {
    auto player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        return "unknown";
    }

    auto cell = player->GetParentCell();
    const bool isInterior = cell && cell->IsInteriorCell();

    std::string weather;
    const auto skyPtr = RE::Sky::GetSingleton();
    if (skyPtr && skyPtr->currentWeather) {
        RE::TESWeather* weatherData = skyPtr->currentWeather;
        const auto flags = weatherData->data.flags;

        auto appendLabel = [&](const char* label) {
            if (!weather.empty()) {
                weather.append(", ");
            }
            weather.append(label);
        };

        if ((flags & RE::TESWeather::WeatherDataFlag::kPleasant) != RE::TESWeather::WeatherDataFlag::kNone) {
            appendLabel("Pleasant");
        }
        if ((flags & RE::TESWeather::WeatherDataFlag::kCloudy) != RE::TESWeather::WeatherDataFlag::kNone) {
            appendLabel("Cloudy");
        }
        if ((flags & RE::TESWeather::WeatherDataFlag::kRainy) != RE::TESWeather::WeatherDataFlag::kNone) {
            appendLabel("Raining");
        }
        if ((flags & RE::TESWeather::WeatherDataFlag::kSnow) != RE::TESWeather::WeatherDataFlag::kNone) {
            appendLabel("Snowning");
        }
        if (WeatherLooksFoggy(weatherData)) {
            appendLabel("Foggy");
        }
    }

    if (weather.empty()) {
        return "";
    }

    if (isInterior) {
        return "outdoors it is " + weather;
    }

    return weather;
}

std::string BuildCurrentWorldContextDetails() {
    char timeDateString[200];
    RE::Calendar::GetSingleton()->GetTimeDateString(timeDateString, 200, true);

    std::string context = std::string("current date ") + timeDateString;
    const std::string weather = GetCurrentWeatherDescription();
    if (!weather.empty()) {
        context += ", current weather: " + weather;
    }

    return context;
}

void ProcedureSendActiveQuests() {
    auto storyTeller = RE::BGSStoryTeller::GetSingleton();

    auto playerInfo = RE::PlayerCharacter::GetSingleton()->GetPlayerRuntimeData();
    
    
    std::string briefing;

    json briefing2;

    std::lock_guard<std::mutex> lock(currentQuestsDetectedFormEditorIDMutex);

    for (int i = 0; i < storyTeller->infoClearQuests.size(); i++) {
    //for (int i = 0; i < storyTeller->queuedStartQuests.size(); i++) {
        briefing.assign("");

        try {
            RE::TESQuest* quest = storyTeller->infoClearQuests[i];
            
            if (std::string(quest->GetFullName()).contains("Wolf")) {
                logger::info("Quest  {}", quest->GetFullName());    
            }
            if (!quest->IsActive()) continue;
            if (quest->IsCompleted()) continue;
            if (!quest->IsEnabled()) continue;

            json sData;

            sData["name"] = quest->GetName();
            sData["formId"] = quest->formEditorID;
            sData["stage"] = quest->currentStage;

            RE::BSSimpleList<RE::BGSQuestObjective*>* objetives = &quest->objectives;
            RE::BGSQuestObjective* lastObjective = nullptr;

            auto aliases = quest->aliases;

            std::unordered_map<uint32_t, std::string> aliasResolvePre;
            std::unordered_map<std::string, std::string> aliasResolveFinal;

            for (auto iter = aliases.begin(); iter != aliases.end(); ++iter) {
                RE::BGSBaseAlias* singlealias = *iter;
                aliasResolvePre[singlealias->aliasID] = std::string(singlealias->aliasName);
            };
            /* auto topics = quest->topics;
            for (auto iter = topics->begin(); iter != topics->end(); ++iter) {
                RE::TESTopic* topic = *iter;
                logger::info("{}", topic->GetName());

            };*/

            auto instanceData = quest->instanceData;
            for (auto iter = instanceData.begin(); iter != instanceData.end(); ++iter) {
                RE::BGSQuestInstanceText* qitext = *iter;
                for (auto iter2 = qitext->stringData.begin(); iter2 != qitext->stringData.end(); ++iter2) {
                    RE::BGSQuestInstanceText::StringData* singleData = iter2;

                    RE::TESForm* resolvedData = RE::TESForm::LookupByID(iter2->fullNameFormID);

                    // logger::info("{} instanceData {} resolvedData {} ", iter2->aliasID,
                    // iter2->fullNameFormID,resolvedData->GetName());
                    if (aliasResolvePre.contains(iter2->aliasID))
                        aliasResolveFinal[aliasResolvePre[iter2->aliasID]] = std::string(resolvedData->GetName());
                };
                for (auto iter2 = qitext->valueData.begin(); iter2 != qitext->valueData.end(); ++iter2) {
                    RE::BGSQuestInstanceText::GlobalValueData* valueData = iter2;
                    // logger::info("{} valuedata {} ", iter2->global->formID, iter2->value);
                };
            };

            

            for (auto iter = objetives->begin(); iter != objetives->end(); ++iter) {
                RE::BGSQuestObjective* stage = *iter;

                // if (stage->index <= quest->currentStage) {
                if (stage->index < 300) {
                    if (stage->state.get() != RE::QUEST_OBJECTIVE_STATE::kDisplayed) continue;

                    RE::TESQuestTarget** targets = stage->targets;
                    if (targets) {
                        auto target = *targets;

                        if (target) {
                            // sData["currentTarget"] = target->alias;
                        }
                    }
                    RE::BSString bsString(stage->displayText);
                    ReplaceTagsInQuestText(&bsString, quest, quest->currentInstanceID);
                    std::string element = "";
                    auto stateX = stage->state.get();

                    briefing.append(bsString.c_str()).append(".\n");
                    element.append(bsString.c_str());

                    if (stage->index == quest->currentStage) {
                        aliasResolveFinal[element]=element;
                    }
                }
            }
            sData["currentbrief2"] = aliasResolveFinal;
            sData["currentbrief"] = briefing;
            /* std::string altBriefing = GetCurrentDescriptionWithReplacedTags(quest, quest->currentInstanceID);
            if (!altBriefing.empty())
                sData["currentbrief2"] = altBriefing;*/

            /* if (quest->waitingStages) {
                for (auto iter = quest->waitingStages->begin(); iter != quest->waitingStages->end(); ++iter) {
                    RE::TESQuestStage* stage = *iter;
                    stage->data.

                }

            }*/

            HTTPManager::log(std::format("_quest|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(), sData.dump()));

            currentQuestsDetectedFormEditorID.push_back(quest->GetFormID());

            

        } catch (nlohmann::json_abi_v3_11_2::detail::type_error& ex) {
            logger::info("Error sending quest. Review encoding, {}", ex.what());
        }
    }

    ThreadPool::getInstance().enqueue("FillLogJournal", []() {
        std::lock_guard<std::mutex> lock(currentQuestsDetectedFormEditorIDMutex);
        for (RE::FormID questFormId : currentQuestsDetectedFormEditorID) {
            if (questFormId > 0) {
                auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                auto args = RE::MakeFunctionArguments(std::move(questFormId));
                RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                    "AIAgentAIMind", "FillLogJournal", args, callback);

                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        currentQuestsDetectedFormEditorID.clear();
    });
}

std::string GetPlayerLocation() {
    auto player = RE::PlayerCharacter::GetSingleton();
    std::string location;
    RE::BGSLocation* locpointer = nullptr;
    locpointer = player->GetCurrentLocation();
    auto cell = player->GetParentCell();

    if (cell) {
        if (locpointer && cell->IsExteriorCell()) {
            /*
            logger::info("Player location lookup for player location {},{:X}", locpointer->GetFullName(),
                         locpointer->GetFormID());
            logger::info("Player cell lookup for player location {},{:X}, IsExteriorCell {}", cell->GetName(),
                         cell->GetFormID(), cell->IsExteriorCell());
            */
            location.assign(player->GetCurrentLocation()->GetFullName());
            if (player->GetCurrentLocation()->parentLoc == nullptr)
                location.append(" outdoors");
            else {
                if (cell && cell->IsInteriorCell()) {
                    location.append(" interior");
                } else {
                    auto cellRtData = cell->GetRuntimeData();
                    if (cellRtData.worldSpace->GetFormID() == RE::FormID(0x0000003C)) {  // Skyrim worldspace
                        location.append(" outdoors");
                    } else {
                        location.append("");
                    }
                }

                location.append(" ,Hold: ");
                location.append(player->GetCurrentLocation()->parentLoc->GetFullName());
            }

        } else {
            locpointer = cell->GetLocation();
            //logger::info("Cell location lookup for player location {},{:X}",cell->GetFullName(),cell->GetFormID());
            std::string cellName(cell->GetFullName());
            if (!cellName.empty()) {
                location.assign(cellName);
            }
            
            if (locpointer) {
                if (location.empty())
                    location.assign(locpointer->GetFullName());

                if (locpointer->parentLoc == nullptr)
                    ;
                else {
                    location.append(" ,Hold: ");
                    location.append(locpointer->parentLoc->GetFullName());
                }
            }

            RE::TESWorldSpace* worldPointer = player->GetWorldspace();
            if (worldPointer) {
                location.assign(worldPointer->GetFullName());
                for (auto iter = worldPointer->locationMap.begin(); iter != worldPointer->locationMap.end(); ++iter) {
                    auto entry = *iter;
                    // logger::info("{} {}", entry.first, entry.second->GetName());
                }
                location.append(", outdoors");
            }
        }
    } else {
        location.assign("Unknown");
    }
    return location;
}

// From
// https://github.com/alexsylex/CompassNavigationOverhaul/blob/733365d5f181154ac25ee2a098a76800020be7d7/source/RE/T/TESQuest.cpp

void ReplaceTagsInQuestText(RE::BSString* a_text, const RE::TESQuest* a_quest, std::uint32_t a_questInstanceId) {
    using func_t = decltype(ReplaceTagsInQuestText);
    REL::Relocation<func_t> func{RELOCATION_ID(23429, 23897)};

    func(a_text, a_quest, a_questInstanceId);
}

std::string GetCurrentDescriptionWithReplacedTags(const RE::TESQuest* a_quest, std::uint32_t a_questInstanceId) {
    /*using func_t = void (RE::TESQuest::*)(RE::BSString&, std::uint32_t) const;
    REL::Relocation<func_t> func{RELOCATION_ID(24549, 25078)};

    RE::BSString retVal;
    func(a_quest, retVal, a_questInstanceId);
    return std::string(retVal);
    */

    return "unknown";
}

// Not in use now, just because is handy
RE::BSContainer::ForEachResult cellIteratorCallBack(RE::TESObjectREFR& object) {
    RE::TESForm* baseForm = object.GetBaseObject();
    if (baseForm->formType == RE::FormType::NPC) {
        const char* currentNPC = object.GetName();
        RE::Actor* actorNpc = object.As<RE::Actor>();
        if (actorNpc) {
            RE::BGSScene* scene = actorNpc->GetCurrentScene();
            if (scene) {
                /*
                RE::ActorState* actState = actorNpc->AsActorState();
                if (actState) {
                    if (actState->actorState2.talkingToPlayer)  // Are you talking to me?
                        logger::info("Iterate over TESObjectsInArea {}", actorNpc->GetName());
                }*/
            }
        }
    }

    return RE::BSContainer::ForEachResult::kContinue;
}

std::string GetCombatStateString(const RE::TESCombatEvent* event) {
    switch (event->newState.get()) {
        case RE::ACTOR_COMBAT_STATE::kNone:
            return "None";
        case RE::ACTOR_COMBAT_STATE::kCombat:
            return "Combat";
        case RE::ACTOR_COMBAT_STATE::kSearching:
            return "Searching";
        default:
            return "Unknown";
    }
}


std::string GetPlayerName() {
    return AIAgentManager::getInstance().getPlayerName();

}

void InterruptNPC(RE::Actor* actor, AIAgent* agent) {
    logger::info("Setting dialogue busy for  {}, isPlayerTeammate {}", actor->GetDisplayFullName(),
                 actor->IsPlayerTeammate());

    actor->PauseCurrentDialogue();  // Needed?
    // Beta test this requests.
    // listenerPtr->StopInteractingQuick(true);  // CTD with OAR?
    actor->EndDialogue();  // Seems to flush pending skyrim dialog
    // listenerPtr->SetSpeakingDone(true);       // CTD with OAR?

    bool resetMouth = false;

    auto dialogueItemTarget = actor->GetActorRuntimeData().dialogueItemTarget;
    if (dialogueItemTarget && dialogueItemTarget.get()) {
        resetMouth = true;
        logger::info("Seems {} is talking to {} ", agent->getActorName(), dialogueItemTarget.get()->GetName());
    }

    if (actor->GetCurrentScene()) {
        // Npc it's on scene, will call prepare dialogue to reset mouth
        resetMouth = true;
        if (agent) agent->setOnScene(true);
    }

    // Mark agent to restore voic eand states
    if (agent) {
        agent->setClean(false);
        agent->setRestored(false);
    }

    // Clears voicetype. NPC will stop talking
    actor->GetActorBase()->voiceType = NullVoiceType->As<RE::BGSVoiceType>();

    if (resetMouth) {
        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
        auto args = RE::MakeFunctionArguments(std::move(actor));
        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentAIMind", "PrepareForDialog",
                                                                                   args, callback);
    }
}

// Helper function to validate import types
bool IsValidImportType(const std::string& fileType) {
    static const std::vector<std::string> validTypes = {
        "biography_import",
        "oghma_import", 
        "dynamic_oghma_import",
        "description_import",
        "custom_action_import",
        "traditional_quest_import"
    };
    
    return std::find(validTypes.begin(), validTypes.end(), fileType) != validTypes.end();
}

// CSV Import Data Detection Functions
void DetectAndUploadImportDataFiles() {
    logger::info("Starting CSV import data detection...");
    
    try {
        std::string chimPath = "Data/CHIM";
        std::vector<std::string> importDataFiles = FindImportDataFiles(chimPath);
        std::vector<std::string> customActionFiles = FindCustomActionImportFiles(chimPath);
        std::vector<std::string> oghmaFiles = FindOghmaImportFiles(chimPath);
        std::vector<std::string> dynamicOghmaFiles = FindDynamicOghmaImportFiles(chimPath);
        std::vector<std::string> itemFiles = FindItemImportFiles(chimPath);
        std::vector<std::string> traditionalQuestFiles = FindTraditionalQuestImportFiles(chimPath);
        
        if (importDataFiles.empty() && customActionFiles.empty() && oghmaFiles.empty() && dynamicOghmaFiles.empty() && itemFiles.empty() && traditionalQuestFiles.empty()) {
            logger::info("No import data CSV files found in CHIM directory");
            return;
        }
        
        logger::info(
            "Found {} biography files, {} custom action files, {} oghma files, {} dynamic oghma files, {} item files, and {} traditional quest files",
            importDataFiles.size(),
            customActionFiles.size(),
            oghmaFiles.size(),
            dynamicOghmaFiles.size(),
            itemFiles.size(),
            traditionalQuestFiles.size()
        );
        
        // Process biography files
        for (const auto& filePath : importDataFiles) {
            const std::string fileType = "biography_import";
            if (!IsValidImportType(fileType)) {
                logger::error("Invalid import type: {}", fileType);
                continue;
            }
            
            std::string csvContent = ParseImportDataCSV(filePath);
            if (!csvContent.empty()) {
                std::string filename = std::filesystem::path(filePath).filename().string();
                
                // Use new CSV upload function instead of HTTPManager::log()
                std::string response = HTTPUploader::UploadCSVFile(csvContent, filename, fileType);
                
                if (!response.empty() && response != "...") {
                    logger::info("Successfully uploaded biography import file: {} (Response: {})", filename, response);
                } else {
                    logger::error("Failed to upload biography import file: {}", filename);
                }
            }
        }
        
        // Process custom action files
        ProcessCustomActionImportFiles(customActionFiles);

        // Process oghma files
        ProcessOghmaImportFiles(oghmaFiles);
        
        // Process dynamic oghma files
        ProcessDynamicOghmaImportFiles(dynamicOghmaFiles);
        
        // Process item files
        ProcessItemImportFiles(itemFiles);

        // Process traditional quest files
        ProcessTraditionalQuestImportFiles(traditionalQuestFiles);
        
    } catch (const std::exception& e) {
        logger::error("Error during import data detection: {}", e.what());
    }
}

std::vector<std::string> FindCustomActionImportFiles(const std::string& directoryPath) {
    std::vector<std::string> customActionFiles;

    try {
        if (!std::filesystem::exists(directoryPath)) {
            logger::warn("CHIM directory does not exist: {}", directoryPath);
            return customActionFiles;
        }

        for (const auto& entry : std::filesystem::directory_iterator(directoryPath)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                if (filename.ends_with("_actions.csv")) {
                    customActionFiles.push_back(entry.path().string());
                    logger::debug("Found custom action import file: {}", filename);
                }
            }
        }
    } catch (const std::exception& e) {
        logger::error("Error scanning for custom action import files: {}", e.what());
    }

    return customActionFiles;
}

void ProcessCustomActionImportFiles(const std::vector<std::string>& customActionFiles) {
    for (const auto& filePath : customActionFiles) {
        try {
            const std::string fileType = "custom_action_import";
            if (!IsValidImportType(fileType)) {
                logger::error("Invalid import type: {}", fileType);
                continue;
            }

            std::string csvContent = ParseImportDataCSV(filePath);
            if (!csvContent.empty()) {
                std::string filename = std::filesystem::path(filePath).filename().string();
                std::string response = HTTPUploader::UploadCSVFile(csvContent, filename, fileType);

                if (!response.empty() && response != "...") {
                    logger::info("Successfully uploaded custom action import file: {} (Response: {})", filename, response);
                } else {
                    logger::error("Failed to upload custom action import file: {}", filename);
                }
            }
        } catch (const std::exception& e) {
            logger::error("Error processing custom action file {}: {}", filePath, e.what());
        }
    }
}

std::vector<std::string> FindImportDataFiles(const std::string& directoryPath) {
    std::vector<std::string> importDataFiles;
    
    try {
        if (!std::filesystem::exists(directoryPath)) {
            logger::warn("CHIM directory does not exist: {}", directoryPath);
            return importDataFiles;
        }
        
        for (const auto& entry : std::filesystem::directory_iterator(directoryPath)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                if (filename.ends_with("_bios.csv")) {
                    importDataFiles.push_back(entry.path().string());
                    logger::debug("Found import data file: {}", filename);
                }
            }
        }
    } catch (const std::exception& e) {
        logger::error("Error scanning for import data files: {}", e.what());
    }
    
    return importDataFiles;
}

std::string ParseImportDataCSV(const std::string& filePath) {
    try {
        // Validate file extension first
        std::filesystem::path path(filePath);
        std::string extension = path.extension().string();
        if (extension != ".csv") {
            logger::error("Invalid file extension for import data file: {} (expected .csv)", filePath);
            return "";
        }
        
        std::ifstream file(filePath);
        if (!file.is_open()) {
            logger::error("Failed to open import data file: {}", filePath);
            return "";
        }
        
        // Check file size first
        file.seekg(0, std::ios::end);
        std::streamsize fileSize = file.tellg();
        file.seekg(0, std::ios::beg);
        
        // Hard limit: Max 10MB as per requirements
        const std::streamsize MAX_FILE_SIZE = 10 * 1024 * 1024; // 10MB
        if (fileSize > MAX_FILE_SIZE) {
            logger::error("CSV file {} is too large ({} bytes). Maximum allowed size is 10MB.", 
                        filePath, fileSize);
            RE::DebugNotification(std::format("[CHIM] CSV file is too large ({}MB). Max size is 10MB.",
                                             fileSize / (1024 * 1024)).c_str());
            return "";
        }
        
        // Warn about large files (>5MB) but allow them
        const std::streamsize WARN_SIZE = 5 * 1024 * 1024; // 5MB
        if (fileSize > WARN_SIZE) {
            logger::warn("CSV file {} is large ({} bytes). This may cause upload issues. Consider splitting into smaller files.", 
                        filePath, fileSize);
            RE::DebugNotification(std::format("[CHIM] Large CSV file detected ({}MB). Upload may be slow.",
                                             fileSize / (1024 * 1024)).c_str());
        }
        
        std::ostringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();
        
        // Basic validation - check if file has content
        if (content.empty()) {
            logger::warn("Import data file is empty: {}", filePath);
            return "";
        }
        
        logger::debug("Successfully parsed import data file: {} ({} bytes)", filePath, content.size());
        return content;
        
    } catch (const std::exception& e) {
        logger::error("Error parsing import data file {}: {}", filePath, e.what());
        return "";
    }
}

// Oghma Import Detection Functions
std::vector<std::string> FindOghmaImportFiles(const std::string& directoryPath) {
    std::vector<std::string> oghmaFiles;
    
    try {
        if (!std::filesystem::exists(directoryPath)) {
            logger::warn("CHIM directory does not exist: {}", directoryPath);
            return oghmaFiles;
        }
        
        for (const auto& entry : std::filesystem::directory_iterator(directoryPath)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                if (filename.ends_with("_oghma.csv")) {
                    oghmaFiles.push_back(entry.path().string());
                    logger::debug("Found oghma import file: {}", filename);
                }
            }
        }
    } catch (const std::exception& e) {
        logger::error("Error scanning for oghma import files: {}", e.what());
    }
    
    return oghmaFiles;
}

void ProcessOghmaImportFiles(const std::vector<std::string>& oghmaFiles) {
    for (const auto& filePath : oghmaFiles) {
        try {
            const std::string fileType = "oghma_import";
            if (!IsValidImportType(fileType)) {
                logger::error("Invalid import type: {}", fileType);
                continue;
            }
            
            std::string csvContent = ParseImportDataCSV(filePath);
            if (!csvContent.empty()) {
                std::string filename = std::filesystem::path(filePath).filename().string();
                
                // Use new CSV upload function instead of HTTPManager::log()
                std::string response = HTTPUploader::UploadCSVFile(csvContent, filename, fileType);
                
                if (!response.empty() && response != "...") {
                    logger::info("Successfully uploaded oghma import file: {} (Response: {})", filename, response);
                } else {
                    logger::error("Failed to upload oghma import file: {}", filename);
                }
            }
        } catch (const std::exception& e) {
            logger::error("Error processing oghma file {}: {}", filePath, e.what());
        }
    }
}

std::vector<std::string> FindDynamicOghmaImportFiles(const std::string& directoryPath) {
    std::vector<std::string> dynamicOghmaFiles;
    
    try {
        if (!std::filesystem::exists(directoryPath)) {
            logger::warn("CHIM directory does not exist: {}", directoryPath);
            return dynamicOghmaFiles;
        }
        
        for (const auto& entry : std::filesystem::directory_iterator(directoryPath)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                if (filename.ends_with("_dynamicoghma.csv")) {
                    dynamicOghmaFiles.push_back(entry.path().string());
                    logger::debug("Found dynamic oghma import file: {}", filename);
                }
            }
        }
    } catch (const std::exception& e) {
        logger::error("Error scanning for dynamic oghma import files: {}", e.what());
    }
    
    return dynamicOghmaFiles;
}

void ProcessDynamicOghmaImportFiles(const std::vector<std::string>& dynamicOghmaFiles) {
    for (const auto& filePath : dynamicOghmaFiles) {
        try {
            const std::string fileType = "dynamic_oghma_import";
            if (!IsValidImportType(fileType)) {
                logger::error("Invalid import type: {}", fileType);
                continue;
            }
            
            std::string csvContent = ParseImportDataCSV(filePath);
            if (!csvContent.empty()) {
                std::string filename = std::filesystem::path(filePath).filename().string();
                
                // Use new CSV upload function instead of HTTPManager::log()
                std::string response = HTTPUploader::UploadCSVFile(csvContent, filename, fileType);
                
                if (!response.empty() && response != "...") {
                    logger::info("Successfully uploaded dynamic oghma import file: {} (Response: {})", filename, response);
                } else {
                    logger::error("Failed to upload dynamic oghma import file: {}", filename);
                }
            }
        } catch (const std::exception& e) {
            logger::error("Error processing dynamic oghma file {}: {}", filePath, e.what());
        }
    }
}

// Item Import Detection Functions
std::vector<std::string> FindItemImportFiles(const std::string& directoryPath) {
    std::vector<std::string> itemFiles;
    
    try {
        if (!std::filesystem::exists(directoryPath)) {
            logger::warn("CHIM directory does not exist: {}", directoryPath);
            return itemFiles;
        }
        
        for (const auto& entry : std::filesystem::directory_iterator(directoryPath)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                if (filename.ends_with("_descriptions.csv")) {
                    itemFiles.push_back(entry.path().string());
                    logger::debug("Found description import file: {}", filename);
                }
            }
        }
    } catch (const std::exception& e) {
        logger::error("Error scanning for item import files: {}", e.what());
    }
    
    return itemFiles;
}

void ProcessItemImportFiles(const std::vector<std::string>& itemFiles) {
    for (const auto& filePath : itemFiles) {
        try {
            const std::string fileType = "description_import";
            if (!IsValidImportType(fileType)) {
                logger::error("Invalid import type: {}", fileType);
                continue;
            }
            
            std::string csvContent = ParseImportDataCSV(filePath);
            if (!csvContent.empty()) {
                std::string filename = std::filesystem::path(filePath).filename().string();
                
                // Use new CSV upload function instead of HTTPManager::log()
                std::string response = HTTPUploader::UploadCSVFile(csvContent, filename, fileType);
                
                if (!response.empty() && response != "...") {
                    logger::info("Successfully uploaded item import file: {} (Response: {})", filename, response);
                } else {
                    logger::error("Failed to upload item import file: {}", filename);
                }
            }
        } catch (const std::exception& e) {
            logger::error("Error processing item file {}: {}", filePath, e.what());
        }
    }
}

// Traditional Quest Import Detection Functions
std::vector<std::string> FindTraditionalQuestImportFiles(const std::string& directoryPath) {
    std::vector<std::string> traditionalQuestFiles;

    try {
        if (!std::filesystem::exists(directoryPath)) {
            logger::warn("CHIM directory does not exist: {}", directoryPath);
            return traditionalQuestFiles;
        }

        for (const auto& entry : std::filesystem::directory_iterator(directoryPath)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                if (filename.ends_with("_tradquest.csv")) {
                    traditionalQuestFiles.push_back(entry.path().string());
                    logger::debug("Found traditional quest import file: {}", filename);
                }
            }
        }
    } catch (const std::exception& e) {
        logger::error("Error scanning for traditional quest import files: {}", e.what());
    }

    return traditionalQuestFiles;
}

void ProcessTraditionalQuestImportFiles(const std::vector<std::string>& traditionalQuestFiles) {
    for (const auto& filePath : traditionalQuestFiles) {
        try {
            const std::string fileType = "traditional_quest_import";
            if (!IsValidImportType(fileType)) {
                logger::error("Invalid import type: {}", fileType);
                continue;
            }

            std::string csvContent = ParseImportDataCSV(filePath);
            if (!csvContent.empty()) {
                std::string filename = std::filesystem::path(filePath).filename().string();
                std::string response = HTTPUploader::UploadCSVFile(csvContent, filename, fileType);

                if (!response.empty() && response != "...") {
                    logger::info("Successfully uploaded traditional quest import file: {} (Response: {})", filename, response);
                } else {
                    logger::error("Failed to upload traditional quest import file: {}", filename);
                }
            }
        } catch (const std::exception& e) {
            logger::error("Error processing traditional quest file {}: {}", filePath, e.what());
        }
    }
}

// Voice CSV Detection and Management Functions
static std::unordered_map<std::string, std::string> csvVoiceCache;
static bool csvVoiceCacheLoaded = false;
static std::mutex csvVoiceCacheMutex;

std::vector<std::string> FindVoiceCSVFiles(const std::string& directoryPath) {
    std::vector<std::string> voiceCSVFiles;
    
    logger::info("[VOICE_CSV] Starting search for voice CSV files in directory: {}", directoryPath);
    
    try {
        if (!std::filesystem::exists(directoryPath)) {
            logger::warn("[VOICE_CSV] CHIM directory does not exist: {}", directoryPath);
            return voiceCSVFiles;
        }
        
        for (const auto& entry : std::filesystem::directory_iterator(directoryPath)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                if (filename.ends_with("_voices.csv")) {
                    voiceCSVFiles.push_back(entry.path().string());
                    logger::info("[VOICE_CSV] Found voice CSV file: {}", filename);
                }
            }
        }
        
        logger::info("[VOICE_CSV] Found {} voice CSV files total", voiceCSVFiles.size());
        
    } catch (const std::exception& e) {
        logger::error("[VOICE_CSV] Error scanning for voice CSV files: {}", e.what());
    }
    
    return voiceCSVFiles;
}

std::unordered_map<std::string, std::string> ParseVoiceCSV(const std::string& filePath) {
    std::unordered_map<std::string, std::string> voiceMap;
    
    logger::info("[VOICE_CSV] Starting to parse voice CSV file: {}", filePath);
    
    try {
        std::ifstream file(filePath);
        if (!file.is_open()) {
            logger::error("[VOICE_CSV] Failed to open voice CSV file: {}", filePath);
            return voiceMap;
        }
        
        std::string line;
        int lineNumber = 0;
        bool isFirstLine = true;
        
        while (std::getline(file, line)) {
            lineNumber++;
            
            // Skip empty lines
            if (line.empty() || line.find_first_not_of(" \t\r\n") == std::string::npos) {
                continue;
            }
            
            // Skip header line (assumed to be first non-empty line)
            if (isFirstLine) {
                isFirstLine = false;
                continue;
            }
            
            // Parse CSV line: voicetype,voicefile,transcript
            std::vector<std::string> columns;
            std::string currentColumn;
            bool inQuotes = false;
            
            for (size_t i = 0; i < line.length(); ++i) {
                char c = line[i];
                
                if (c == '"') {
                    inQuotes = !inQuotes;
                } else if (c == ',' && !inQuotes) {
                    columns.push_back(currentColumn);
                    currentColumn.clear();
                } else {
                    currentColumn += c;
                }
            }
            columns.push_back(currentColumn); // Add the last column
            
            if (columns.size() >= 2) {
                std::string voiceType = columns[0];
                std::string voiceFile = columns[1];
                
                // Trim whitespace from voiceType and voiceFile
                voiceType.erase(0, voiceType.find_first_not_of(" \t"));
                voiceType.erase(voiceType.find_last_not_of(" \t") + 1);
                voiceFile.erase(0, voiceFile.find_first_not_of(" \t"));
                voiceFile.erase(voiceFile.find_last_not_of(" \t") + 1);
                
                if (!voiceType.empty() && !voiceFile.empty()) {
                    // Convert voiceType to lowercase for case-insensitive matching
                    std::string lowerVoiceType = voiceType;
                    std::transform(lowerVoiceType.begin(), lowerVoiceType.end(), lowerVoiceType.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    
                    voiceMap[lowerVoiceType] = voiceFile;
                } else {
                    logger::warn("[VOICE_CSV] Skipping line {} - empty voiceType or voiceFile: '{}'", 
                               lineNumber, line);
                }
            } else {
                logger::warn("[VOICE_CSV] Skipping malformed line {} (expected at least 2 columns, got {}): '{}'", 
                           lineNumber, columns.size(), line);
            }
        }
        
        logger::info("[VOICE_CSV] Successfully parsed {} voice mappings from file: {}", 
                   voiceMap.size(), filePath);
        
    } catch (const std::exception& e) {
        logger::error("[VOICE_CSV] Error parsing voice CSV file {}: {}", filePath, e.what());
    }
    
    return voiceMap;
}

void LoadVoiceCSVData() {
    std::lock_guard<std::mutex> lock(csvVoiceCacheMutex);
    
    if (csvVoiceCacheLoaded) {
        return;
    }
    
    logger::info("[VOICE_CSV] Starting to load voice CSV data into cache");
    
    csvVoiceCache.clear();
    
    std::string chimPath = "Data/CHIM";
    std::vector<std::string> voiceCSVFiles = FindVoiceCSVFiles(chimPath);
    
    if (voiceCSVFiles.empty()) {
        logger::info("[VOICE_CSV] No voice CSV files found in CHIM directory");
        csvVoiceCacheLoaded = true;
        return;
    }
    
    int totalMappings = 0;
    for (const auto& filePath : voiceCSVFiles) {
        logger::info("[VOICE_CSV] Processing voice CSV file: {}", filePath);
        
        auto fileVoiceMap = ParseVoiceCSV(filePath);
        
        for (const auto& mapping : fileVoiceMap) {
            // Check for duplicates and warn if found
            if (csvVoiceCache.find(mapping.first) != csvVoiceCache.end()) {
                logger::warn("[VOICE_CSV] Duplicate voice type '{}' found! Previous: '{}', New: '{}' - Using new mapping", 
                           mapping.first, csvVoiceCache[mapping.first], mapping.second);
            }
            
            csvVoiceCache[mapping.first] = mapping.second;
            totalMappings++;
        }
    }
    
    csvVoiceCacheLoaded = true;
    logger::info("[VOICE_CSV] Voice CSV cache loaded successfully! Total mappings: {} from {} files", 
               totalMappings, voiceCSVFiles.size());
}

std::string FindVoiceInCSV(const std::string& voiceType) {
    // Ensure CSV data is loaded
    if (!csvVoiceCacheLoaded) {
        LoadVoiceCSVData();
    }
    
    std::lock_guard<std::mutex> lock(csvVoiceCacheMutex);
    
    if (csvVoiceCache.empty()) {
        return "";
    }
    
    // Convert to lowercase for case-insensitive search
    std::string lowerVoiceType = voiceType;
    std::transform(lowerVoiceType.begin(), lowerVoiceType.end(), lowerVoiceType.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    
    auto it = csvVoiceCache.find(lowerVoiceType);
    if (it != csvVoiceCache.end()) {
        logger::info("[VOICE_CSV] Found voice mapping for '{}' -> '{}'", voiceType, it->second);
        return it->second;
    }
    
    return "";
}

// Utility function to manually trigger CSV voice data reload (useful for testing/debugging)
void ReloadVoiceCSVData() {
    std::lock_guard<std::mutex> lock(csvVoiceCacheMutex);
    
    logger::info("[VOICE_CSV] Manual reload of voice CSV data requested");
    
    csvVoiceCacheLoaded = false;
    csvVoiceCache.clear();
}

float GetPitchFromQuaternionDebug(const RE::NiQuaternion& q) {
    // Debug: Log all quaternion components
    logger::info("Quaternion: w={}, x={}, y={}, z={}", q.w, q.x, q.y, q.z);

    // Calculate all three Euler angles from your reference
    // Roll (x-axis rotation)
    double sinr_cosp = 2 * (q.w * q.x + q.y * q.z);
    double cosr_cosp = 1 - 2 * (q.x * q.x + q.y * q.y);
    float roll = std::atan2(sinr_cosp, cosr_cosp) * -1;  // negated like in reference

    // Pitch (y-axis rotation)
    double sinp = 2 * (q.w * q.y - q.z * q.x);
    float pitch;
    if (std::abs(sinp) >= 1)
        pitch = std::copysign(3.141592654f / 2, sinp);
    else
        pitch = std::asin(sinp);
    // pitch is NOT negated in reference

    // Yaw (z-axis rotation)
    double siny_cosp = 2 * (q.w * q.z + q.x * q.y);
    double cosy_cosp = 1 - 2 * (q.y * q.y + q.z * q.z);
    float yaw = std::atan2(siny_cosp, cosy_cosp) * -1;  // negated like in reference

    // Convert to degrees for logging
    float rollDeg = roll * (180.0f / 3.141592654f);
    float pitchDeg = pitch * (180.0f / 3.141592654f);
    float yawDeg = yaw * (180.0f / 3.141592654f);

    logger::info("Euler angles - Roll: {}�, Pitch: {}�, Yaw: {}�", rollDeg, pitchDeg, yawDeg);

    return pitch;  // Return pitch in radians
}

float GetPitchFromQuaternion(const RE::NiQuaternion& q) {
    // In Skyrim's coordinate system, pitch (looking up/down) is the Roll (x-axis rotation)
    double sinr_cosp = 2 * (q.w * q.x + q.y * q.z);
    double cosr_cosp = 1 - 2 * (q.x * q.x + q.y * q.y);
    float pitch = std::atan2(sinr_cosp, cosr_cosp) * -1;  // negated like in reference

    return pitch;  // Returns radians
}
