#include "Commands.h"

#include "Globals.h"
#include "HTTPManager.h"
#include "HTTPUploader.h"
#include "Misc.h"
#include "Papyrus.h"
#include "PrismaUIBridge.h"
#include "Replacements.h"
#include "SPGResponse.h"
#include "SpeakManager.h"
#include "MusicManager.h"
#include "SpatialAwareness.h"
#include "json.hpp"
#include "RE/Skyrim.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <mutex>


using json = nlohmann::json;

extern std::list<RE::TESObjectREFR*> cachedObjects;
extern bool GlobalAnimations;
extern int GlobalRechatPolicyAsap;
extern std::chrono::high_resolution_clock::time_point controlLastBoredTriggerTS;
extern RE::TESFaction* AIAgentRoleMasterFaction;

extern int sgmode;

int MoveToPlayerRetries = 0;

using json = nlohmann::json;

extern void ScriptProxyRun(const std::string& jsonStr);


std::string toLower(const std::string& str) {
    std::string lowerStr = str;
    std::transform(lowerStr.begin(), lowerStr.end(), lowerStr.begin(), [](unsigned char c) { return std::tolower(c); });
    return lowerStr;
}

bool containsCaseInsensitive(const std::string& haystack, const std::string& needle) {
    std::string lowerHaystack = toLower(haystack);
    std::string lowerNeedle = toLower(needle);
    return lowerHaystack.find(lowerNeedle) != std::string::npos;
}

bool containsCaseSensitive(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

std::string removeTextAfterLastQuote(const std::string& targetName) {
    // Find the position of the last single quote
    size_t pos = targetName.find_last_of('\'');

    // If a single quote is found, return the substring before it
    if (pos != std::string::npos) {
        return targetName.substr(0, pos);
    }

    // If no single quote is found, return the original string
    return targetName;
}

std::string removeTextInParenthesesAndTrim(const std::string& input) {
    // Find the position of the first opening parenthesis
    size_t pos = input.find('(');

    // Take the substring before the parenthesis if found
    std::string result = (pos != std::string::npos) ? input.substr(0, pos) : input;

    // Trim leading whitespace
    auto start = result.find_first_not_of(" \t\n\r\f\v");
    if (start == std::string::npos) return "";  // all spaces

    // Trim trailing whitespace
    auto end = result.find_last_not_of(" \t\n\r\f\v");

    return result.substr(start, end - start + 1);
}

std::string jusTrim(const std::string& input) {
    std::string result = input;
    // Remove all newline characters
    result.erase(std::remove(result.begin(), result.end(), '\n'), result.end());
    result.erase(std::remove(result.begin(), result.end(), '\r'), result.end());
    // Trim leading whitespace
    auto start = result.find_first_not_of(" \t\n\r\f\v");
    if (start == std::string::npos) return "";  // all spaces
    // Trim trailing whitespace
    auto end = result.find_last_not_of(" \t\n\r\f\v");
    return result.substr(start, end - start + 1);
}
std::vector<std::string> splitString(const std::string& input) {
    std::stringstream ss(input);
    std::string segment;
    std::vector<std::string> result;

    // Split the string by '@' and store the segments in a vector
    while (std::getline(ss, segment, '@')) {
        result.push_back(segment);
    }

    return result;  // Return the vector of strings
}

std::string normalizeVoiceEditorId(const char* editorId) {
    if (!editorId) {
        return "";
    }

    std::string value = jusTrim(editorId);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
    return value;
}

std::shared_ptr<AIAgent> findAgentForVoiceRefresh(const std::string& refIdText, const std::string& npcName) {
    AIAgentManager& aiam = AIAgentManager::getInstance();

    if (!refIdText.empty()) {
        try {
            auto formId = static_cast<RE::FormID>(std::stoul(refIdText, nullptr, 0));
            auto agentPtr = aiam.getAgentByFormId(formId);
            if (agentPtr) {
                return agentPtr;
            }
        } catch (const std::exception&) {
            logger::warn("[RefreshNPCVoice] Invalid refid '{}'", refIdText);
        }
    }

    if (!npcName.empty()) {
        return aiam.getAgentByName(npcName);
    }

    return nullptr;
}

void refreshNpcVoiceRecovery(const std::shared_ptr<AIAgent>& agentPtr, const std::string& refIdText,
                             const std::string& npcName) {
    RE::Actor* actor = nullptr;
    if (agentPtr) {
        actor = agentPtr->getActor();
    }

    if (!actor && !refIdText.empty()) {
        try {
            auto formId = static_cast<RE::FormID>(std::stoul(refIdText, nullptr, 0));
            auto form = RE::TESForm::LookupByID(formId);
            if (form) {
                actor = form->As<RE::Actor>();
            }
        } catch (const std::exception&) {
            logger::warn("[RefreshNPCVoice] Failed to resolve actor from refid '{}'", refIdText);
        }
    }

    if (!actor && !npcName.empty()) {
        AIAgentManager& aiam = AIAgentManager::getInstance();
        auto fallbackAgent = aiam.getAgentByName(npcName);
        if (fallbackAgent) {
            actor = fallbackAgent->getActor();
        }
    }

    if (!actor) {
        logger::warn("[RefreshNPCVoice] Actor not found for npc='{}' ref='{}'", npcName, refIdText);
        return;
    }

    std::string actorName = actor->GetDisplayFullName() ? actor->GetDisplayFullName() : npcName;
    std::string voiceId;
    auto baseActor = actor->GetActorBase();
    if (baseActor && baseActor->voiceType) {
        voiceId = normalizeVoiceEditorId(baseActor->voiceType->GetFormEditorID());
    }

    if (!voiceId.empty()) {
        logger::info("[RefreshNPCVoice] Resolved voiceid '{}' for {}", voiceId, actorName);
        HTTPManager::log(std::format("npcvoice_refresh|{}|{}|{}@{:08X}@{}", getCurrentTimeMillis(),
                                     GetGameTimeStamp(), actorName, actor->GetFormID(), voiceId));
    } else {
        logger::warn("[RefreshNPCVoice] No actor base voice type found for {}", actorName);
    }

    auto audiofile = AudioFilesBufferManager::findAudioFile(actor);
    RE::BSResourceNiBinaryStream finaudioFileDetected(audiofile);
    if (finaudioFileDetected.good()) {
        auto size = finaudioFileDetected.stream->totalSize;
        if (size > 0) {
            auto buffer = std::make_unique<char[]>(size);
            finaudioFileDetected.read(buffer.get(), size);
            std::string finalData(buffer.get(), size);

            if (!finalData.empty()) {
                logger::info("[RefreshNPCVoice] Uploading recovered voice sample for {} from {}", actorName, audiofile);
                HTTPUploader& uploader = HTTPUploader::getInstance();
                uploader.UploadVoiceSample(finalData, actorName, audiofile);

                if (agentPtr) {
                    agentPtr->setNeedsVoiceSample(false);
                    agentPtr->setVoiceSamplePath(audiofile);
                }
                return;
            }
        }
    }

    if (agentPtr) {
        logger::warn("[RefreshNPCVoice] No immediate sample for {} - deferring to next dialogue capture", actorName);
        agentPtr->setNeedsVoiceSample(true);
    }
}

int getCarriageFare(const std::string& destinationName) {
    static const std::vector<std::string> majorDestinations = {
        "Whiterun",
        "Solitude",
        "Markarth",
        "Riften",
        "Windhelm",
    };

    const auto normalizedDestination = toLower(trim(destinationName));
    for (const auto& majorDestination : majorDestinations) {
        if (toLower(majorDestination) == normalizedDestination) {
            return 20;
        }
    }

    return 50;
}

int getFerryFare(const std::string& destinationName) {
    const auto normalizedDestination = toLower(trim(destinationName));
    if (normalizedDestination == toLower("Icewater Jetty")) {
        return 500;
    }
    if (normalizedDestination == toLower("Castle Volkihar")) {
        return 0;
    }

    return 50;
}

json parseActionParameterPayload(const std::string& parameter) {
    const std::string trimmedParameter = trim(parameter);
    if (trimmedParameter.empty()) {
        return json();
    }

    try {
        return json::parse(trimmedParameter);
    } catch (...) {
    }

    try {
        const auto jsonStart = trimmedParameter.find('{');
        const auto jsonEnd = trimmedParameter.rfind('}');
        if (jsonStart != std::string::npos && jsonEnd != std::string::npos && jsonEnd > jsonStart) {
            return json::parse(trimmedParameter.substr(jsonStart, jsonEnd - jsonStart + 1));
        }
    } catch (...) {
    }

    return json();
}

std::string extractStructuredActionStringField(const json& payload, const std::initializer_list<const char*>& keys) {
    if (!payload.is_object()) {
        return "";
    }

    for (const auto* key : keys) {
        if (!payload.contains(key)) {
            continue;
        }

        try {
            if (payload[key].is_string()) {
                const auto value = trim(payload[key].get<std::string>());
                if (!value.empty()) {
                    return value;
                }
            }
        } catch (...) {
        }
    }

    return "";
}

bool equalsCaseInsensitive(const std::string& left, const std::string& right) {
    return toLower(trim(left)) == toLower(trim(right));
}

int parseStructuredActionAmount(const std::string& parameter, int defaultCost, bool allowZero = false) {
    const std::string trimmedParameter = trim(parameter);

    if (trimmedParameter.empty()) {
        return defaultCost;
    }

    int amount = defaultCost;
    bool parsedAmount = false;
    const auto payload = parseActionParameterPayload(trimmedParameter);

    if (payload.is_object() && payload.contains("amount")) {
        try {
            if (payload["amount"].is_number_integer()) {
                amount = payload["amount"].get<int>();
                parsedAmount = true;
            } else if (payload["amount"].is_string()) {
                amount = std::stoi(payload["amount"].get<std::string>());
                parsedAmount = true;
            }
        } catch (...) {
        }
    }

    if (!parsedAmount) {
        try {
            amount = std::stoi(trimmedParameter);
            parsedAmount = true;
        } catch (...) {
            amount = defaultCost;
        }
    }

    if (!parsedAmount) {
        return defaultCost;
    }

    if (allowZero) {
        return (amount >= 0) ? amount : defaultCost;
    }

    if (amount <= 0) {
        return defaultCost;
    }

    return amount;
}

std::string parseStructuredActionTarget(const std::string& parameter) {
    const auto payload = parseActionParameterPayload(parameter);
    if (payload.is_object() && payload.contains("target")) {
        try {
            if (payload["target"].is_string()) {
                return trim(payload["target"].get<std::string>());
            }
        } catch (...) {
        }
    }

    return trim(parameter);
}

int parseRentRoomCost(const std::string& parameter) {
    return parseStructuredActionAmount(parameter, 10, false);
}

RE::TESFaction* getCrimeFaction(RE::Actor* npc) {
    if (!npc) return nullptr;
    auto crimeFaction = npc->GetCrimeFaction();
    if (crimeFaction) {
        return crimeFaction;
    }

    auto actorBase = npc->GetActorBase();
    if (actorBase) {
        return actorBase->crimeFaction;
    }

    return nullptr;
}

struct CrimeInfo {
    int amount;
    bool violent;
};

CrimeInfo getCrimeAmount(const std::string& crimeType, const std::string& customAmount) {
    auto normalized = toLower(trim(crimeType));
    if (normalized == "assault") return {40, true};
    if (normalized == "murder") return {1000, true};
    if (normalized == "theft") return {100, false};
    if (normalized == "pickpocketing") return {25, false};
    if (normalized == "trespassing") return {5, false};
    if (normalized == "jailbreak") return {100, false};
    if (normalized == "custom" && !customAmount.empty()) {
        int amt = atoi(customAmount.c_str());
        if (amt <= 0) amt = 40;
        return {amt, false};
    }
    return {40, false};
}

RE::TESObjectREFR* resolveLocationMarker(RE::TESForm* locationForm) {
    if (!locationForm) {
        return nullptr;
    }

    if (auto locationRef = locationForm->AsReference()) {
        return locationRef;
    }

    auto location = locationForm->As<RE::BGSLocation>();
    if (!location) {
        return nullptr;
    }

    for (int i = 0; i < location->specialRefs.size(); i++) {
        auto specialRef = location->specialRefs[i];
        auto refForm = RE::TESForm::LookupByID(specialRef.refData.refID);
        if (!refForm || refForm->GetFormType() != RE::FormType::Reference) {
            continue;
        }

        auto ref = refForm->As<RE::TESObjectREFR>();
        if (!ref || !ref->GetBaseObject()) {
            continue;
        }

        auto baseObjectFormId = ref->GetBaseObject()->GetFormID();
        if (baseObjectFormId == 0x3b || baseObjectFormId == 0x10) {
            logger::info("[HireCarriage] Marker found {} 0x{:08x}", ref->GetFormEditorID(), ref->GetFormID());
            return ref;
        }
    }

    if (location->worldLocMarker) {
        auto markerPtr = location->worldLocMarker.get();
        if (markerPtr) {
            auto marker = markerPtr.get();
            if (marker) {
                logger::info("[HireCarriage] World location marker found {} 0x{:08x}", marker->GetFormEditorID(),
                             marker->GetFormID());
                return marker;
            }
        }
    }

    return nullptr;
}

bool isPlayerTeleportTargetName(const std::string& rawTargetName) {
    auto normalizedTarget = toLower(jusTrim(rawTargetName));
    if (normalizedTarget.empty()) {
        return true;
    }

    if (normalizedTarget == "player" || normalizedTarget == "me") {
        return true;
    }

    auto player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        return false;
    }

    if (containsCaseInsensitive(player->GetDisplayFullName(), rawTargetName) ||
        containsCaseInsensitive(rawTargetName, player->GetDisplayFullName())) {
        return true;
    }

    if (player->GetName() && (containsCaseInsensitive(player->GetName(), rawTargetName) ||
                              containsCaseInsensitive(rawTargetName, player->GetName()))) {
        return true;
    }

    AIAgentManager& aiam = AIAgentManager::getInstance();
    auto configuredPlayerName = jusTrim(aiam.getPlayerName());
    if (!configuredPlayerName.empty() &&
        (containsCaseInsensitive(configuredPlayerName, rawTargetName) ||
         containsCaseInsensitive(rawTargetName, configuredPlayerName))) {
        return true;
    }

    return false;
}

bool isNarratorRoleTargetName(const std::string& rawTargetName) {
    auto normalizedTarget = toLower(jusTrim(rawTargetName));
    return normalizedTarget == "the narrator" || normalizedTarget == "narrator";
}

std::string getPreferredActorDisplayName(RE::Actor* actor, const std::string& fallbackName = "") {
    if (!actor) {
        return fallbackName;
    }

    auto player = RE::PlayerCharacter::GetSingleton();
    if (player && actor->GetFormID() == player->GetFormID()) {
        AIAgentManager& aiam = AIAgentManager::getInstance();
        auto configuredPlayerName = jusTrim(aiam.getPlayerName());
        if (!configuredPlayerName.empty()) {
            return configuredPlayerName;
        }

        if (player->GetName() && std::strlen(player->GetName()) > 0) {
            return std::string(player->GetName());
        }
    }

    std::string resolvedName = actor->GetDisplayFullName();
    if (resolvedName.empty() && actor->GetName() && std::strlen(actor->GetName()) > 0) {
        resolvedName = actor->GetName();
    }

    if (resolvedName.empty()) {
        resolvedName = fallbackName;
    }

    return resolvedName;
}

RE::Actor* resolveTeleportTargetActor(const std::string& rawTargetName) {
    if (isPlayerTeleportTargetName(rawTargetName)) {
        return RE::PlayerCharacter::GetSingleton()->As<RE::Actor>();
    }

    auto targetName = jusTrim(rawTargetName);
    if (targetName.empty()) {
        return RE::PlayerCharacter::GetSingleton()->As<RE::Actor>();
    }

    AIAgentManager& aiam = AIAgentManager::getInstance();
    auto agentPtr = aiam.getAgentByName(targetName);
    if (agentPtr && agentPtr->getActor()) {
        return agentPtr->getActor();
    }

    auto npcListTarget = NPCList::GetInstance().GetNPC(targetName);
    if (npcListTarget) {
        if (auto actor = npcListTarget->As<RE::Actor>()) {
            return actor;
        }
    }

    auto player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        return nullptr;
    }

    auto playerCell = player->GetParentCell();
    if (!playerCell) {
        return nullptr;
    }

    auto targetRef = findActorInCell(targetName, playerCell, player->As<RE::Actor>(), 4096.0f, false);
    return targetRef ? targetRef->As<RE::Actor>() : nullptr;
}

RE::Actor* resolveNarratorRoleTargetActor(const std::string& rawTargetName) {
    if (isNarratorRoleTargetName(rawTargetName)) {
        return RE::PlayerCharacter::GetSingleton()->As<RE::Actor>();
    }

    return resolveTeleportTargetActor(rawTargetName);
}

bool isSafeSpawnableInventoryForm(RE::TESForm* form)
{
    if (!form) {
        return false;
    }

    if (!form->As<RE::TESBoundObject>()) {
        return false;
    }

    if (form->As<RE::TESObjectWEAP>()) {
        return true;
    }
    if (form->As<RE::TESAmmo>()) {
        return true;
    }
    if (form->As<RE::TESObjectARMO>()) {
        return true;
    }
    if (form->As<RE::TESObjectBOOK>()) {
        return true;
    }
    if (form->As<RE::TESObjectMISC>()) {
        return true;
    }
    if (form->As<RE::TESObjectLIGH>()) {
        return true;
    }
    if (form->As<RE::IngredientItem>()) {
        return true;
    }
    if (form->As<RE::AlchemyItem>()) {
        return true;
    }
    if (form->As<RE::TESSoulGem>()) {
        return true;
    }
    if (form->As<RE::TESKey>()) {
        return true;
    }

    return false;
}

RE::TESForm* resolveSpawnableNpcTemplateForm(RE::TESForm* form)
{
    if (!form) {
        return nullptr;
    }

    if (form->As<RE::TESNPC>() || form->As<RE::TESLevCharacter>()) {
        return form;
    }

    if (auto reference = form->As<RE::TESObjectREFR>()) {
        auto baseObject = reference->GetBaseObject();
        if (baseObject && (baseObject->As<RE::TESNPC>() || baseObject->As<RE::TESLevCharacter>())) {
            return baseObject;
        }
    }

    return nullptr;
}

RE::TESForm* findLocation(std::string parameter) {
    auto world = RE::PlayerCharacter::GetSingleton()->GetWorldspace();

    parameter = jusTrim(parameter);
    if (!world) {
        auto location = RE::PlayerCharacter::GetSingleton()->GetCurrentLocation();
        if (location) {
            auto marker = location->worldLocMarker;
            if (marker) world = marker.get()->GetWorldspace();
            if (!world) {
                if (location->parentLoc) {
                    auto parentLocMarker = location->parentLoc->worldLocMarker;

                    if (parentLocMarker) {
                        world = parentLocMarker.get()->GetWorldspace();
                    }
                }
            }
        }
    }

    if (!world) {
        logger::info("[FINDLOCATION] No world info when searching for {}", parameter);
        return nullptr;
    }

    RE::TESWorldSpace* candidate(nullptr);

    std::vector<const RE::TESWorldSpace*> worlds;
    while (world) {
        if (world->locationMap.size() > 0) {
            // parent world has locations, OK to proceed
            logger::info("Found location-bearing parent world {}/0x{:08x} for cell 0x{:08x}", world->GetName(),
                         world->GetFormID(), world->GetFormID());
            candidate = world;
        }
        if (!world->parentWorld) {
            logger::info("Reached root of worldspace hierarchy {}/0x{:08x} for cell 0x{:08x}", world->GetName(),
                         world->GetFormID(), world->GetFormID());
            break;
        }
        world = world->parentWorld;
        if (std::find(worlds.cbegin(), worlds.cend(), world) != worlds.cend()) {
            // cycle in worldspace graph, return best so far
            logger::info("Cycle in worldspace graph at {}/0x{:08x} for cell 0x{:08x}", world->GetName(),
                         world->GetFormID(), world->GetFormID());
            break;
        }
        logger::info("findLocation Nothing found at {}/0x{:08x} for cell 0x{:08x}", world->GetName(),
                     world->GetFormID(), world->GetFormID());
        worlds.push_back(world);
    }

    RE::TESForm* locationForm = nullptr;

    if (candidate) {
        for (auto& pair : candidate->locationMap) {
            RE::FormID key = pair.first;
            RE::BGSLocation* value = pair.second;

            auto fullname = value->GetFullName();

            std::string haystack(value->GetName());
            std::string needle = trim(parameter);
            // logger::info("Marker: name:{} full name:{}", haystack, fullname);
            if (containsCaseSensitive(haystack, needle)) {
                auto marker0 = value->worldLocMarker;
                auto marker1 = value->worldLocMarker.get();
                auto marker2 = marker1.get();

                auto overrideData = value->overrideData;

                for (int i = 0; i < value->specialRefs.size(); i++) {
                    auto specialRefs = value->specialRefs[i];
                    auto ref =
                        RE::TESForm::LookupByID(specialRefs.refData.refID);  // Pick nowhere place here // Serpent Stone

                    if (ref) {
                        // logger::info(" 0x{:08x} ", specialRefs.refData.refID);
                        if (ref->GetFormType() == RE::FormType::Reference) {
                            auto refFinal = ref->As<RE::TESObjectREFR>();
                            if (refFinal->GetBaseObject()->GetFormID() == 0x3b) {
                                logger::info("Early reference found {} 0x{:08x}", ref->GetFormEditorID(),
                                             ref->GetFormID());
                                locationForm = refFinal;
                                break;
                            }
                        }
                    }
                }

                if (locationForm != nullptr) break;

                /* for (const auto& ref : refes) {
                    // Process each RE::ObjectRefHandle (ref)
                    if (ref.get())
                        if (ref.get().get())
                            auto id = ref.get().get()->GetFormID();
                }*/

                if (marker2) {
                    locationForm = marker2;
                    logger::info("FormId 0x{:08x} location {} formId 0x{:08x}", key, value->GetName(),
                                 value->GetFormID());
                    logger::info("Marker Data {}", marker2->GetName());
                    break;
                }
            }
        }
    }

    if (!locationForm) {
        auto player = RE::PlayerCharacter::GetSingleton();
        // Lets search for any building around
        for (const auto& entry : LocationList::GetInstance()) {
            const std::string& name = entry.first;
            RE::TESObjectREFR* location = entry.second;
            if (location->GetPosition().GetDistance(player->GetPosition()) < 10000) {
                
                std::string normalizedname = entry.first;
                // Remove all newline characters
                normalizedname.erase(std::remove(normalizedname.begin(), normalizedname.end(), '\n'),
                                     normalizedname.end());
                normalizedname.erase(std::remove(normalizedname.begin(), normalizedname.end(), '\r'),
                                     normalizedname.end());
                // Trim leading and trailing whitespace (including any remaining control characters)
                auto start = normalizedname.find_first_not_of(" \t\n\r\f\v");
                if (start == std::string::npos) {
                    normalizedname = "";
                } else {
                    auto end = normalizedname.find_last_not_of(" \t\n\r\f\v");
                    normalizedname = normalizedname.substr(start, end - start + 1);
                }

                logger::info("[findLocation] Sanitized from:<{}> to:<{}>", entry.first, normalizedname);


                if (containsCaseInsensitive(normalizedname, parameter)) {
                    locationForm = RE::TESForm::LookupByID(location->GetFormID());
                    logger::info("[findLocation] Nearby LocationList Found: {} 0x{:08x}", name, locationForm->GetFormID());
                    break;
                }
                // Also check for doors and passages
                std::string passage = parameter;
                passage.append(" (door/passage)");
                if (containsCaseInsensitive(normalizedname, passage)) {
                    locationForm = RE::TESForm::LookupByID(location->GetFormID());
                    logger::info("[findLocation] Nearby LocationList Found: {} 0x{:08x}", name, locationForm->GetFormID());
                    break;
                }
            }
        }
    }
    if (!locationForm) {
        logger::info("[findLocation] No location found for <{}>", parameter);
        return nullptr;
    }

    logger::info("[findLocation] Returning  0x{:08x}", locationForm->GetFormID());

    return locationForm;
}

void parseRoleCommand(std::string rawCommand) {
    static std::string delimiter = "@";
    size_t pos = rawCommand.find(delimiter);
    if (pos == std::string::npos) {
        responsePop("rolecommand");
        return;
    }

    std::string command = rawCommand.substr(0, pos);
    std::string parameter = rawCommand.substr(pos + delimiter.length());

    if (command.contains("spawnCharacter")) {
        std::vector<std::string> splitResult = splitString(parameter);

        if (splitResult.size() != 7) {
            logger::info("Command has not enough parms {}", command);
        } else {
            int fidNPC = atoi(splitResult[1].c_str());
            int fidClothes = atoi(splitResult[2].c_str());
            int fidWeapon = atoi(splitResult[3].c_str());
            int fidPlace = atoi(splitResult[4].c_str());
            int fidSource = atoi(splitResult[6].c_str());

            controlLastBoredTriggerTS = std::chrono::high_resolution_clock::now() + std::chrono::seconds(15);
            ;

            auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
            auto args = RE::MakeFunctionArguments(std::move(splitResult[0]), std::move(fidNPC), std::move(fidClothes),
                                                  std::move(fidWeapon), std::move(fidPlace), std::move(splitResult[5]),
                                                  std::move(fidSource));
            RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentAIMind", "SpawnAgent",
                                                                                       args, callback);
        }

    } else if (command.contains("spawnItem")) {
        std::vector<std::string> splitResult = splitString(parameter);

        if (splitResult.size() != 4) {
            logger::info("Command has not enough parms {}", command);
        } else {
            int fidType = atoi(splitResult[1].c_str());
            int fidLoc = atoi(splitResult[2].c_str());
            auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
            auto args = RE::MakeFunctionArguments(std::move(splitResult[0]), std::move(fidType), std::move(fidLoc),
                                                  std::move(splitResult[3]));
            RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentAIMind", "SpawnItem",
                                                                                       args, callback);
        }

    } else if (command.contains("spawnBook")) {
        std::vector<std::string> splitResult = splitString(parameter);

        if (splitResult.size() != 5) {
            logger::info("Command has not enough parms {}", command);
        } else {
            int fidType = atoi(splitResult[1].c_str());
            int fidLoc = atoi(splitResult[2].c_str());

            auto oNoteId = RE::TESDataHandler::GetSingleton()->LookupFormID((RE::FormID)0x021d0b, "AIAgent.esp");
            auto oNote = RE::TESForm::LookupByID(oNoteId);

            SpeakManager::getInstance().downloadFakeNote(splitResult[0]);

            auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
            auto args = RE::MakeFunctionArguments(std::move(splitResult[0]), std::move(fidType), std::move(fidLoc),
                                                  std::move(splitResult[3]), std::move(splitResult[4]));
            RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentAIMind", "SpawnBook",
                                                                                       args, callback);
        }

    } else if (command.contains("generateLetter")) {
        std::vector<std::string> splitResult = splitString(parameter);

        if (splitResult.size() != 1) {
            logger::info("Command has not enough parms {}", command);
        } else {
            
            SpeakManager::getInstance().downloadFakeNote(splitResult[0]);

        }

    } else if (command.contains("moveToPlayer")) {
        std::vector<std::string> splitResult = splitString(parameter);

        if (splitResult.size() != 3) {
            logger::info("Command has not enough parms {}", command);
        } else {
            AIAgentManager& aiam = AIAgentManager::getInstance();
            auto agentPtr = aiam.getAgentByName(splitResult[0]);
            if (agentPtr) {
                if (agentPtr->getActor()) {
                    int intent = atoi(splitResult[2].c_str());

                    auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                    auto args = RE::MakeFunctionArguments(std::move(agentPtr->getActor()), std::move(splitResult[1]),
                                                          std::move(intent));
                    RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                        "AIAgentAIMind", "MoveToPlayer", args, callback);

                    RefreshAIAgentInventory(agentPtr->getActor(), agentPtr->getActorName(),true);
                }
                MoveToPlayerRetries = 0;
            } else {
                if (MoveToPlayerRetries < 10) {
                    MoveToPlayerRetries++;
                    return;
                } else {
                    MoveToPlayerRetries = 0;
                }
            }
        }

    } else if (command.contains("stayAtPlace")) {
        std::vector<std::string> splitResult = splitString(parameter);

        if (splitResult.size() != 3) {
            logger::info("Command has not enough parms {}", command);
        } else {
            AIAgentManager& aiam = AIAgentManager::getInstance();
            auto agentPtr = aiam.getAgentByName(splitResult[0]);
            if (agentPtr)
                if (agentPtr->getActor()) {
                    int followPlayer = atoi(splitResult[1].c_str());
                    auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                    auto args = RE::MakeFunctionArguments(std::move(agentPtr->getActor()), std::move(followPlayer),
                                                          std::move(splitResult[2]));
                    RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                        "AIAgentAIMind", "stayAtPlace", args, callback);

                    auto npc = agentPtr->getActor();
                }
        }
    } else if (command.contains("TravelTo")) {
        std::vector<std::string> splitResult = splitString(parameter);

        if (splitResult.size() != 3) {
            logger::info("Command has not enough parms {}", command);
        } else {
            AIAgentManager& aiam = AIAgentManager::getInstance();
            auto agentPtr = aiam.getAgentByName(splitResult[0]);
            if (agentPtr)
                if (agentPtr->getActor()) {
                    auto targetActor = agentPtr->getActor();

                    auto markerForm = RE::TESForm::LookupByID(0x000e0f69);  // Pick nowhere place here // Serpent Stone
                    if (markerForm) {
                        auto marker = markerForm->As<RE::TESObjectREFR>();

                        logger::info("Location target: {}", marker->GetName());

                        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                        auto args = RE::MakeFunctionArguments(std::move(targetActor), std::move(marker),
                                                              std::move(marker->GetName()));
                        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                            "AIAgentAIMind", "TravelToTarget", args, callback);

                        agentPtr.get()->setCurrentCommand("TravelTo");
                        // agentPtr.get()->setCommandBusy(true);
                    }
                }
        }
    } else if (command.contains("TeleportNPCRaw")) {
        std::vector<std::string> splitResult = splitString(parameter);
        if (splitResult.size() < 2) {
            logger::info("Command has not enough parms {}", command);
        } else {
            auto targetName = jusTrim(splitResult[0]);
            auto locationIdText = jusTrim(splitResult[1]);
            auto locationLabel = splitResult.size() >= 3 ? jusTrim(splitResult[2]) : locationIdText;

            RE::Actor* targetActor = resolveNarratorRoleTargetActor(targetName);
            if (!targetActor) {
                auto errorText = std::format("[CHIM] Could not find {}", targetName.empty() ? "the teleport target" : targetName);
                logger::warn("[TeleportNPCRaw] Could not resolve target '{}'", targetName);
                RE::DebugNotification(errorText.c_str());
            } else {
                try {
                    auto locationFormId = static_cast<RE::FormID>(std::stoul(locationIdText, nullptr, 0));
                    auto locationForm = RE::TESForm::LookupByID(locationFormId);
                    auto locationMarker = resolveLocationMarker(locationForm);

                    if (!locationForm || !locationMarker) {
                        auto resolvedTargetName = getPreferredActorDisplayName(targetActor, targetName);
                        auto errorText = std::format("[CHIM] Could not find a teleport marker for {}", locationLabel);
                        logger::warn("[TeleportNPCRaw] No location marker found for '{}' ({})", locationLabel, locationIdText);
                        HTTPManager::log(std::format("infoaction|{}|{}|Could not teleport {} to {}. Destination marker not found.",
                                                     getCurrentTimeMillis(), GetGameTimeStamp(),
                                                     resolvedTargetName, locationLabel));
                        RE::DebugNotification(errorText.c_str());
                    } else {
                        auto resolvedTargetName = getPreferredActorDisplayName(targetActor, targetName);
                        auto papyrusTargetName = resolvedTargetName;
                        auto papyrusLocationLabel = locationLabel;
                        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                        auto args = RE::MakeFunctionArguments(std::move(targetActor), std::move(locationMarker),
                                                              std::move(papyrusLocationLabel), std::move(papyrusTargetName));
                        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                            "AIAgentAIMind", "TeleportActorToLocation", args, callback);

                        HTTPManager::log(std::format("infoaction|{}|{}|{} teleports to {}", getCurrentTimeMillis(),
                                                     GetGameTimeStamp(), resolvedTargetName, locationLabel),
                                         targetActor);
                    }
                } catch (const std::exception&) {
                    auto errorText = std::format("[CHIM] Invalid teleport destination {}", locationIdText);
                    logger::warn("[TeleportNPCRaw] Invalid location formid '{}'", locationIdText);
                    RE::DebugNotification(errorText.c_str());
                }
            }
        }
    } else if (command.contains("TeleportNPC")) {
        std::vector<std::string> splitResult = splitString(parameter);
        if (splitResult.size() < 2) {
            logger::info("Command has not enough parms {}", command);
        } else {
            auto targetName = jusTrim(splitResult[0]);
            auto destinationName = jusTrim(splitResult[1]);

            RE::Actor* targetActor = resolveNarratorRoleTargetActor(targetName);
            if (!targetActor) {
                auto errorText = std::format("[CHIM] Could not find {}", targetName.empty() ? "the teleport target" : targetName);
                logger::warn("[TeleportNPC] Could not resolve target '{}'", targetName);
                RE::DebugNotification(errorText.c_str());
            } else {
                auto locationForm = findLocation(destinationName);
                auto destinationMarker = resolveLocationMarker(locationForm);

                if (!locationForm || !destinationMarker) {
                    auto resolvedTargetName = getPreferredActorDisplayName(targetActor, targetName);
                    auto errorText = std::format("[CHIM] Destination {} is unknown", destinationName);
                    logger::warn("[TeleportNPC] Could not resolve location '{}'", destinationName);
                    HTTPManager::log(std::format("infoaction|{}|{}|Could not teleport {} to {}. Destination not known.",
                                                 getCurrentTimeMillis(), GetGameTimeStamp(),
                                                 resolvedTargetName, destinationName));
                    RE::DebugNotification(errorText.c_str());
                } else {
                    auto resolvedTargetName = getPreferredActorDisplayName(targetActor, targetName);
                    auto papyrusTargetName = resolvedTargetName;
                    auto papyrusDestinationName = destinationName;
                    auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                    auto args = RE::MakeFunctionArguments(std::move(targetActor), std::move(destinationMarker),
                                                          std::move(papyrusDestinationName), std::move(papyrusTargetName));
                    RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                        "AIAgentAIMind", "TeleportActorToLocation", args, callback);

                    HTTPManager::log(std::format("infoaction|{}|{}|{} teleports to {}", getCurrentTimeMillis(),
                                                 GetGameTimeStamp(), resolvedTargetName, destinationName),
                                     targetActor);
                }
            }
        }
    } else if (command.contains("KillTargetRaw")) {
        std::vector<std::string> splitResult = splitString(parameter);
        auto targetName = splitResult.empty() ? std::string() : jusTrim(splitResult[0]);

        if (targetName.empty()) {
            auto errorText = std::string("[CHIM] Kill_Target requires a target");
            logger::warn("[KillTargetRaw] Missing target");
            HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                         "command@KillTarget@@Could not kill the target."),
                             RE::PlayerCharacter::GetSingleton()->As<RE::Actor>());
            RE::DebugNotification(errorText.c_str());
        } else {
            RE::Actor* targetActor = resolveNarratorRoleTargetActor(targetName);
            if (!targetActor) {
                auto errorText = std::format("[CHIM] Could not find {}", targetName);
                auto resultText = std::format("Could not kill {}.", targetName);
                logger::warn("[KillTargetRaw] Could not resolve target '{}'", targetName);
                HTTPManager::log(std::format("infoaction|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                             resultText),
                                 RE::PlayerCharacter::GetSingleton()->As<RE::Actor>());
                HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                             "command@KillTarget@" + targetName + "@" + resultText),
                                 RE::PlayerCharacter::GetSingleton()->As<RE::Actor>());
                RE::DebugNotification(errorText.c_str());
            } else {
                std::string resolvedTargetName = getPreferredActorDisplayName(targetActor, targetName);
                std::string papyrusTargetName = resolvedTargetName;
                std::string narratorName = "The Narrator";
                auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                auto args = RE::MakeFunctionArguments(std::move(targetActor), std::move(papyrusTargetName),
                                                      std::move(narratorName));
                RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                    "AIAgentAIMind", "KillActorTarget", args, callback);
            }
        }
    } else if (command.contains("SpawnNPCRaw")) {
        std::vector<std::string> splitResult = splitString(parameter);
        if (splitResult.size() < 2) {
            logger::info("Command has not enough parms {}", command);
        } else {
            auto templateKey = jusTrim(splitResult[0]);
            auto formIdText = jusTrim(splitResult[1]);

            int spawnAmount = 1;
            if (splitResult.size() >= 3) {
                try {
                    spawnAmount = std::stoi(jusTrim(splitResult[2]));
                } catch (const std::exception&) {
                    spawnAmount = 1;
                }
            }
            if (spawnAmount <= 0) {
                spawnAmount = 1;
            } else if (spawnAmount > 10) {
                spawnAmount = 10;
            }

            try {
                auto templateFormId = static_cast<RE::FormID>(std::stoul(formIdText, nullptr, 16));
                auto requestedForm = RE::TESForm::LookupByID(templateFormId);
                auto spawnableTemplateForm = resolveSpawnableNpcTemplateForm(requestedForm);
                if (!spawnableTemplateForm) {
                    auto errorText = std::format("[CHIM] Could not spawn {}", templateKey.empty() ? "that NPC template" : templateKey);
                    auto resultText = std::format("Could not spawn {}.", templateKey.empty() ? "that NPC template" : templateKey);
                    auto formTypeValue = requestedForm ? static_cast<int>(requestedForm->GetFormType()) : -1;
                    auto formName = requestedForm ? std::string(requestedForm->GetName()) : std::string();
                    logger::warn("[SpawnNPCRaw] Invalid or unsafe NPC template form '{}' for '{}' (type={}, form_name='{}')",
                                 formIdText, templateKey, formTypeValue, formName);
                    HTTPManager::log(std::format("infoaction|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(), resultText),
                                     RE::PlayerCharacter::GetSingleton()->As<RE::Actor>());
                    HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                                 "command@SpawnNPC@" + templateKey + "@" + resultText),
                                     RE::PlayerCharacter::GetSingleton()->As<RE::Actor>());
                    RE::DebugNotification(errorText.c_str());
                } else {
                    std::string templateLabel = templateKey.empty() ? "npc_template" : templateKey;
                    std::string narratorName = "The Narrator";
                    AIAgentManager& aiam = AIAgentManager::getInstance();
                    std::string playerDisplayName = jusTrim(aiam.getPlayerName());
                    if (playerDisplayName.empty()) {
                        auto player = RE::PlayerCharacter::GetSingleton();
                        if (player && player->GetName() && std::strlen(player->GetName()) > 0) {
                            playerDisplayName = std::string(player->GetName());
                        } else {
                            playerDisplayName = "Player";
                        }
                    }
                    auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                    auto args = RE::MakeFunctionArguments(std::move(spawnableTemplateForm), static_cast<int>(spawnAmount),
                                                          std::move(templateLabel), std::move(narratorName),
                                                          std::move(playerDisplayName));
                    RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                        "AIAgentAIMind", "SpawnNpcTemplateNearPlayer", args, callback);
                }
            } catch (const std::exception&) {
                auto errorText = std::format("[CHIM] Invalid NPC template form {}", formIdText);
                auto resultText = std::format("Could not spawn {}.", templateKey.empty() ? "that NPC template" : templateKey);
                logger::warn("[SpawnNPCRaw] Invalid NPC template formid '{}'", formIdText);
                HTTPManager::log(std::format("infoaction|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(), resultText),
                                 RE::PlayerCharacter::GetSingleton()->As<RE::Actor>());
                HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                             "command@SpawnNPC@" + templateKey + "@" + resultText),
                                 RE::PlayerCharacter::GetSingleton()->As<RE::Actor>());
                RE::DebugNotification(errorText.c_str());
            }
        }
    } else if (command.contains("SpawnItemRaw")) {
        std::vector<std::string> splitResult = splitString(parameter);
        if (splitResult.size() < 3) {
            logger::info("Command has not enough parms {}", command);
        } else {
            auto targetName = jusTrim(splitResult[0]);
            auto itemFormIdText = jusTrim(splitResult[1]);
            auto itemName = splitResult.size() >= 4 ? jusTrim(splitResult[3]) : itemFormIdText;

            int itemAmount = 1;
            try {
                itemAmount = std::stoi(jusTrim(splitResult[2]));
            } catch (const std::exception&) {
                itemAmount = 1;
            }
            if (itemAmount <= 0) {
                itemAmount = 1;
            } else if (itemAmount > 1000) {
                itemAmount = 1000;
            }

            RE::Actor* targetActor = resolveNarratorRoleTargetActor(targetName);
            if (!targetActor) {
                auto errorText = std::format("[CHIM] Could not find {}", targetName.empty() ? "the spawn target" : targetName);
                logger::warn("[SpawnItemRaw] Could not resolve target '{}'", targetName);
                RE::DebugNotification(errorText.c_str());
            } else {
                try {
                    auto itemFormId = static_cast<RE::FormID>(std::stoul(itemFormIdText, nullptr, 16));
                    auto itemForm = RE::TESForm::LookupByID(itemFormId);
                    if (!itemForm || !isSafeSpawnableInventoryForm(itemForm)) {
                        auto errorText = std::format("[CHIM] Could not spawn {}", itemName);
                        auto formTypeValue = itemForm ? static_cast<int>(itemForm->GetFormType()) : -1;
                        auto formName = itemForm ? std::string(itemForm->GetName()) : std::string();
                        logger::warn("[SpawnItemRaw] Invalid or unsafe item form '{}' for '{}' (type={}, form_name='{}')",
                                     itemFormIdText, itemName, formTypeValue, formName);
                        HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                                     "command@SpawnItem@" + itemName + "@Error: item cannot be spawned"),
                                         targetActor);
                        RE::DebugNotification(errorText.c_str());
                    } else {
                        std::string resolvedTargetName = getPreferredActorDisplayName(targetActor, targetName);
                        std::string papyrusTargetName = resolvedTargetName;
                        std::string itemNameForPapyrus = itemName;
                        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                        auto args = RE::MakeFunctionArguments(std::move(targetActor), std::move(itemForm),
                                                              static_cast<int>(itemAmount), std::move(itemNameForPapyrus),
                                                              std::move(papyrusTargetName));
                        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                            "AIAgentAIMind", "SpawnAndGiveItemToActor", args, callback);

                        HTTPManager::log(std::format("infoaction|{}|{}|{} receives {} {}", getCurrentTimeMillis(),
                                                     GetGameTimeStamp(), resolvedTargetName, itemAmount, itemName),
                                         targetActor);
                        HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                                     "command@SpawnItem@" + resolvedTargetName + "@" +
                                                         resolvedTargetName + " receives " + std::to_string(itemAmount) +
                                                         " " + itemName + "."),
                                         targetActor);
                    }
                } catch (const std::exception&) {
                    auto errorText = std::format("[CHIM] Invalid spawn item form {}", itemFormIdText);
                    logger::warn("[SpawnItemRaw] Invalid item formid '{}'", itemFormIdText);
                    HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                                 "command@SpawnItem@" + itemName + "@Error: invalid item reference"),
                                     targetActor);
                    RE::DebugNotification(errorText.c_str());
                }
            }
        }
    } else if (command.contains("SpawnGoldRaw")) {
        std::vector<std::string> splitResult = splitString(parameter);
        if (splitResult.size() < 2) {
            logger::info("Command has not enough parms {}", command);
        } else {
            auto targetName = jusTrim(splitResult[0]);

            int goldAmount = 1;
            try {
                goldAmount = std::stoi(jusTrim(splitResult[1]));
            } catch (const std::exception&) {
                goldAmount = 1;
            }
            if (goldAmount <= 0) {
                goldAmount = 1;
            } else if (goldAmount > 1000000) {
                goldAmount = 1000000;
            }

            RE::Actor* targetActor = resolveNarratorRoleTargetActor(targetName);
            if (!targetActor) {
                auto errorText = std::format("[CHIM] Could not find {}", targetName.empty() ? "the gold recipient" : targetName);
                logger::warn("[SpawnGoldRaw] Could not resolve target '{}'", targetName);
                RE::DebugNotification(errorText.c_str());
            } else {
                auto goldForm = RE::TESForm::LookupByID(0x0f);
                if (!goldForm) {
                    auto errorText = std::string("[CHIM] Could not find the gold form");
                    logger::error("[SpawnGoldRaw] Could not find gold form");
                    HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                                 "command@SpawnGold@Gold@Error: invalid gold reference"),
                                     targetActor);
                    RE::DebugNotification(errorText.c_str());
                } else {
                    std::string resolvedTargetName = getPreferredActorDisplayName(targetActor, targetName);
                    std::string papyrusTargetName = resolvedTargetName;
                    std::string goldName = "Gold";
                    auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                    auto args = RE::MakeFunctionArguments(std::move(targetActor), std::move(goldForm),
                                                          static_cast<int>(goldAmount), std::move(goldName),
                                                          std::move(papyrusTargetName));
                    RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                        "AIAgentAIMind", "SpawnAndGiveItemToActor", args, callback);

                    HTTPManager::log(std::format("infoaction|{}|{}|{} receives {} gold", getCurrentTimeMillis(),
                                                 GetGameTimeStamp(), resolvedTargetName, goldAmount),
                                     targetActor);
                    HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                                 "command@SpawnGold@" + resolvedTargetName + "@" +
                                                     resolvedTargetName + " receives " + std::to_string(goldAmount) +
                                                     " gold."),
                                     targetActor);
                }
            }
        }
    } else if (command.contains("CombatPlayer")) {
        std::vector<std::string> splitResult = splitString(parameter);

        if (splitResult.size() != 2) {
            logger::info("Command has not enough parms {}", command);
        } else {
            AIAgentManager& aiam = AIAgentManager::getInstance();
            auto agentPtr = aiam.getAgentByName(splitResult[0]);
            if (agentPtr)
                if (agentPtr->getActor()) {
                    auto targetActor = agentPtr->getActor();

                    auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                    auto args = RE::MakeFunctionArguments(std::move(targetActor));
                    RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                        "AIAgentAIMind", "CombatPlayer", args, callback);
                }
        }
    } else if (command.contains("Instruction")) {
        std::vector<std::string> splitResult = splitString(parameter);

        if (splitResult.size() != 3) {
            logger::info("Command has not enough parms {}, size {}", command, splitResult.size());
        } else {
            AIAgentManager& aiam = AIAgentManager::getInstance();
            auto agentPtr = aiam.getAgentByName(splitResult[0]);
            if (agentPtr)
                if (agentPtr->getActor()) {
                    auto targetActor = agentPtr->getActor();

                    SpeakManager::getInstance().deleteQueue(true);
                    if (SpeakManager::getInstance().getProcessing())
                        SpeakManager::getInstance().abortPlay();  // Stop NPCs talking

                    auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                    auto args = RE::MakeFunctionArguments(std::move(targetActor), std::move(splitResult[1]));
                    RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                        "AIAgentAIMind", "SendInstruction", args, callback);
                }
        }
    } else if (command.contains("Suggestion")) {
        std::vector<std::string> splitResult = splitString(parameter);

        if (splitResult.size() != 3) {
            logger::info("Command has not enough parms {}", command);
        } else {
            AIAgentManager& aiam = AIAgentManager::getInstance();
            auto agentPtr = aiam.getAgentByName(splitResult[0]);
            if (agentPtr)
                if (agentPtr->getActor()) {
                    auto targetActor = agentPtr->getActor();

                    auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                    auto args = RE::MakeFunctionArguments(std::move(targetActor), std::move(splitResult[1]));
                    RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                        "AIAgentAIMind", "SendSuggestion", args, callback);
                }
        }
    } else if (command.contains("Disposition")) {
        std::vector<std::string> splitResult = splitString(parameter);

        if (splitResult.size() != 3) {
            logger::info("Command has not enough parms {}", command);
        } else {
            AIAgentManager& aiam = AIAgentManager::getInstance();
            auto agentPtr = aiam.getAgentByName(splitResult[0]);
            if (agentPtr)
                if (agentPtr->getActor()) {
                    auto targetActor = agentPtr->getActor();

                    auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                    auto args = RE::MakeFunctionArguments(std::move(targetActor), std::move(splitResult[1]));
                    RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                        "AIAgentAIMind", "SetDisposition", args, callback);
                }
        }
    } else if (command.contains("Despawn")) {
        std::vector<std::string> splitResult = splitString(parameter);

        if (splitResult.size() != 2) {
            logger::info("Command has not enough parms {}", command);
        } else {
            AIAgentManager& aiam = AIAgentManager::getInstance();
            auto agentPtr = aiam.getAgentByName(splitResult[0]);
            if (agentPtr)
                if (agentPtr->getActor()) {
                    auto targetActor = agentPtr->getActor();

                    auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                    auto args = RE::MakeFunctionArguments(std::move(targetActor));
                    RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                        "AIAgentAIMind", "Despawn", args, callback);
                }
        }
    } else if (command.contains("EndQuest")) {
        std::vector<std::string> splitResult = splitString(parameter);

        if (splitResult.size() != 2) {
            logger::info("Command has not enough parms {}", command);
        } else {
            // Try to update context info
            auto player = RE::PlayerCharacter::GetSingleton();
            auto result = InspectLocations(player->AsReference());
            char timeDateString[200];
            RE::Calendar::GetSingleton()->GetTimeDateString(timeDateString, 200, true);

            HTTPManager::log(std::format("infoloc|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                         "(Context location: " + std::string(GetPlayerLocation()) +
                                             ", Buildings to go:" + result +
                                             ", Current Date in Skyrim World: " + timeDateString + ")"));

            result = InspectSurroundings(player->AsReference(), true, HERIKA_MAX_VISION_RANGE, ",",
                                         DISTANCE_ACTIVATING_NPC_OUT);
            HTTPManager::log(std::format("infonpc|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                         "(beings in range:" + result + ")"));



            logger::info("EndQuest {} TaskId {}", splitResult[0], splitResult[1]);
            auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
            auto args = RE::MakeFunctionArguments(std::move(splitResult[0]), std::move(splitResult[1]));
            RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                "AIAgentAIMind", "EndQuestNotification", args, callback);
        }
    } else if (command.contains("StartQuest")) {
        std::vector<std::string> splitResult = splitString(parameter);

        if (splitResult.size() != 2) {
            logger::info("Command has not enough parms {}", command);
        } else {
            logger::info("StartQuest {} TaskId {}", splitResult[0], splitResult[1]);
            auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
            auto args = RE::MakeFunctionArguments(std::move(splitResult[0]), std::move(splitResult[1]));
            RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                "AIAgentAIMind", "StartQuestNotification", args, callback);
        }
    } else if (command.contains("UpdateQuest")) {
        std::vector<std::string> splitResult = splitString(parameter);

        if (splitResult.size() != 2) {
            logger::info("Command has not enough parms {}", command);
        } else {
            logger::info("StartQuest {} TaskId {}", splitResult[0], splitResult[1]);
            auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
            auto args = RE::MakeFunctionArguments(std::move(splitResult[0]), std::move(splitResult[1]));
            RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                "AIAgentAIMind", "UpdateQuest", args, callback);
        }
    } else if (command.contains("Sandbox")) {
        std::vector<std::string> splitResult = splitString(parameter);

        if (splitResult.size() != 2) {
            logger::info("Command has not enough parms {}", command);
        } else {
            AIAgentManager& aiam = AIAgentManager::getInstance();
            auto agentPtr = aiam.getAgentByName(splitResult[0]);
            if (agentPtr)
                if (agentPtr->getActor()) {
                    auto targetActor = agentPtr->getActor();

                    auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                    auto args = RE::MakeFunctionArguments(std::move(targetActor), std::move(splitResult[1]));
                    RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                        "AIAgentAIMind", "Sandbox", args, callback);
                }
        }
    } else if (command.contains("ImpersonatePlayer")) {
        std::vector<std::string> splitResult = splitString(parameter);

        if (splitResult.size() != 2) {
            logger::info("Command has not enough parms {}", command);
        } else {
            sendMessageReal(splitResult[0], splitResult[1]);
        }
    } else if (command.contains("QuestNotifySound")) {
        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
        auto args = RE::MakeFunctionArguments();
        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentAIMind", "QuestNotifySound",
                                                                                   args, callback);

    } else if (command.contains("RawDebugNotification") || command.contains("DebugNotification")) {
        std::vector<std::string> splitResult = splitString(parameter);
        if (splitResult.size() != 1) {
            logger::info("Command has not enough parms {}", command);
        } else {
            std::string message(splitResult[0].c_str());
            const bool addPrefix = true;
            if (addPrefix && message.find("[CHIM]") != 0) {
                message = std::format("[CHIM] {}", message);
            }

            auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
            auto args = RE::MakeFunctionArguments(std::move(message));
            bool dispatched = RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                "AIAgentAIMind", "ShowDebugNotification", args, callback);
            if (!dispatched) {
                logger::warn("[DebugNotification] Failed to dispatch Papyrus notification helper, falling back to RE::DebugNotification");
                auto fallbackMessage = splitResult[0];
                if (addPrefix && fallbackMessage.find("[CHIM]") != 0) {
                    fallbackMessage = std::format("[CHIM] {}", fallbackMessage);
                }
                RE::DebugNotification(fallbackMessage.c_str());
            }
        }

    } else if (command.contains("InternalSetting")) {
        std::vector<std::string> splitResult = splitString(parameter);
        if (splitResult.size() != 3) {
            logger::info("Command has not enough parms {}", command);
        } else {
            // parse int from splitResult[1]
            int intval = 0;
            try {
                intval = std::stoi(splitResult[1]);
            } catch (const std::exception& e) {
                logger::warn("Invalid integer parameter: {}", splitResult[1]);
                intval = 0;  // default fallback
            }

            // parse float from splitResult[2]
            float floatval = 0.0f;
            try {
                floatval = std::stof(splitResult[2]);
            } catch (const std::exception& e) {
                logger::warn("Invalid float parameter: {}", splitResult[2]);
                floatval = 0.0f;  // default fallback
            }

            Papyrus::setConfReal(splitResult[0], floatval, intval, splitResult[2]);
        }

    } else if (command.contains("QuestTrackReference")) {
        std::vector<std::string> splitResult = splitString(parameter);
        if (splitResult.size() != 1) {
            logger::info("Command has not enough parms {}", command);
        } else {
            // parse int from splitResult[1]
            std::string backgroundCmdStr(splitResult[0].c_str());

            uint32_t intval = 0;
            try {
                logger::info("[QuestTrackReference] Attempting to parse '{}'", splitResult[0]);
                intval = std::stoul(splitResult[0], nullptr, 0);
                logger::info("[QuestTrackReference] Parsed value: 0x{:08x}", intval);
            } catch (const std::exception&) {
                // Try parsing as hex explicitly if the first attempt fails
                try {
                    intval = std::stoul(splitResult[0], nullptr, 16);
                    logger::info("[QuestTrackReference] Fallback hex parse: 0x{:08x}", intval);
                } catch (const std::exception&) {
                    logger::warn("[QuestTrackReference] Invalid integer parameter: '{}'", splitResult[0]);
                    intval = 0;  // default fallback
                }
            }

            RE::TESForm* form = RE::TESForm::LookupByID(intval);

            if (!form) {
                logger::info("[QuestTrackReference] Reference 0x{:08x} <{}>, is not a valid form", intval, splitResult[0]);
            } else {
                logger::info("[QuestTrackReference] Reference 0x{:08x} is a valid form", form->GetFormID());

                auto reference = form->As<RE::TESObjectREFR>();
                if (reference) {
                    logger::info("[QuestTrackReference] Reference {} is a valid reference", reference->GetDisplayFullName());

                    auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                    auto args = RE::MakeFunctionArguments(std::move(reference));

                    RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                        "AIAgentAIMind", "SetQuestTracker", args, callback);
                } else {
                    logger::info("[QuestTrackReference] Reference 0x{:08x} is not a valid TESObjectREFR", intval);
                }
            }

        }

    } else if (command.contains("RefreshNPCVoice")) {
        std::vector<std::string> splitResult = splitString(parameter);
        std::string refIdText = splitResult.size() > 0 ? jusTrim(splitResult[0]) : "";
        std::string npcName = splitResult.size() > 1 ? jusTrim(splitResult[1]) : "";

        auto agentPtr = findAgentForVoiceRefresh(refIdText, npcName);
        refreshNpcVoiceRecovery(agentPtr, refIdText, npcName);

    } else if (command.contains("RenameNPC")) {
        std::vector<std::string> splitResult = splitString(parameter);
        if (splitResult.size() != 2) {
            logger::info("Command has not enough parms {}", command);
        } else {
            // parse int from splitResult[1]
            uint32_t intval = 0;
            try {
                intval = std::stoul(splitResult[0], nullptr, 0);

            } catch (const std::exception& e) {
                logger::warn("Invalid integer parameter: {}", splitResult[1]);
                intval = 0;  // default fallback
            }

            std::string newName(splitResult[1].c_str());

            RE::TESForm* form = RE::TESForm::LookupByID(intval);
            if (form) {
                auto reference = form->As<RE::TESObjectREFR>();
                if (reference) {
                    logger::info("[RenameNPC] Found reference 0x{:08x} , name {}", intval,
                                 reference->GetDisplayFullName());

                    AIAgentManager& aiam = AIAgentManager::getInstance();

                    reference->SetDisplayName(newName.c_str(), true);

                    auto actor = reference->As<RE::Actor>();

                    if (actor) {
                        actor->SetDisplayName(newName.c_str(), true);
                        aiam.addRenamedNpc(actor->GetFormID(), reference->GetDisplayFullName());
                        if (AIAgentRoleMasterFaction) {
                            actor->AddToFaction(AIAgentRoleMasterFaction, 1);
                            logger::info("[RenameNPC] Actor {} added to Master Faction {}",newName.c_str(),
                                         AIAgentRoleMasterFaction->GetName());
                        }

                        auto agentPtr = aiam.getAgentByName(reference->GetDisplayFullName());
                        if (agentPtr) {
                            agentPtr->setActor(actor);
                        }
                        logger::info("[RenameNPC] Actor renamed  0x{:08x} to {}", intval, newName.c_str());
                    }
                    HTTPManager::log(std::format("enable_bg|{}|{}|{}/{:08X}", getCurrentTimeMillis(),
                                                 GetGameTimeStamp(), newName.c_str(), intval));

                    RE::DebugNotification(std::format("[CHIM] {} marked for Background Life (BgL).",newName).c_str());
                    std::string storedname(newName.c_str());
                    auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                    auto args = RE::MakeFunctionArguments(std::move(reference), std::move(storedname));
                    RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                        "AIAgentAIMind", "addRenamedKeyword", args, callback);

                } else {
                    logger::info("[RenameNPC] Reference 0x{:08x} is not a valid TESObjectREFR", intval);
                }
            }

            /*
            logger::info("[RenameNPC] Reference 0x{:08x} to {}, {}", intval, newName, splitResult[0]);
            auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
            auto args = RE::MakeFunctionArguments(std::move(intval), std::move(newName));
            RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentAIMind", "RenameNPC",
            args, callback);
            */
        }
    } else if (command.contains("BackgroundCmd")) {
        // External command to AI Mind. Thi version wont search for agent
        std::vector<std::string> splitResult = splitString(parameter);
        if (splitResult.size() != 2) {
            logger::info("Command has not enough parms {}", command);
        } else {

            // Pseudocode plan:
            // - The issue is with parsing hex strings like "0xFF0026FF" using std::stoul.
            // - std::stoul with base 0 should always parse "0x..." as hex, but locale or input issues may cause
            // problems.
            // - Add logging to show the string before parsing and the result after parsing.
            // - Add a fallback: if std::stoul fails, try std::stoul with base 16 explicitly.
            // - If both fail, log an error and set intval to 0.


            std::string backgroundCmdStr(splitResult[1].c_str());

            uint32_t intval = 0;
            try {
                logger::info("[BackgroundCmd] Attempting to parse '{}'", splitResult[0]);
                intval = std::stoul(splitResult[0], nullptr, 0);
                logger::info("[BackgroundCmd] Parsed value: 0x{:08x}", intval);
            } catch (const std::exception&) {
                // Try parsing as hex explicitly if the first attempt fails
                try {
                    intval = std::stoul(splitResult[0], nullptr, 16);
                    logger::info("[BackgroundCmd] Fallback hex parse: 0x{:08x}", intval);
                } catch (const std::exception&) {
                    logger::warn("Invalid integer parameter: '{}'", splitResult[0]);
                    intval = 0;  // default fallback
                }
            }

            RE::TESForm* form = RE::TESForm::LookupByID(intval);

            if (!form) {
                logger::info("[BackgroundCmd] Reference 0x{:08x} <{}>, is not a valid form", intval, splitResult[0]);
            } else {
                logger::info("[BackgroundCmd] Reference 0x{:08x} is a valid form", form->GetFormID());
                
                auto reference = form->As<RE::TESObjectREFR>();
                if (reference) {
                    auto actor = reference->As<RE::Actor>();

                    logger::info("[BackgroundCmd] Reference {} is a valid reference", reference->GetDisplayFullName());

                    auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                    auto args = RE::MakeFunctionArguments(std::move(form), std::move(backgroundCmdStr));

                    RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                        "AIAgentAIMind", "BackgroundCmd", args, callback);
                } else {
                    logger::info("[BackgroundCmd] Reference 0x{:08x} is not a valid TESObjectREFR", intval);
                }
            }
        }
    } else if (command.contains("ScriptProxy")) {
        // External command to AI Mind. Thi version wont search for agent
        std::vector<std::string> splitResult = splitString(parameter);
        if (splitResult.size() != 1) {
            logger::info("Command has not enough parms {}", command);
        } else {
            
            std::string jsonStr(splitResult[0].c_str());
            ScriptProxyRun(jsonStr);

        }
    } else if (command.contains("ShowTrainingMenu")) {
        std::vector<std::string> splitResult = splitString(parameter);
        if (splitResult.size() >= 1) {
            RE::Actor* trainerActor = nullptr;
            
            // First try to find as AIAgent
            AIAgentManager& aiam = AIAgentManager::getInstance();
            auto agentPtr = aiam.getAgentByName(splitResult[0]);
            if (agentPtr && agentPtr->getActor()) {
                trainerActor = agentPtr->getActor();
            } else {
                // If not an AIAgent, search for vanilla NPC by name
                auto player = RE::PlayerCharacter::GetSingleton();
                if (player) {
                    auto playerCell = player->GetParentCell();
                    auto targetRef = findActorInCell(splitResult[0], playerCell, player->As<RE::Actor>(), 2048, false);
                    if (targetRef) {
                        trainerActor = targetRef->As<RE::Actor>();
                    }
                }
            }
            
            if (trainerActor) {
                // Queue the menu opening on the main thread
                SKSE::GetTaskInterface()->AddTask([trainerActor]() {
                    if (!trainerActor) {
                        logger::error("[ShowTrainingMenu] Trainer actor is null in task");
                        return;
                    }
                    
                    // Call the native Papyrus function Game.ShowTrainingMenu
                    auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
                    if (vm) {
                        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                        RE::Actor* trainer = trainerActor;
                        auto args = RE::MakeFunctionArguments(std::move(trainer));
                        vm->DispatchStaticCall("Game", "ShowTrainingMenu", args, callback);
                    } else {
                        logger::error("[ShowTrainingMenu] Could not get VirtualMachine");
                    }
                });
            } else {
                logger::error("[ShowTrainingMenu] Could not find trainer actor: {}", splitResult[0]);
            }
        }
    }

    responsePop("rolecommand");
}

void parseCommand(std::string rawCommand, std::string actorname) {
    static std::string delimiter = "@";
    size_t pos = rawCommand.find(delimiter);
    if (pos == std::string::npos) {
        responsePop("command");
        return;
    }

    AIAgentManager& aiam = AIAgentManager::getInstance();
    auto agentPtr = aiam.getAgentByName(actorname);

    /*
    if (!agentPtr) {
        logger::info("No AI actor found");
        responsePop("command");

        if (actorname == aiam.getPlayerName()) {
            std::string command = rawCommand.substr(0, pos);
            std::string parameter = rawCommand.substr(pos + delimiter.length());

            if (command.contains("TravelTo")) {
                responsePop("command");

                auto player = RE::PlayerCharacter::GetSingleton();

                auto locationForm = findLocation(parameter);

                if (locationForm) {
                    RE::BGSLocation* location = locationForm->As<RE::BGSLocation>();
                    logger::info("Location target: {}", location->GetName());

                    auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                    auto args = RE::MakeFunctionArguments(std::move(player->As<RE::Actor>()),
                                                          std::move(location->worldLocMarker.get().get()),
                                                          std::move(location->GetName()));
                    RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                        "AIAgentAIMind", "TravelToTargetPlayer", args, callback);
                }
            }
        }
        return;
    }
    */

    if (!agentPtr) {
        logger::info("No actorptr found for {}", actorname);
        responsePop("command");
        return;
    }
    RE::Actor* targetActor = agentPtr->getActor();

    if (!targetActor) {
        logger::info("No actor found for {}", actorname);
        responsePop("command");
        return;
    }

    std::string command = rawCommand.substr(0, pos);
    std::string parameter = rawCommand.substr(pos + delimiter.length());

    SPGResponse& spgResponse = SPGResponse::getInstance();

    if (command.contains("Halt") || spgResponse.findInQueue("command", "Halt") ||
        spgResponse.findInQueue("command", "Relax") || (spgResponse.findInQueue("command", "ToggleModel"))) {
        logger::info("Stop");
        agentPtr->setCommandBusy(false);
        agentPtr->setExternalLocked(false);
        agentPtr->setAnimationBusy(false);
        ;
        /* if ((spgResponse.findInQueue(qName, "Follow")) &&
            (spgResponse.findInQueue(qName, RE::PlayerCharacter::GetSingleton()->GetName()))) {
            HTTPLogger->critical("funcret|{}|{}|{}{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                 "command@Follow@@#HERIKA_NPC1# follows ",
                                 RE::PlayerCharacter::GetSingleton()->GetName());
        }

        if ((spgResponse.findInQueue("command", "Relax"))) {
            HTTPLogger->critical("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                 "command@Relax@@#HERIKA_NPC1# takes a relaxed pose ");
        }

        */
        if ((spgResponse.findInQueue("command", "ToggleModel"))) {
            RE::DebugNotification(
                std::format("[CHIM] Model changed to {} for {} ", trim(parameter), agentPtr->getActorName()).c_str());
        }

        if ((spgResponse.findInQueue("command", "Relax"))) {
            HTTPManager::log(std::format("force_current_task|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                         "Resting and relaxing."));

            /*
            * Considder
            HTTPManager::stream(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                            "command@" + command  + "@#HERIKA_NPC1# is relaxed now.")
                                , agentPtr->getActor()
                                );
            */
            responsePop("command");
            clearQueue("command");
            return;
        }

        StopCurrent(agentPtr->getActor());
        responsePop("command");
        clearQueue("command");
        return;
        // HTTPLogger->error("info|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(), "Herika: ");

    } else if (command.contains("AddBounty")) {
        responsePop("command");

        auto npc = agentPtr->getActor();
        auto player = RE::PlayerCharacter::GetSingleton()->As<RE::Actor>();
        if (!npc || !player) {
            logger::info("[AddBounty] Missing npc or player actor");
            return;
        }

        auto crimeFaction = getCrimeFaction(npc);
        if (!crimeFaction) {
            HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                         "command@" + command + "@@Error: guard has no crime faction"),
                             npc);
            return;
        }

        std::string crimeType = trim(parameter);
        std::string customAmount;
        auto atPos = crimeType.find('@');
        if (atPos != std::string::npos) {
            customAmount = trim(crimeType.substr(atPos + 1));
            crimeType = trim(crimeType.substr(0, atPos));
        }

        if (crimeType.empty()) crimeType = "Assault";

        auto crime = getCrimeAmount(crimeType, customAmount);
        bool isViolent = crime.violent;

        HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                     "command@" + command + "@" + crimeType + "@" + std::to_string(crime.amount) +
                                         " gold bounty added"),
                         npc);

        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
        auto args = RE::MakeFunctionArguments(std::move(player), std::move(npc), std::move(crimeFaction),
                                              std::move(crime.amount), std::move(isViolent));
        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentAIMind", "AddBounty", args,
                                                                                   callback);

    } else if (command.contains("PayBounty")) {
        responsePop("command");

        auto npc = agentPtr->getActor();
        auto player = RE::PlayerCharacter::GetSingleton()->As<RE::Actor>();
        if (!npc || !player) {
            logger::info("[PayBounty] Missing npc or player actor");
            return;
        }

        auto crimeFaction = getCrimeFaction(npc);
        if (!crimeFaction) {
            HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                         "command@" + command + "@@Error: guard has no crime faction"),
                             npc);
            return;
        }

        int currentBounty = static_cast<int>(crimeFaction->GetCrimeGold());
        HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                     "command@" + command + "@@" + std::to_string(currentBounty) +
                                         " gold bounty paid"),
                         npc);

        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
        auto args = RE::MakeFunctionArguments(std::move(player), std::move(npc), std::move(crimeFaction));
        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentAIMind", "PayBounty", args,
                                                                                   callback);

    } else if (command.contains("ArrestPlayer")) {
        responsePop("command");

        auto npc = agentPtr->getActor();
        auto player = RE::PlayerCharacter::GetSingleton()->As<RE::Actor>();
        if (!npc || !player) {
            logger::info("[ArrestPlayer] Missing npc or player actor");
            return;
        }

        auto crimeFaction = getCrimeFaction(npc);
        if (!crimeFaction) {
            HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                         "command@" + command + "@@Error: guard has no crime faction"),
                             npc);
            return;
        }

        HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                     "command@" + command + "@@Player arrested and sent to jail"),
                         npc);

        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
        auto args = RE::MakeFunctionArguments(std::move(player), std::move(npc), std::move(crimeFaction));
        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentAIMind", "ArrestPlayer",
                                                                                   args, callback);

    } else if (command.contains("ForgiveCrime")) {
        responsePop("command");

        auto npc = agentPtr->getActor();
        auto player = RE::PlayerCharacter::GetSingleton()->As<RE::Actor>();
        if (!npc || !player) {
            logger::info("[ForgiveCrime] Missing npc or player actor");
            return;
        }

        auto crimeFaction = getCrimeFaction(npc);
        if (!crimeFaction) {
            HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                         "command@" + command + "@@Error: guard has no crime faction"),
                             npc);
            return;
        }

        HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                     "command@" + command + "@@Bounty forgiven"),
                         npc);

        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
        auto args = RE::MakeFunctionArguments(std::move(player), std::move(npc), std::move(crimeFaction));
        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentAIMind", "ForgiveCrime",
                                                                                   args, callback);

    } else if (command.contains("Attack")) {
        if (agentPtr.get()->isCommandBusy()) return;
        logger::info("Start Attack: Target:{} Actor:{} ", parameter, targetActor->GetDisplayFullName());
        StartAttack(trim(parameter), targetActor, true);
        responsePop("command");

        // HTTPLogger->error("info|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(), "Herika: ");
    } else if (command.contains("Brawl")) {
        if (agentPtr.get()->isCommandBusy()) {
            logger::info("Start Attack: Target:{} Actor:{}, busy with command: {} ", parameter,
                         targetActor->GetDisplayFullName(), agentPtr.get()->getCurrentCommand());
            return;
        }
        logger::info("Start Attack: Target:{} Actor:{} ", parameter, targetActor->GetDisplayFullName());
        StartAttack(trim(parameter), targetActor, false);
        responsePop("command");

        // HTTPLogger->error("info|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(), "Herika: ");
    } else if (command.contains("OpenInventory")) {
        if (agentPtr.get()->isCommandBusy()) return;

        responsePop("command");

        auto player = RE::PlayerCharacter::GetSingleton();
        float distance = targetActor->GetPosition().GetDistance(player->GetPosition());
        if (distance > 1000) {
            logger::info("{} is {} units away (too far)", agentPtr->getActorName(), distance);

            return;
        }

        RE::Actor* pendingMerchantActor = ResolvePendingBarterMerchant(targetActor);
        SetPendingBarterMerchant(pendingMerchantActor);

        const auto scriptFactory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
        const auto script = scriptFactory ? scriptFactory->Create() : nullptr;
        if (script) {
            float scale = 0.5f;

            auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
            auto args = RE::MakeFunctionArguments(std::move(targetActor), std::move(command));

            RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentAIMind", "OpenInventory",
                                                                                       args, callback);
        };
        /*
        HTTPManager::log(std::format("infoaction|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                         "(Context: "+targetActor->GetDisplayFullName()+"" issued {" + command + "(" + trim(parameter) +
                             " )})( #HERIKA_NPC1# trade things with " + RE::PlayerCharacter::GetSingleton()->GetName() +
        ") " ), targetActor);
        */
        HTTPManager::log(std::format("infoaction|{}|{}|{} opens inventory", getCurrentTimeMillis(), GetGameTimeStamp(),
                                     targetActor->GetDisplayFullName()),
                         targetActor);

        // HTTPLogger->error("info|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(), "Herika: ");
    } else if (command.contains("SetCurrentTask")) {
        responsePop("command");
        std::string buffer;
        std::string equiped;

        HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                     "command@" + command + "@" + trim(parameter) + "@"),
                         targetActor);
        /*

        */
        /*
        HTTPLogger->critical("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                             "command@" + command + "@" + trim(parameter) + "@");*/

    } else if (command.contains("MoveTo")) {
        responsePop("command");

        auto npc = agentPtr->getActor();
        auto player = RE::PlayerCharacter::GetSingleton();
        if (!npc || !player) {
            return;
        }

        float distance = npc->GetPosition().GetDistance(player->GetPosition());
        if (distance > 2048) {
            logger::info("{} is {} units away (too far)", npc->GetDisplayFullName(), distance);
            HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                         "command@" + command + "@" + trim(parameter) +
                                             "@Error: actor is too far away"),
                             npc);
            return;
        }

        if (agentPtr.get()->isCommandBusy()) {
            logger::info("[CHIM] Actor {} busy : {}", agentPtr.get()->getActorName(),
                         agentPtr.get()->getCurrentCommand().c_str());
            RE::DebugNotification(std::format("[CHIM] Actor {} busy : {}", agentPtr.get()->getActorName(),
                                              agentPtr.get()->getCurrentCommand())
                                      .c_str());
            return;
        }

        std::string targetName = trim(parameter);
        if (targetName.empty()) {
            HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                         "command@" + command + "@" + targetName + "@Error: missing target"),
                             npc);
            return;
        }

        auto playerActor = player->As<RE::Actor>();
        auto normalizeActorName = [](std::string value) {
            value = trim(value);
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        };

        const std::string normalizedTargetName = normalizeActorName(targetName);
        bool targetIsPlayer = normalizedTargetName == "player";
        if (!targetIsPlayer && playerActor) {
            const std::string playerDisplayName = normalizeActorName(playerActor->GetDisplayFullName());
            const std::string playerName = normalizeActorName(playerActor->GetName());
            targetIsPlayer = (!playerDisplayName.empty() && normalizedTargetName == playerDisplayName) ||
                             (!playerName.empty() && normalizedTargetName == playerName);
        }

        RE::TESObjectREFR* target = nullptr;
        RE::Actor* targetAsActor = nullptr;
        if (targetIsPlayer) {
            target = playerActor;
            targetAsActor = playerActor;
        } else {
            target = findActorInCell(targetName, npc->GetParentCell(), npc, 2048, false);
            targetAsActor = target ? target->As<RE::Actor>() : nullptr;
        }

        if (!target || !targetAsActor) {
            logger::info("[MoveTo] target {} not found", targetName);
            HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                         "command@" + command + "@" + targetName + "@Error: target not found"),
                             npc);
            return;
        }

        std::string resolvedTargetName(targetAsActor->GetDisplayFullName());
        if (resolvedTargetName.empty()) resolvedTargetName = targetName;

        const std::string notificationText =
            std::format("[CHIM] {} moves to {}", npc->GetDisplayFullName(), resolvedTargetName);
        RE::DebugNotification(notificationText.c_str());

        HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                     "command@" + command + "@" + resolvedTargetName + "@" +
                                         npc->GetDisplayFullName() + " starts moving to " + resolvedTargetName),
                         npc);

        int intent = 0;
        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
        auto args = RE::MakeFunctionArguments(std::move(npc), std::move(target), std::move(intent));
        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentAIMind", "MoveToTarget",
                                                                                   args, callback);

        agentPtr.get()->setCurrentCommand("MoveTo");

    } else if (command.contains("TravelToRaw")) {
        // This used when server finds location on database, and sends formid
        responsePop("command");

        auto player = RE::PlayerCharacter::GetSingleton();
        float distance = targetActor->GetPosition().GetDistance(player->GetPosition());

        if (distance > 2048) {
            logger::info("{} is {} units away (too far)", targetActor->GetDisplayFullName(), distance);
            return;
        }

        if (agentPtr.get()->isCommandBusy()) {
            logger::info("[CHIM] Actor {} busy : {}", agentPtr.get()->getActorName(),
                         agentPtr.get()->getCurrentCommand().c_str());
            RE::DebugNotification(std::format("[CHIM] Actor {} busy : {}", agentPtr.get()->getActorName(),
                                              agentPtr.get()->getCurrentCommand())
                                      .c_str());
            return;
        }

        std::uint32_t formIdRec = atoi(parameter.c_str());

        auto locationForm = RE::TESForm::LookupByID(formIdRec);

        RE::TESForm* markerForm = nullptr;

        if (locationForm) {
            auto value = locationForm->As<RE::BGSLocation>();
            for (int i = 0; i < value->specialRefs.size(); i++) {
                auto specialRefs = value->specialRefs[i];
                auto ref = RE::TESForm::LookupByID(specialRefs.refData.refID);  // Get Markers

                if (ref) {
                    // logger::info(" 0x{:08x} ", specialRefs.refData.refID);
                    if (ref->GetFormType() == RE::FormType::Reference) {
                        auto refFinal = ref->As<RE::TESObjectREFR>();
                        if (refFinal->GetBaseObject()->GetFormID() == 0x3b) {
                            logger::info("Early XMarker reference found {} 0x{:08x}", ref->GetFormEditorID(),
                                         ref->GetFormID());
                            markerForm = refFinal;
                            break;
                        } else if (refFinal->GetBaseObject()->GetFormID() == 0x10) {
                            logger::info("Early MapMarker reference found {} 0x{:08x}", ref->GetFormEditorID(),
                                         ref->GetFormID());
                            markerForm = refFinal;
                            break;
                        }
                    }
                }
            }
            if (!markerForm) {
                auto value = locationForm->As<RE::BGSLocation>();
                if (value->worldLocMarker) {
                    auto wmarkerPtr = value->worldLocMarker.get();
                    if (wmarkerPtr) {
                        auto wmarker = wmarkerPtr.get();
                        if (wmarker) {
                            logger::info("[TRAVELTORAW] Found Worldloc marker reference {} 0x{:08x}",
                                         wmarker->GetFormEditorID(), wmarker->GetFormID());
                            markerForm = wmarker;
                        }
                    }
                }
            }
            if (markerForm) {
                // RE::BGSLocation* location = locationForm->As<RE::BGSLocation>();
                std::string locatioName(trim(locationForm->GetName()));
                logger::info("Location target: {}", locatioName);
                auto locationRef = markerForm->AsReference();
                if (GetPlayerLocation() == locatioName) {
                    HTTPManager::stream(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                                    "command@TravelTo@" + locatioName + "@Error, you're already there"),
                                        targetActor);
                } else {
                    HTTPManager::stream(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                                    "command@TravelTo@" + locatioName + "@" + agentPtr->getActorName() +
                                                        " starts traveling to " + locatioName + " , current location " +
                                                        GetPlayerLocation()),
                                        targetActor);

                    auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                    auto args = RE::MakeFunctionArguments(std::move(targetActor), std::move(locationRef),
                                                          std::move(locatioName));
                    RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                        "AIAgentAIMind", "TravelToLocation", args, callback);

                    agentPtr.get()->setCurrentCommand("TravelTo");
                    // agentPtr.get()->setCommandBusy(true);
                }
            } else {
                logger::info("[COMMAND TravelToRaw] No markerForm found for  {:X}", formIdRec);
                HTTPManager::log(
                    std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                "command@TravelTo@" + trim(parameter) + "@You cannot travel to place, path is unknown"),
                    targetActor);
            }
        } else {
            logger::info("[COMMAND TravelToRaw] No location found for  {:X}", formIdRec);
            HTTPManager::log(
                std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                            "command@TravelTo@" + trim(parameter) + "@Error: target " + parameter + " not known"),
                targetActor);
        }

    } else if (command.contains("TravelTo")) {
        responsePop("command");

        auto player = RE::PlayerCharacter::GetSingleton();
        float distance = targetActor->GetPosition().GetDistance(player->GetPosition());

        if (distance > 2048) {
            logger::info("{} is {} units away (too far)", targetActor->GetDisplayFullName(), distance);
            return;
        }

        if (agentPtr.get()->isCommandBusy()) {
            RE::DebugNotification(std::format("[CHIM] Actor {} busy : {}", agentPtr.get()->getActorName(),
                                              agentPtr.get()->getCurrentCommand())
                                      .c_str());
            return;
        }

        auto locationForm = findLocation(parameter);

        if (locationForm) {
            // RE::BGSLocation* location = locationForm->As<RE::BGSLocation>();
            std::string locatioName(trim(parameter));
            logger::info("Location target: {}", locatioName);

            if (GetPlayerLocation() == locatioName) {
                HTTPManager::stream(
                    std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                "command@" + command + "@" + locatioName + "@Error, you're already there"),
                    targetActor);
            } else {
                auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                auto args = RE::MakeFunctionArguments(std::move(targetActor), std::move(locationForm->AsReference()),
                                                      std::move(parameter));
                RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                    "AIAgentAIMind", "TravelToLocation", args, callback);

                HTTPManager::stream(
                    std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                "command@" + command + "@" + locatioName + "@#HERIKA_NPC1# starts traveling to " +
                                    locatioName + " , current location " + GetPlayerLocation()),
                    targetActor);

                agentPtr.get()->setCurrentCommand("TravelTo");
                // agentPtr.get()->setCommandBusy(true);
            }
        } else {
            HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                         "command@" + command + "@" + trim(parameter) + "@Error: target " + parameter +
                                             " not known"),
                             targetActor);
        }

    } else if (command.contains("CheckInventory")) {
        responsePop("command");

        std::string buffer;
        std::string equiped;
        auto agentActor = agentPtr->getActor();

        auto inventory = agentActor->GetInventory();
        for (const auto& item : inventory) {
            RE::TESBoundObject* boundObject = item.first;
            RE::TESObjectREFR::Count count = item.second.first;
            const std::unique_ptr<RE::InventoryEntryData>& entryData = item.second.second;
            equiped.assign("");
            if (item.second.second != nullptr) {
                auto extraList = entryData.get()->extraLists;
                if (extraList) {
                    for (RE::ExtraDataList* el : *extraList) {
                        for (RE::ExtraDataList::iterator it = el->begin(); it != el->end(); ++it) {
                            RE::BSExtraData* extraData = &(*it);
                            if (extraData->GetType() == RE::ExtraDataType::kWorn)  // ExtraDataType::kAction
                                equiped.assign("(currently equipped)");
                        }
                    }
                }
            }
            buffer.append(boundObject->GetName()).append(equiped).append(",");
        }

        HTTPManager::stream(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                        "command@" + command + "@#HERIKA_NPC1#@" + buffer),
                            agentActor);

    } else if (command.contains("IncreaseWalkSpeed")) {
        auto herika = agentPtr->getActor();

        RE::TESPackage* package = herika->GetActorRuntimeData().currentProcess->GetRunningPackage();

        if (!package) {
            responsePop("command");
            return;
        }

        RE::PACKAGE_DATA* pdata = &package->packData;

        logger::info("Current package: {} {} {}", package->GetFormID(), package->GetFormEditorID(),
                     pdata->maxSpeed.underlying());

        pdata->maxSpeed.reset(pdata->maxSpeed.get());

        if (trim(parameter) == "run") {
            pdata->maxSpeed.set(RE::PACKAGE_DATA::PreferredSpeed::kRun);
            // actorPackage->preferredSpeed = 2;
        } else if (trim(parameter) == "fastwalk") {
            pdata->maxSpeed.set(RE::PACKAGE_DATA::PreferredSpeed::kFastWalk);
            // actorPackage->preferredSpeed = 3;
        } else if (trim(parameter) == "jog") {
            pdata->maxSpeed.set(RE::PACKAGE_DATA::PreferredSpeed::kJog);
            // actorPackage->preferredSpeed = 1;
        } else {
            pdata->maxSpeed.set(RE::PACKAGE_DATA::PreferredSpeed::kRun);
            // actorPackage->preferredSpeed = 0;
        }
        pdata->packFlags.reset(RE::PACKAGE_DATA::GeneralFlag::kPreferredSpeed);
        pdata->packFlags.set(RE::PACKAGE_DATA::GeneralFlag::kPreferredSpeed);

        herika->EvaluatePackage(true, true);

        responsePop("command");

        return;
    } else if (command.contains("DecreaseWalkSpeed")) {
        auto herika = agentPtr->getActor();

        RE::TESPackage* package = herika->GetActorRuntimeData().currentProcess->GetRunningPackage();

        if (!package) {
            responsePop("command");
            return;
        }

        RE::PACKAGE_DATA* pdata = &package->packData;

        logger::info("Current package: {} {} {}", package->GetFormID(), package->GetFormEditorID(),
                     pdata->maxSpeed.underlying());

        pdata->maxSpeed.reset(pdata->maxSpeed.get());

        if (trim(parameter) == "run") {
            pdata->maxSpeed.set(RE::PACKAGE_DATA::PreferredSpeed::kRun);
            // actorPackage->preferredSpeed = 2;
        } else if (trim(parameter) == "fastwalk") {
            pdata->maxSpeed.set(RE::PACKAGE_DATA::PreferredSpeed::kFastWalk);
            // actorPackage->preferredSpeed = 3;
        } else if (trim(parameter) == "jog") {
            pdata->maxSpeed.set(RE::PACKAGE_DATA::PreferredSpeed::kJog);
            // actorPackage->preferredSpeed = 1;
        } else {
            pdata->maxSpeed.set(RE::PACKAGE_DATA::PreferredSpeed::kWalk);
            // actorPackage->preferredSpeed = 0;
        }
        pdata->packFlags.reset(RE::PACKAGE_DATA::GeneralFlag::kPreferredSpeed);
        pdata->packFlags.set(RE::PACKAGE_DATA::GeneralFlag::kPreferredSpeed);

        herika->EvaluatePackage(true, true);

        responsePop("command");

        return;
    } else if (command.contains("ReadQuestJournal")) {
        responsePop("command");
        std::string buffer;
        std::string equiped;
        auto herika = agentPtr;

        if (herika->isAvailableforAnimation()) {
            // player->setReadingBook(true);
            auto npc = herika->getActor();

            auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
            auto args = RE::MakeFunctionArguments(std::move(npc), std::move(0x00089975));

            RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentAIMind", "PlayIdle",
                                                                                       args, callback);
        }

        HTTPManager::stream(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                        "command@" + command + "@" + trim(parameter) + "@"),
                            herika->getActor());

    } else if (command.contains("SearchMemory")) {
        responsePop("command");
        std::string buffer;
        std::string equiped;
        auto herika = agentPtr;

        HTTPManager::stream(std::format("memory|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(), trim(parameter)),
                            herika->getActor());

    } else if (command.contains("InspectSurroundings") || (command.contains("LookAround"))) {
        auto herika = agentPtr;
        auto npc = herika->getActor();

        RE::TESObjectCELL* cell = npc->GetParentCell();
        if (cell) {
            if (cell->IsInteriorCell()) {
                logger::info("[COMMAND InspectSurroundings] {} is in interior cell {}", npc->GetDisplayFullName(),
                             cell->GetName());
            } else {
                logger::info("[COMMAND InspectSurroundings] {} is in exterior cell {}", npc->GetDisplayFullName(),
                             cell->GetName());
            }
        } else {
            logger::info("[COMMAND InspectSurroundings] {} is in no cell", npc->GetDisplayFullName());
        }
        const std::string result =
            InspectSurroundings(npc->AsReference(), false, HERIKA_MAX_VISION_RANGE, ",", HERIKA_MAX_VISION_RANGE);
        responsePop("command");

        if (herika->isAvailableforAnimation()) commandAnimation("IdleLookFar", npc);

        HTTPManager::stream(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                        "command@" + command + "@@" + result),
                            npc);

    } else if (command.contains("Inspect")) {
        responsePop("command");
        std::string buffer;
        std::string equiped;
        auto herika = agentPtr;
        auto npc = herika->getActor();

        parameter = removeTextInParenthesesAndTrim(parameter);
        parameter = removeTextAfterLastQuote(parameter);

        auto target = findActorInCell(trim(parameter), npc->GetParentCell(), npc, 1024, true);

        if (trim(parameter).empty()) {
            HTTPManager::stream(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                            "command@" + command + "@" + trim(parameter) + "@Error: missing target"),
                                targetActor);

        } else {
            if (target) {
                auto targetPos = target->GetPosition();

                logger::info("[COMMAND INSPECT] Inspecting {} {:X}", target->GetDisplayFullName(), target->GetFormID());
                auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();

                bool deadFlag = false;
                if (target->IsDead()) {
                    deadFlag = true;
                }
                auto inventory = target->GetInventory();
                for (const auto& item : inventory) {
                    RE::TESBoundObject* boundObject = item.first;
                    RE::TESObjectREFR::Count count = item.second.first;
                    const std::unique_ptr<RE::InventoryEntryData>& entryData = item.second.second;
                    equiped.assign("");
                    if (item.second.second != nullptr) {
                        auto extraList = entryData.get()->extraLists;
                        if (extraList) {
                            for (RE::ExtraDataList* el : *extraList) {
                                for (RE::ExtraDataList::iterator it = el->begin(); it != el->end(); ++it) {
                                    RE::BSExtraData* extraData = &(*it);
                                    if (extraData->GetType() == RE::ExtraDataType::kWorn)  // ExtraDataType::kAction
                                        equiped.assign("(currently equipped)");
                                }
                            }
                        }
                    }

                    if (!equiped.empty()) {
                        // Things visible to others
                        buffer.append(entryData->GetDisplayName()).append(",");
                    } else {
                        auto actor = target->As<RE::Actor>();
                        if (actor) {
                            // Loot inspect
                            if (actor->IsDead()) {
                                deadFlag = true;
                                buffer.append(entryData->GetDisplayName()).append(",");
                            }
                        }
                    }
                }
                std::string inspectedActor = target->GetDisplayFullName();

                std::string responseMsg(trim(inspectedActor));
                responseMsg.append(" is wearing: ");
                responseMsg.append(buffer);
                if (deadFlag) {
                    responseMsg.append("(note: this actor is dead)");
                }
                HTTPManager::stream(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                                "command@" + command + "@" + trim(inspectedActor) + "@" + responseMsg),
                                    targetActor);

                if (deadFlag) {
                    std::string place(target->GetDisplayFullName());
                    place.append("'s corpse");

                    auto args = RE::MakeFunctionArguments(std::move(npc), std::move(target), std::move(place));
                    RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                        "AIAgentAIMind", "TravelToTarget", args, callback);
                }

            } else {
                RE::DebugNotification(std::string("[CHIM] Target not found " + trim(parameter)).c_str());

                HTTPManager::stream(
                    std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                "command@" + command + "@" + trim(parameter) + "@Error: target not found"),
                    npc);
            }
        }
    } else if (command.contains("TakeASeat")) {
        responsePop("command");
        std::string buffer;

        auto herika = agentPtr;
        auto npc = herika->getActor();
        auto player = RE::PlayerCharacter::GetSingleton();

        RE::FormID furniture = findFurnitureInCell(player->GetParentCell(), player->As<RE::Actor>(), 0);

        auto occupedFurn = herika->getActor()->GetOccupiedFurniture();
        std::string actualFurn;
        if (occupedFurn) {
            // Check if already on seat
            if (occupedFurn.get())
                if (occupedFurn.get()->As<RE::TESFurniture>()) 
                    if (occupedFurn.get()->As<RE::TESFurniture>()->workBenchData.benchType == RE::TESFurniture::WorkBenchData::BenchType::kNone) 
                        actualFurn.assign(occupedFurn.get()->GetDisplayFullName());

        }
        if (!actualFurn.empty()) {
            if (GlobalRechatPolicyAsap == 1) {
                HTTPManager::stream(
                    std::format("funcret|{}|{}|{} ({})", getCurrentTimeMillis(), GetGameTimeStamp(),
                                "command@" + command + "@" + trim(parameter) +
                                    "@take a seat error, #HERIKA_NPC1# is currently using " + actualFurn,
                                npc->GetDisplayFullName()),
                    npc);
            } else {
                HTTPManager::log(std::format("infoaction|{}|{}|{} is sitting/using {}", getCurrentTimeMillis(),
                                             GetGameTimeStamp(), npc->GetDisplayFullName(), actualFurn));
                logger::info("{} is using furniture {}", npc->GetDisplayFullName(), actualFurn);
            }
        } else {
            if (furniture > 0) {
                RE::TESObjectREFR* furnitureForm = RE::TESForm::LookupByID(furniture)->AsReference();
                logger::info("Furniture: {}", furnitureForm->GetName());
                auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                auto args = RE::MakeFunctionArguments(std::move(npc), std::move(furnitureForm));
                RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentAIMind", "TakeASeat",
                                                                                           args, callback);

                if (GlobalRechatPolicyAsap == 1) {
                    HTTPManager::stream(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                                    "command@" + command + "@" + trim(parameter) +
                                                        "@#HERIKA_NPC1# sits at " + furnitureForm->GetName()),
                                        npc);
                } else {
                    HTTPManager::log(std::format("infoaction|{}|{}|{} seated now on {} ", getCurrentTimeMillis(),
                                                 GetGameTimeStamp(), npc->GetDisplayFullName(),
                                                 furnitureForm->GetName()));
                }

            } else {
                if (GlobalRechatPolicyAsap == 1) {
                    HTTPManager::stream(std::format("funcret|{}|{}|{} ({})", getCurrentTimeMillis(), GetGameTimeStamp(),
                                                    "command@" + command + "@" + trim(parameter) +
                                                        "@#HERIKA_NPC1# could not find any place to sit",
                                                    npc->GetDisplayFullName()),
                                        npc);
                }
            }
        }
    } else if (command.contains("GoToSleep")) {
        responsePop("command");
        std::string buffer;

        auto herika = agentPtr;
        auto npc = herika->getActor();
        auto player = RE::PlayerCharacter::GetSingleton();

        RE::FormID furniture = findFurnitureInCell(player->GetParentCell(), player->As<RE::Actor>(), 1);

        if (furniture > 0) {
            RE::TESObjectREFR* furnitureForm = RE::TESForm::LookupByID(furniture)->AsReference();
            logger::info("Furniture: {}", furnitureForm->GetName());
            auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
            auto args = RE::MakeFunctionArguments(std::move(npc), std::move(furnitureForm));
            RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentAIMind", "SleepInBed",
                                                                                       args, callback);

            if (GlobalRechatPolicyAsap == 1) {
                HTTPManager::stream(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                                "command@" + command + "@" + trim(parameter) +
                                                    "@#HERIKA_NPC1# sleeps at " + furnitureForm->GetName()),
                                    npc);
            } else {
                HTTPManager::log(std::format("infoaction|{}|{}|{} going to sleep on {} ", getCurrentTimeMillis(),
                                             GetGameTimeStamp(), npc->GetDisplayFullName(), furnitureForm->GetName()));
            }

        } else {
            if (GlobalRechatPolicyAsap == 1) {
                HTTPManager::stream(std::format("funcret|{}|{}|{} ({})", getCurrentTimeMillis(), GetGameTimeStamp(),
                                                "command@" + command + "@" + trim(parameter) +
                                                    "@#HERIKA_NPC1# could not find any place to sleep",
                                                npc->GetDisplayFullName()),
                                    npc);
            }
        }

    } else if (command.contains("WaitHere")) {
        responsePop("command");
        std::string buffer;

        auto npc = agentPtr->getActor();
        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
        auto args = RE::MakeFunctionArguments(std::move(npc));

        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentAIMind", "StartWait", args,
                                                                                   callback);

    } else if (command.contains("Surrender")) {
        responsePop("command");
        auto npc = agentPtr->getActor();
        if (npc && agentPtr->isAvailableforAnimation()) {
            commandAnimation("ArmsRaised", npc);
        }

    } else if (command.contains("UseSoulGaze")) {
        responsePop("command");
        std::string buffer;
        int sgmodelocal = sgmode;

        auto args = RE::MakeFunctionArguments(std::move(sgmodelocal));

        SKSE::GetTaskInterface()->AddTask([args]() {
            // actor->NotifyAnimationGraph(anim);
            auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
            RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentSoulGazeEffect",
                                                                                       "Soulgaze", args, callback);
        });

    } else if (command.contains("CastSpell")) {
        responsePop("command");
        
        // Parse parameters - server sends as JSON: {"target":"RANGROO","item":"Healing Hands"}
        std::string spellName = "";
        std::string targetName = "";
        
        try {
            // Try to parse as JSON
            auto paramJson = json::parse(parameter);
            if (paramJson.contains("item")) {
                spellName = paramJson["item"].get<std::string>();
            }
            if (paramJson.contains("target")) {
                targetName = paramJson["target"].get<std::string>();
            }
        } catch (...) {
            // Fallback to simple string parsing if JSON parse fails
            std::string paramTrimmed = trim(parameter);
            size_t atPos = paramTrimmed.find("@");
            if (atPos != std::string::npos) {
                spellName = trim(paramTrimmed.substr(0, atPos));
                targetName = trim(paramTrimmed.substr(atPos + 1));
            } else {
                spellName = paramTrimmed;
            }
        }
        
        // Validate we got a spell name
        if (spellName.empty()) {
            logger::warn("CastSpell called without spell name for {}", actorname);
            HTTPManager::log(std::format("funcret|{}|{}|{}", 
                getCurrentTimeMillis(), GetGameTimeStamp(),
                "command@CastSpell@Error: No spell specified"),
                targetActor);
            return;
        }
        
        logger::info("CastSpell: spell='{}', target='{}', actor='{}'", spellName, targetName, actorname);
        
        // Find the spell in the actor's spell list (check both base and runtime-added spells)
        RE::SpellItem* spellToCast = nullptr;
        
        // Helper lambda to strip spell name suffixes
        auto stripSpellSuffix = [](const std::string& name) -> std::string {
            std::string cleanName = name;
            size_t colonPos = cleanName.find(": Added Spell");
            if (colonPos != std::string::npos) {
                cleanName = cleanName.substr(0, colonPos);
            } else {
                colonPos = cleanName.find(": Base Spell");
                if (colonPos != std::string::npos) {
                    cleanName = cleanName.substr(0, colonPos);
                }
            }
            return cleanName;
        };
        
        // First, check base spell list
        auto actorBase = targetActor->GetActorBase();
        if (actorBase && !spellToCast) {
            auto spellList = actorBase->GetSpellList();
            if (spellList && spellList->numSpells > 0) {
                for (uint32_t i = 0; i < spellList->numSpells; i++) {
                    auto spell = spellList->spells[i];
                    if (spell && spell->GetName()) {
                        std::string cleanSpellName = stripSpellSuffix(spell->GetName());
                        if (cleanSpellName == spellName) {
                            spellToCast = spell;
                            break;
                        }
                    }
                }
            }
        }
        
        // If not found in base list, check runtime-added spells (learned during gameplay)
        if (!spellToCast) {
            try {
                auto& addedSpells = targetActor->GetActorRuntimeData().addedSpells;
                for (auto* spellForm : addedSpells) {
                    if (spellForm) {
                        auto spell = spellForm->As<RE::SpellItem>();
                        if (spell && spell->GetName()) {
                            std::string cleanSpellName = stripSpellSuffix(spell->GetName());
                            if (cleanSpellName == spellName) {
                                spellToCast = spell;
                                break;
                            }
                        }
                    }
                }
            } catch (...) {
                logger::trace("Exception checking runtime spells for {}", actorname);
            }
        }
        
        if (!spellToCast) {
            logger::warn("Spell '{}' not found in {}'s spell list (checked base and runtime spells)", spellName, actorname);
            HTTPManager::log(std::format("funcret|{}|{}|{}", 
                getCurrentTimeMillis(), GetGameTimeStamp(),
                "command@CastSpell@Error: " + spellName + " not known"),
                targetActor);
            return;
        }
        
        logger::info("Found spell '{}' for {}", spellName, actorname);
        
        // Get spell properties
        auto castingType = spellToCast->GetCastingType();
        uint32_t deliveryType = 0;
        if (spellToCast->effects.size() > 0) {
            auto effect = spellToCast->effects[0];
            if (effect && effect->baseEffect) {
                deliveryType = static_cast<uint32_t>(effect->baseEffect->data.delivery);
            }
        }
        
        logger::info("Spell properties: casting={}, delivery={}", 
            static_cast<uint32_t>(castingType), deliveryType);
        
        // Find target actor (or use location for Target Location spells)
        RE::Actor* targetToCastOn = nullptr;
        
        // Delivery type 4 = Target Location (e.g., Conjure spells, Runes)
        // For these, we don't need a target actor - just cast at a location
        if (deliveryType == 4) {
            // Target Location spell - try to find nearby target for location, else use caster's location
            AIAgentManager& aiam = AIAgentManager::getInstance();
            
            // If target specified, try to find them for their location
            if (!targetName.empty() && targetName != "self" && targetName != actorname && 
                targetName != "Target Location") {
                auto targetAgentPtr = aiam.getAgentByName(targetName);
                if (targetAgentPtr) {
                    targetToCastOn = targetAgentPtr->getActor();
                } else if (targetName == aiam.getPlayerName()) {
                    targetToCastOn = RE::PlayerCharacter::GetSingleton();
                }
            }
            
            // If no valid target found or target is "Target Location", use caster as reference point
            // The spell will be cast at a location near the caster
            if (!targetToCastOn) {
                targetToCastOn = targetActor;  // Use caster's location
            }
        } else {
            // Regular targeted spells (Self, Contact, Aimed, Target Actor)
            if (targetName.empty() || targetName == "self" || targetName == actorname) {
                targetToCastOn = targetActor;  // Cast on self
            } else {
                // Find target by name
                AIAgentManager& aiam = AIAgentManager::getInstance();
                auto targetAgentPtr = aiam.getAgentByName(targetName);
                if (targetAgentPtr) {
                    targetToCastOn = targetAgentPtr->getActor();
                } else if (targetName == aiam.getPlayerName()) {
                    targetToCastOn = RE::PlayerCharacter::GetSingleton();
                }
            }
        }
        
        // Dispatch to Papyrus based on casting/delivery type
        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
        
        // Concentration: charge to max before firing
        if (castingType == RE::MagicSystem::CastingType::kConcentration) {
            auto args = RE::MakeFunctionArguments(
                std::move(targetActor), 
                std::move(spellToCast->GetFormID()),
                std::move(targetToCastOn ? targetToCastOn->GetFormID() : 0));
            RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                "AIAgentAIMind", "CastConcentrationSpell", args, callback);
        }
        // Constant: cast for random 3-5 seconds
        else if (castingType == RE::MagicSystem::CastingType::kConstantEffect) {
            auto args = RE::MakeFunctionArguments(
                std::move(targetActor), 
                std::move(spellToCast->GetFormID()),
                std::move(targetToCastOn ? targetToCastOn->GetFormID() : 0));
            RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                "AIAgentAIMind", "CastConstantSpell", args, callback);
        }
        // Fire & Forget: cast once
        else {
            auto args = RE::MakeFunctionArguments(
                std::move(targetActor), 
                std::move(spellToCast->GetFormID()),
                std::move(targetToCastOn ? targetToCastOn->GetFormID() : 0));
            RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                "AIAgentAIMind", "CastSpellOnTarget", args, callback);
        }
        
        std::string targetDescription = targetToCastOn ? targetToCastOn->GetDisplayFullName() : "nearby";
        if (deliveryType == 4) {
            targetDescription = "nearby location";
        }
        
        logger::info("Casting spell '{}' from {} on {}", spellName, actorname, targetDescription);
        HTTPManager::log(std::format("infoaction|{}|{}|{} casts {} at {}", 
            getCurrentTimeMillis(), GetGameTimeStamp(),
            actorname, spellName, targetDescription),
            targetActor);

    } else if (command.rfind("ExtCmd", 0) == 0) {
        responsePop("command");
        auto npc = agentPtr->getActorName();
        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
        std::string localCommand = command;
        std::string localParameter = parameter;
        bool dispatched = false;

        auto separatorPos = localCommand.find('_', 6);
        if (separatorPos != std::string::npos && separatorPos > 6) {
            std::string bridgeScript = localCommand.substr(6, separatorPos - 6);
            auto bridgeArgs = RE::MakeFunctionArguments(std::string(npc), std::string(localCommand), std::string(localParameter));
            dispatched = RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                bridgeScript,
                "DispatchExternalCommand",
                bridgeArgs,
                callback);
        }

        if (!dispatched) {
            auto fallbackArgs = RE::MakeFunctionArguments(std::move(npc), std::move(localCommand), std::move(localParameter));
            RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                "AIAgentAIMind",
                "SendExternalEvent",
                fallbackArgs,
                callback);
        }

    } else if (command.rfind("IntCmd", 0) == 0) {
        responsePop("command");
        auto npc = agentPtr->getActorName();
        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
        std::string localParameter = trim(parameter);
        auto args = RE::MakeFunctionArguments(std::move(npc), std::move(command), std::move(localParameter));

        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentAIMind", "SendInternalEvent",
                                                                                   args, callback);

    } else if (command.rfind("WebCmd", 0) == 0) {
        responsePop("command");

        HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                     "command@" + command + "@" + trim(parameter) + "@"));

    } else if (command.contains("TakeGoldFromPlayer")) {
        responsePop("command");

        auto npc = agentPtr->getActor();
        parameter = removeTextAfterLastQuote(parameter);

        int amount = atoi(parameter.c_str());
        if (amount == 0) amount = 1;
        std::string amt = std::to_string(amount);

        HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                     "command@" + command + "@" + amt + "@"),
                         npc);

        auto player = RE::PlayerCharacter::GetSingleton()->As<RE::Actor>();
        auto form = RE::TESForm::LookupByID(0x0f);
        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
        std::string itemname("Gold");
        auto args = RE::MakeFunctionArguments(std::move(player), std::move(npc), std::move(form), std::move(amount),
                                              std::move(itemname));

        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentAIMind", "MoveInventoryItem",
                                                                                   args, callback);

    } else if (command.contains("RentRoom")) {
        responsePop("command");

        auto npc = agentPtr->getActor();
        int amount = parseRentRoomCost(parameter);
        std::string amt = std::to_string(amount);

        HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                     "command@" + command + "@" + amt + "@"),
                        npc);

        auto player = RE::PlayerCharacter::GetSingleton()->As<RE::Actor>();
        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
        auto args = RE::MakeFunctionArguments(std::move(player), std::move(npc), std::move(amount));

        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentAIMind", "RentRoom", args,
                                                                                   callback);

    } else if (command.contains("HireCarriage")) {
        responsePop("command");

        auto npc = agentPtr->getActor();
        auto player = RE::PlayerCharacter::GetSingleton()->As<RE::Actor>();
        std::string destinationName = parseStructuredActionTarget(parameter);

        if (!npc || !player) {
            logger::info("[HireCarriage] Missing npc or player actor");
            return;
        }

        if (destinationName.empty()) {
            HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                         "command@" + command + "@@Error: missing destination"),
                             npc);
            return;
        }

        if (GetPlayerLocation() == destinationName) {
            HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                         "command@" + command + "@" + destinationName + "@Error, you're already there"),
                             npc);
            return;
        }

        auto locationForm = findLocation(destinationName);
        if (!locationForm) {
            HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                         "command@" + command + "@" + destinationName +
                                             "@Error: destination not known"),
                             npc);
            return;
        }

        auto destinationMarker = resolveLocationMarker(locationForm);
        if (!destinationMarker) {
            HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                         "command@" + command + "@" + destinationName +
                                             "@Error: destination marker not found"),
                             npc);
            return;
        }

        int fare = parseStructuredActionAmount(parameter, getCarriageFare(destinationName), true);
        std::string fareString = std::to_string(fare);

        HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                     "command@" + command + "@" + destinationName + "@" + fareString),
                         npc);

        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
        auto args = RE::MakeFunctionArguments(std::move(player), std::move(npc), std::move(destinationMarker),
                                              std::move(destinationName), std::move(fare));
        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentAIMind", "HireCarriage",
                                                                                   args, callback);

    } else if (command.contains("HireFerry")) {
        responsePop("command");

        auto npc = agentPtr->getActor();
        auto player = RE::PlayerCharacter::GetSingleton()->As<RE::Actor>();
        std::string destinationName = parseStructuredActionTarget(parameter);

        if (!npc || !player) {
            logger::info("[HireFerry] Missing npc or player actor");
            return;
        }

        if (destinationName.empty()) {
            HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                         "command@" + command + "@@Error: missing destination"),
                             npc);
            return;
        }

        if (GetPlayerLocation() == destinationName) {
            HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                         "command@" + command + "@" + destinationName + "@Error, you're already there"),
                             npc);
            return;
        }

        auto locationForm = findLocation(destinationName);
        if (!locationForm) {
            HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                         "command@" + command + "@" + destinationName +
                                             "@Error: destination not known"),
                             npc);
            return;
        }

        auto destinationMarker = resolveLocationMarker(locationForm);
        if (!destinationMarker) {
            HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                         "command@" + command + "@" + destinationName +
                                             "@Error: destination marker not found"),
                             npc);
            return;
        }

        int fare = parseStructuredActionAmount(parameter, getFerryFare(destinationName), true);
        std::string fareString = std::to_string(fare);

        HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                     "command@" + command + "@" + destinationName + "@" + fareString),
                         npc);

        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
        auto args = RE::MakeFunctionArguments(std::move(player), std::move(npc), std::move(destinationMarker),
                                              std::move(destinationName), std::move(fare));
        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentAIMind", "HireFerry", args,
                                                                                   callback);

    } else if (command.contains("FollowPlayer")) {
        responsePop("command");

        auto npc = agentPtr->getActor();
        parameter = removeTextAfterLastQuote(parameter);

        int amount = atoi(parameter.c_str());

        HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                     "command@" + command + "@" + trim(parameter) + "@"),
                         npc);

        std::string taskid;
        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
        auto args = RE::MakeFunctionArguments(std::move(npc), std::move(1), std::move(taskid));

        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentAIMind", "stayAtPlace", args,
                                                                                   callback);

    } else if (command.contains("MakeFollower")) {
        responsePop("command");
        auto npc = agentPtr->getActor();
        if (npc) {
            auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
            auto args = RE::MakeFunctionArguments(std::move(npc));
            auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            logger::info("[MakeFollower] About to dispatch Papyrus call!");

            if (!vm) {
                logger::error("[MakeFollower] Failed to get VirtualMachine!");
                return;
            }

            bool dispatchResult = vm->DispatchStaticCall("AIAgentAIMind", "MakeFollower", args, callback);
            if (!dispatchResult) {
                logger::error("[MakeFollower] Failed to dispatch Papyrus call!");
            }
        }

    } else if (command.contains("Follow")) {
        responsePop("command");

        auto npc = agentPtr->getActor();
        parameter = removeTextAfterLastQuote(parameter);

        auto target = findActorInCell(trim(parameter), npc->GetParentCell(), npc, 1024, false);
        if (target) {
            HTTPManager::log(std::format("infoaction|{}|{}|{} follows {}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                         agentPtr->getActorName(), parameter));

            std::string taskid;
            auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
            auto args = RE::MakeFunctionArguments(std::move(npc), std::move(target));

            RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentAIMind", "Follow", args,
                                                                                       callback);
        }

    } else if (command.contains("ComeCloser")) {
        responsePop("command");
        auto npc = agentPtr->getActor();
        auto player = RE::PlayerCharacter::GetSingleton()->AsReference();

        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
        auto args = RE::MakeFunctionArguments(std::move(npc), std::move(player));

        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentAIMind", "FollowSoft", args,
                                                                                   callback);

    } else if (command.contains("EndConversation")) {
        responsePop("command");
        auto npc = agentPtr->getActor();
        
        // Set cooldown on agent
        agentPtr->setConversationEnded();
        logger::info("{} conversation ended, cooldown started", agentPtr->getActorName());
        
        HTTPManager::log(std::format("infoaction|{}|{}|{} leaves the conversation", getCurrentTimeMillis(), GetGameTimeStamp(),
                                     agentPtr->getActorName()));

        // Call Papyrus to clean up packages and timers
        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
        auto args = RE::MakeFunctionArguments(std::move(npc));

        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentAIMind", "EndConversation", args,
                                                                                   callback);

    } else if (command.contains("ReturnBackHome")) {
        responsePop("command");
        auto npc = agentPtr->getActor();
        if (agentPtr->getActor()) {
            auto targetActor = agentPtr->getActor();

            auto markerForm = RE::TESForm::LookupByID(0x000e0f69);  // Pick nowhere place here // Serpent Stone
            if (markerForm) {
                auto marker = markerForm->As<RE::TESObjectREFR>();

                logger::info("Location target: {}", marker->GetName());

                HTTPManager::log(std::format("infoaction|{}|{}|{} leaves the place", getCurrentTimeMillis(),
                                             GetGameTimeStamp(), targetActor->GetDisplayFullName()));

                std::string destName("");
                auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();

                auto args = RE::MakeFunctionArguments(std::move(targetActor), std::move(marker), std::move(destName));
                RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                    "AIAgentAIMind", "TravelToTarget", args, callback);

                agentPtr.get()->setCurrentCommand("TravelTo");
                // agentPtr.get()->setCommandBusy(true);
            }
        }

    } else if (command.contains("GiveGoldTo")) {
        responsePop("command");
        auto npc = agentPtr->getActor();
        if (npc) {
            // Parse JSON parameters: {"target":"NpcName", "amount":100}
            std::string targetName;
            int goldAmount = 0;
            
            // Try to parse as JSON first
            bool isJson = false;
            if (parameter.find('{') != std::string::npos) {
                try {
                    auto jsonStart = parameter.find('{');
                    auto jsonEnd = parameter.rfind('}');
                    if (jsonEnd != std::string::npos) {
                        std::string jsonStr = parameter.substr(jsonStart, jsonEnd - jsonStart + 1);
                        size_t targetPos = jsonStr.find("\"target\"");
                        // Check for "item" first (new format), then fall back to "amount" (legacy)
                        size_t itemPos = jsonStr.find("\"item\"");
                        size_t amountPos = jsonStr.find("\"amount\"");
                        
                        if (targetPos != std::string::npos) {
                            size_t valueStart = jsonStr.find(':', targetPos) + 1;
                            size_t quoteStart = jsonStr.find('"', valueStart) + 1;
                            size_t quoteEnd = jsonStr.find('"', quoteStart);
                            if (quoteEnd != std::string::npos) {
                                targetName = jsonStr.substr(quoteStart, quoteEnd - quoteStart);
                            }
                        }
                        
                        // Try "item" first (new format from server)
                        if (itemPos != std::string::npos) {
                            size_t valueStart = jsonStr.find(':', itemPos) + 1;
                            size_t quoteStart = jsonStr.find('"', valueStart);
                            size_t digitStart = jsonStr.find_first_of("0123456789", valueStart);
                            if (digitStart != std::string::npos) {
                                try {
                                    goldAmount = std::stoi(jsonStr.substr(digitStart));
                                    isJson = true;
                                } catch (...) {
                                    goldAmount = 0;
                                }
                            }
                        }
                        // Fall back to "amount" for backwards compatibility
                        else if (amountPos != std::string::npos) {
                            size_t valueStart = jsonStr.find(':', amountPos) + 1;
                            size_t digitStart = jsonStr.find_first_of("0123456789", valueStart);
                            if (digitStart != std::string::npos) {
                                try {
                                    goldAmount = std::stoi(jsonStr.substr(digitStart));
                                    isJson = true;
                                } catch (...) {
                                    goldAmount = 0;
                                }
                            }
                        }
                    }
                } catch (...) {
                    logger::warn("[GiveGoldTo] JSON parsing failed, falling back to legacy");
                }
            }
            
            // Legacy fallback: parameter is just the target name
            if (!isJson) {
                targetName = trim(parameter);
                goldAmount = 10; // Default amount
            }
            
            if (goldAmount <= 0) {
                logger::info("[GiveGoldTo] Invalid amount: {}", goldAmount);
                HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                             "command@GiveGoldTo@" + targetName + "@Error: invalid gold amount"),
                                 npc);
                return;
            }

            auto playerActor = RE::PlayerCharacter::GetSingleton()->As<RE::Actor>();
            auto normalizeActorName = [](std::string value) {
                value = trim(value);
                std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                return value;
            };

            const std::string normalizedTargetName = normalizeActorName(targetName);
            bool targetIsPlayer = normalizedTargetName == "player";
            if (!targetIsPlayer && playerActor) {
                const std::string playerDisplayName = normalizeActorName(playerActor->GetDisplayFullName());
                const std::string playerName = normalizeActorName(playerActor->GetName());
                targetIsPlayer = (!playerDisplayName.empty() && normalizedTargetName == playerDisplayName) ||
                                 (!playerName.empty() && normalizedTargetName == playerName);
            }

            RE::TESObjectREFR* target = nullptr;
            RE::Actor* targetAsActor = nullptr;
            if (targetIsPlayer) {
                targetAsActor = playerActor;
                target = playerActor;
            } else {
                target = findActorInCell(trim(targetName), npc->GetParentCell(), npc, 2048, false);
                targetAsActor = target ? target->As<RE::Actor>() : nullptr;
            }

            if (!target || !targetAsActor) {
                logger::info("[COMMAND] GiveGoldTo target {} not found", targetName);
                HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                             "command@GiveGoldTo@" + targetName + "@Error: target not found"),
                                 npc);
                return;
            }

            std::string resolvedTargetName(targetAsActor->GetDisplayFullName());
            if (resolvedTargetName.empty()) resolvedTargetName = targetName;
            
            // Get gold form and call Papyrus function
            auto goldForm = RE::TESForm::LookupByID(0x0f);
            if (!goldForm) {
                logger::error("[GiveGoldTo] Could not find gold form!");
                return;
            }
            
            logger::info("[GiveGoldTo] Calling transfer: {} -> {}, Gold, Amount={}", 
                        agentPtr->getActorName(), resolvedTargetName, goldAmount);
            
            RE::DebugNotification(
                std::format("[CHIM] {} gives {} gold to {}", agentPtr->getActorName(), goldAmount, resolvedTargetName).c_str());

            HTTPManager::log(std::format("infoaction|{}|{}|{} gives {} gold to {}", getCurrentTimeMillis(),
                                         GetGameTimeStamp(), agentPtr->getActorName(), goldAmount, resolvedTargetName),
                             npc);

            auto form = goldForm;
            std::string goldName = "Gold";
            auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
            auto args = RE::MakeFunctionArguments(std::move(npc), std::move(targetAsActor), std::move(form), 
                                                  static_cast<int>(goldAmount), std::move(goldName));
            RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                "AIAgentAIMind", targetIsPlayer ? "MoveInventoryItem" : "GiveItemToTarget", args, callback);
            
            // Refresh inventory after giving gold
            logger::info("[GiveGoldTo] Refreshing inventory for {} after giving gold", agentPtr->getActorName());
            RefreshAIAgentInventory(npc, agentPtr->getActorName(), true);
        }

    } else if (command.contains("TradeItems")) {
        responsePop("command");
        auto npc = agentPtr->getActor();
        if (npc) {
            auto target = findActorInCell(trim(parameter), npc->GetParentCell(), npc, 2048, false);

            RE::DebugNotification(
                std::format("[CHIM] {} trade items with {}", agentPtr->getActorName(), parameter).c_str());

            /*HTTPManager::stream(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                            "command@" + command + "@" + trim(parameter) + "@#HERIKA_NPC1# Gives Gold to
               " + parameter), npc);*/

            HTTPManager::log(std::format("infoaction|{}|{}|{} trade items with {}", getCurrentTimeMillis(),
                                         GetGameTimeStamp(), targetActor->GetDisplayFullName(), parameter),
                             targetActor);

            if (target) {
                auto targetRef = target->AsReference();
                std::string destinationName(parameter);
                int intent = 2;
                auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                auto args = RE::MakeFunctionArguments(std::move(targetActor), std::move(targetRef), std::move(intent));
                RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                    "AIAgentAIMind", "MoveToTarget", args, callback);
            }
        }

    } else if (command.contains("Consume")) {
        responsePop("command");
        auto npc = agentPtr->getActor();
        if (npc) {
            auto normalizeConsumeItemName = [](std::string value) {
                value = trim(value);
                std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });

                const std::string separator = " - ";
                auto separatorPos = value.find(separator);
                if (separatorPos != std::string::npos) {
                    std::string prefix = value.substr(0, separatorPos);
                    if (prefix.find("food") != std::string::npos ||
                        prefix.find("drink") != std::string::npos ||
                        prefix.find("potion") != std::string::npos ||
                        prefix.find("poison") != std::string::npos) {
                        value = value.substr(separatorPos + separator.length());
                    }
                }

                std::string normalized;
                normalized.reserve(value.size());
                bool lastWasSpace = false;
                for (char c : value) {
                    if (std::isalnum(static_cast<unsigned char>(c))) {
                        normalized.push_back(c);
                        lastWasSpace = false;
                    } else if (!lastWasSpace) {
                        normalized.push_back(' ');
                        lastWasSpace = true;
                    }
                }

                return trim(normalized);
            };

            const auto payload = parseActionParameterPayload(parameter);
            std::string requestedItem = extractStructuredActionStringField(payload, {"target", "item"});
            if (requestedItem.empty()) {
                requestedItem = trim(parameter);
            }

            if (requestedItem.empty()) {
                logger::warn("[Consume] No inventory item specified");
                HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                             "command@Consume@@Error: no inventory item specified"),
                                 npc);
                return;
            }

            RE::TESBoundObject* matchedObject = nullptr;
            RE::AlchemyItem* matchedAlchemy = nullptr;
            RE::ExtraDataList* matchedExtraList = nullptr;
            std::string matchedItemName;
            std::string rejectionReason;
            const std::string normalizedRequestedItem = normalizeConsumeItemName(requestedItem);

            struct ConsumeCandidate {
                RE::TESBoundObject* object = nullptr;
                RE::AlchemyItem* alchemy = nullptr;
                RE::ExtraDataList* extraList = nullptr;
                std::string name;
                std::string rejectionReason;
            };

            std::vector<ConsumeCandidate> exactNormalizedCandidates;
            std::vector<ConsumeCandidate> fuzzyCandidates;

            auto inventory = npc->GetInventory();
            for (const auto& invItem : inventory) {
                RE::TESBoundObject* boundObject = invItem.first;
                RE::TESObjectREFR::Count count = invItem.second.first;
                const std::unique_ptr<RE::InventoryEntryData>& entryData = invItem.second.second;

                if (!boundObject || count <= 0) {
                    continue;
                }

                std::string currentItemName(boundObject->GetName());
                if (entryData && entryData.get()) {
                    const auto displayName = entryData.get()->GetDisplayName();
                    if (displayName && *displayName) {
                        currentItemName.assign(displayName);
                    }
                }

                auto candidateObject = boundObject;
                auto candidateName = currentItemName;
                auto candidateAlchemy = boundObject->As<RE::AlchemyItem>();
                RE::ExtraDataList* candidateExtraList = nullptr;
                if (entryData && entryData->extraLists && !entryData->extraLists->empty()) {
                    candidateExtraList = entryData->extraLists->front();
                }

                auto currentRejectionReason = std::string();
                if (!candidateAlchemy) {
                    currentRejectionReason = "it is not a consumable item";
                } else if (candidateAlchemy->IsPoison()) {
                    currentRejectionReason = "poisons cannot be consumed with this action";
                } else if (!(candidateAlchemy->IsFood() || candidateAlchemy->IsMedicine())) {
                    currentRejectionReason = "it is not a food, drink, or potion";
                }

                const std::string normalizedCurrentItem = normalizeConsumeItemName(currentItemName);
                const bool exactDisplayMatch = equalsCaseInsensitive(currentItemName, requestedItem);
                const bool exactNormalizedMatch =
                    !normalizedRequestedItem.empty() && normalizedCurrentItem == normalizedRequestedItem;
                const bool fuzzyMatch =
                    !normalizedRequestedItem.empty() &&
                    !normalizedCurrentItem.empty() &&
                    (normalizedCurrentItem.find(normalizedRequestedItem) != std::string::npos ||
                     normalizedRequestedItem.find(normalizedCurrentItem) != std::string::npos);

                ConsumeCandidate candidate{boundObject, matchedAlchemy, matchedExtraList, currentItemName, currentRejectionReason};
                candidate.object = candidateObject;
                candidate.alchemy = candidateAlchemy;
                candidate.extraList = candidateExtraList;
                candidate.name = candidateName;
                candidate.rejectionReason = currentRejectionReason;

                if (exactDisplayMatch) {
                    matchedObject = candidate.object;
                    matchedAlchemy = candidate.alchemy;
                    matchedExtraList = candidate.extraList;
                    matchedItemName = candidate.name;
                    rejectionReason = candidate.rejectionReason;
                    break;
                }

                if (exactNormalizedMatch) {
                    exactNormalizedCandidates.push_back(candidate);
                } else if (fuzzyMatch) {
                    fuzzyCandidates.push_back(candidate);
                }
            }

            if (!matchedObject && exactNormalizedCandidates.size() == 1) {
                const auto& candidate = exactNormalizedCandidates.front();
                matchedObject = candidate.object;
                matchedAlchemy = candidate.alchemy;
                matchedExtraList = candidate.extraList;
                matchedItemName = candidate.name;
                rejectionReason = candidate.rejectionReason;
            } else if (!matchedObject && exactNormalizedCandidates.size() > 1) {
                logger::info("[Consume] Item '{}' matched multiple inventory entries for {}", requestedItem, agentPtr->getActorName());
                HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                             "command@Consume@" + requestedItem + "@Error: multiple matching items were found in inventory"),
                                 npc);
                return;
            } else if (!matchedObject && fuzzyCandidates.size() == 1) {
                const auto& candidate = fuzzyCandidates.front();
                matchedObject = candidate.object;
                matchedAlchemy = candidate.alchemy;
                matchedExtraList = candidate.extraList;
                matchedItemName = candidate.name;
                rejectionReason = candidate.rejectionReason;
            } else if (!matchedObject && fuzzyCandidates.size() > 1) {
                logger::info("[Consume] Item '{}' fuzzy-matched multiple inventory entries for {}", requestedItem, agentPtr->getActorName());
                HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                             "command@Consume@" + requestedItem + "@Error: the item name was ambiguous in inventory"),
                                 npc);
                return;
            }

            if (!matchedObject || matchedItemName.empty()) {
                logger::info("[Consume] Item '{}' not found in {}'s inventory", requestedItem, agentPtr->getActorName());
                HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                             "command@Consume@" + requestedItem + "@Error: item not in inventory"),
                                 npc);
                return;
            }

            if (!rejectionReason.empty() || !matchedAlchemy) {
                logger::info("[Consume] {} could not consume {} because {}", agentPtr->getActorName(), matchedItemName,
                             rejectionReason);
                HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                             "command@Consume@" + matchedItemName + "@Error: " + agentPtr->getActorName() +
                                                 " could not consume " + matchedItemName + " because " + rejectionReason),
                                 npc);
                return;
            }

            logger::info("[Consume] {} consuming {}", agentPtr->getActorName(), matchedItemName);
            const bool consumed = npc->DrinkPotion(matchedAlchemy, matchedExtraList);
            if (!consumed) {
                logger::warn("[Consume] Engine refused to consume {} for {}", matchedItemName, agentPtr->getActorName());
                HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                             "command@Consume@" + matchedItemName +
                                                 "@Error: the item could not be consumed right now"),
                                 npc);
                return;
            }

            const std::string resultText = agentPtr->getActorName() + " consumes " + matchedItemName + ".";
            std::string notificationText = resultText;

            auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
            auto args = RE::MakeFunctionArguments(std::move(npc), std::move(notificationText));
            RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                "AIAgentAIMind", "ConsumeItemFeedback", args, callback);

            HTTPManager::log(std::format("infoaction|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                         resultText),
                             npc);
            HTTPManager::stream(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                            "command@Consume@#HERIKA_NPC1#@" + resultText),
                                npc);

            RefreshAIAgentInventory(npc, agentPtr->getActorName(), true);
        }

    } else if (command.contains("GiveItemTo")) {
        responsePop("command");
        auto npc = agentPtr->getActor();
        if (npc) {
            // Parse JSON parameters: {"target":"NpcName", "item":"ItemName", "amount":1}
            std::string targetName;
            std::string itemName;
            int itemAmount = 1;
            
            // Try to parse as JSON first
            bool isJson = false;
            if (parameter.find('{') != std::string::npos) {
                try {
                    auto jsonStart = parameter.find('{');
                    auto jsonEnd = parameter.rfind('}');
                    if (jsonEnd != std::string::npos) {
                        std::string jsonStr = parameter.substr(jsonStart, jsonEnd - jsonStart + 1);
                        // Simple JSON parsing for our specific format
                        size_t targetPos = jsonStr.find("\"target\"");
                        size_t itemPos = jsonStr.find("\"item\"");
                        size_t amountPos = jsonStr.find("\"amount\"");
                        
                        if (targetPos != std::string::npos) {
                            size_t valueStart = jsonStr.find(':', targetPos) + 1;
                            size_t quoteStart = jsonStr.find('"', valueStart) + 1;
                            size_t quoteEnd = jsonStr.find('"', quoteStart);
                            if (quoteEnd != std::string::npos) {
                                targetName = jsonStr.substr(quoteStart, quoteEnd - quoteStart);
                            }
                        }
                        
                        if (itemPos != std::string::npos) {
                            size_t valueStart = jsonStr.find(':', itemPos) + 1;
                            size_t quoteStart = jsonStr.find('"', valueStart) + 1;
                            size_t quoteEnd = jsonStr.find('"', quoteStart);
                            if (quoteEnd != std::string::npos) {
                                itemName = jsonStr.substr(quoteStart, quoteEnd - quoteStart);
                                isJson = true;
                            }
                        }
                        
                        if (amountPos != std::string::npos) {
                            size_t valueStart = jsonStr.find(':', amountPos) + 1;
                            size_t digitStart = jsonStr.find_first_of("0123456789", valueStart);
                            if (digitStart != std::string::npos) {
                                try {
                                    itemAmount = std::stoi(jsonStr.substr(digitStart));
                                } catch (...) {
                                    itemAmount = 1;
                                }
                            }
                        }
                    }
                } catch (...) {
                    logger::warn("[GiveItemTo] JSON parsing failed, falling back to legacy");
                }
            }
            
            // Legacy fallback: parameter is just the target name
            if (!isJson) {
                targetName = trim(parameter);
                itemName = ""; // No item specified
                logger::info("[GiveItemTo] Legacy mode: target='{}', no item specified", targetName);
            }
            
            // If no item was specified, this is an error
            if (itemName.empty()) {
                logger::warn("[GiveItemTo] No item specified in command");
                HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                             "command@GiveItemTo@" + targetName + "@Error: no item specified"),
                                 npc);
                return;
            }
            
            auto playerActor = RE::PlayerCharacter::GetSingleton()->As<RE::Actor>();
            auto normalizeActorName = [](std::string value) {
                value = trim(value);
                std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                return value;
            };

            const std::string normalizedTargetName = normalizeActorName(targetName);
            bool targetIsPlayer = normalizedTargetName == "player";
            if (!targetIsPlayer && playerActor) {
                const std::string playerDisplayName = normalizeActorName(playerActor->GetDisplayFullName());
                const std::string playerName = normalizeActorName(playerActor->GetName());
                targetIsPlayer = (!playerDisplayName.empty() && normalizedTargetName == playerDisplayName) ||
                                 (!playerName.empty() && normalizedTargetName == playerName);
            }

            RE::TESObjectREFR* target = nullptr;
            RE::Actor* targetAsActor = nullptr;
            if (targetIsPlayer) {
                targetAsActor = playerActor;
                target = playerActor;
            } else {
                target = findActorInCell(trim(targetName), npc->GetParentCell(), npc, 2048, false);
                targetAsActor = target ? target->As<RE::Actor>() : nullptr;
            }

            if (!target || !targetAsActor) {
                logger::info("[COMMAND] GiveItemTo target {} not found", targetName);
                HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                             "command@GiveItemTo@" + targetName + "@Error: target not found"),
                                 npc);
                return;
            }

            std::string resolvedTargetName(targetAsActor->GetDisplayFullName());
            if (resolvedTargetName.empty()) resolvedTargetName = targetName;
            
            // Search for item in NPC's inventory and get the Form
            bool itemFound = false;
            RE::TESBoundObject* itemForm = nullptr;
            auto inventory = npc->GetInventory();
            
            for (const auto& invItem : inventory) {
                RE::TESBoundObject* boundObject = invItem.first;
                RE::TESObjectREFR::Count count = invItem.second.first;
                const std::unique_ptr<RE::InventoryEntryData>& entryData = invItem.second.second;
                
                std::string currentItemName(boundObject->GetName());
                if (entryData && entryData.get()) {
                    currentItemName.assign(entryData.get()->GetDisplayName());
                }
                
                // Check if this is the item we're looking for
                if (containsCaseInsensitive(currentItemName, itemName)) {
                    // Item matches
                    itemFound = true;
                    itemForm = boundObject;
                    itemName = currentItemName; // Use exact name
                    // Check if NPC has enough
                    if (count < itemAmount) {
                        itemAmount = count; // Give what they have
                    }
                    break;
                }
            }
            
            if (!itemFound || !itemForm || itemName.empty()) {
                logger::info("[GiveItemTo] Item {} not found in {}'s inventory", itemName, agentPtr->getActorName());
                HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                             "command@GiveItemTo@" + targetName + "@Error: item '" + itemName + "' not in inventory"),
                                 npc);
                return;
            }
            
            // Call Papyrus function to handle the transfer
            logger::info("[GiveItemTo] Calling transfer: {} -> {}, Item={}, Amount={}", 
                        agentPtr->getActorName(), resolvedTargetName, itemName, itemAmount);
            
            RE::DebugNotification(
                std::format("[CHIM] {} gives {} {} to {}", agentPtr->getActorName(), itemAmount, itemName, resolvedTargetName).c_str());
            
            HTTPManager::log(std::format("infoaction|{}|{}|{} gives {} {} to {}", getCurrentTimeMillis(),
                                         GetGameTimeStamp(), agentPtr->getActorName(), itemAmount, itemName, resolvedTargetName),
                             npc);
            
            auto form = itemForm; // Keep form
            auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
            auto args = RE::MakeFunctionArguments(std::move(npc), std::move(targetAsActor), std::move(form), 
                                                  static_cast<int>(itemAmount), std::move(itemName));
            RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                "AIAgentAIMind", targetIsPlayer ? "MoveInventoryItem" : "GiveItemToTarget", args, callback);
            
            // Refresh inventory after giving item
            logger::info("[GiveItemTo] Refreshing inventory for {} after giving item", agentPtr->getActorName());
            RefreshAIAgentInventory(npc, agentPtr->getActorName(), true);
        }

    } else if (command.contains("PickupItem")) {
        responsePop("command");
        auto npc = agentPtr->getActor();
        if (npc) {
            // Parameter format: "0x12345678:Iron Sword"
            std::string paramTrimmed = trim(parameter);
            size_t colonPos = paramTrimmed.find(':');
            
            if (colonPos == std::string::npos) {
                logger::warn("[PickupItem] Invalid parameter format: {}", paramTrimmed);
                HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                             "command@PickupItem@" + paramTrimmed + "@Error: invalid format"),
                                 npc);
                return;
            }
            
            // Extract RefID and item name
            std::string formIdStr = paramTrimmed.substr(0, colonPos);
            std::string itemName = paramTrimmed.substr(colonPos + 1);
            
            // Parse the RefID (hex string like "0xFF00550D")
            uint32_t targetFormID = 0;
            try {
                if (formIdStr.substr(0, 2) == "0x" || formIdStr.substr(0, 2) == "0X") {
                    targetFormID = std::stoul(formIdStr, nullptr, 16);
                } else {
                    targetFormID = std::stoul(formIdStr, nullptr, 10);
                }
            } catch (const std::exception& e) {
                logger::error("[PickupItem] Failed to parse FormID '{}': {}", formIdStr, e.what());
                HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                             "command@PickupItem@" + itemName + "@Error: invalid FormID"),
                                 npc);
                return;
            }
            
            // Search for the item in the NPC's current cell by FormID (most accurate)
            RE::TESObjectREFR* itemRef = nullptr;
            
            auto player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                logger::error("[PickupItem] Failed to get player!");
                return;
            }
            
            if (auto cell = npc->GetParentCell()) {
                // Use ForEachReference to find the item by its FormID
                cell->ForEachReference([&itemRef, &itemName, &player, targetFormID](RE::TESObjectREFR& object) -> RE::BSContainer::ForEachResult {
                    if (object.IsDisabled() || object.IsDeleted()) {
                        return RE::BSContainer::ForEachResult::kContinue;
                    }
                    
                    // Check if this is the exact item we're looking for by FormID
                    if (object.GetFormID() == targetFormID) {
                        float dist = player->GetPosition().GetDistance(object.GetPosition());
                        
                        // Verify it's within 512 units from player
                        if (dist < 512.0f) {
                            itemRef = &object;
                            return RE::BSContainer::ForEachResult::kStop; // Found it, stop searching
                        }
                    }
                    
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            }
            
            if (!itemRef) {
                logger::warn("[PickupItem] Item '{}' not found in cell", itemName);
                HTTPManager::log(std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                             "command@PickupItem@" + itemName + "@Error: item not found"),
                                 npc);
                return;
            }
            
            HTTPManager::log(std::format("infoaction|{}|{}|{} picks up {}", getCurrentTimeMillis(),
                                         GetGameTimeStamp(), agentPtr->getActorName(), itemName),
                             npc);
            
            // Call PickupItemFromWorld with the actual ObjectReference (much faster - no scanning in Papyrus)
            auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
            auto args = RE::MakeFunctionArguments(std::move(npc), std::move(itemRef), std::move(itemName));
            auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            
            if (!vm) {
                logger::error("[PickupItem] Failed to get VirtualMachine!");
                return;
            }
            
            bool dispatchResult = vm->DispatchStaticCall("AIAgentAIMind", "PickupItemFromWorld", args, callback);
            if (!dispatchResult) {
                logger::error("[PickupItem] Failed to dispatch Papyrus call!");
            }
        }


    } else if (command.contains("PlaySong")) {
        responsePop("command");
        auto npc = agentPtr->getActor();
        parameter = trim(parameter);
        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
        auto args = RE::MakeFunctionArguments(std::move(npc), std::move(parameter));
        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentNpcUtil", "StartMusicScene", args, callback);
        
        

    } else if (command.contains("CommandAnimation")) {
        responsePop("command");
        auto npc = agentPtr->getActor();
        parameter = trim(parameter);
        if (agentPtr->isAvailableforAnimation())
            commandAnimation(parameter, npc);

    } else {
        logger::info("Command not recognized {}", command);
        responsePop("command");
    }
}

bool PlayerIsInInterior() {
    if (auto player = RE::PlayerCharacter::GetSingleton()) {
        if (auto parentCell = player->parentCell) {
            return parentCell->IsInteriorCell();
        }
    }
    return false;
}

std::string InspectLocations(RE::TESObjectREFR* reference) {
    std::string buffer;

    for (const auto& entry : LocationList::GetInstance()) {
        const std::string& name = entry.first;
        RE::TESObjectREFR* location = entry.second;
        if (location->GetPosition().GetDistance(reference->GetPosition()) < 10000) buffer.append(entry.first + ",");
    }
    if (buffer.empty()) buffer.assign("none");
    return buffer;
}

std::string InspectSurroundings(RE::TESObjectREFR* reference, bool useCache, float visionRange, std::string separator,
                                 float farAwayLimit) {
    std::vector<std::string> results;

    // logger::info("InspectSurroundings: running");
    auto player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        logger::error("InspectSurroundings: PlayerCharacter is null.");
        return "";
    }

    auto cell = player->GetParentCell();
    if (!cell) {
        logger::warn("InspectSurroundings: Player's parent cell is null.");
        return "";
    }

    auto processLists = RE::ProcessLists::GetSingleton();
    if (!processLists) {
        logger::warn("InspectSurroundings: ProcessLists is null.");
        return "";
    }

    for (auto& targetHandle : processLists->highActorHandles) {
        if (!targetHandle || !targetHandle.get()) {
            continue;  // Skip invalid handles
        }

        if (!targetHandle.get()) {
            logger::warn("InspectSurroundings: Target handle is null.");
            continue;
        }

        targetHandle.get()->IncRefCount();  // Increment reference count to ensure the actor is valid during processing
        auto target = targetHandle.get().get();
        if (!target) {
            logger::warn("InspectSurroundings: Target actor is null.");
            targetHandle.get()->DecRefCount();
            continue;
        }

        // Check for valid process
        if (!target->GetActorRuntimeData().currentProcess) {
            logger::warn("InspectSurroundings: Target actor runtime data is null.");
            targetHandle.get()->DecRefCount();
            continue;
        }

        std::string actorLabel;
        try {
            actorLabel = target->GetDisplayFullName();
        } catch (...) {
            logger::warn("InspectSurroundings: Failed to get display name.");
            targetHandle.get()->DecRefCount();
            continue;
        }

        if (actorLabel.empty()) {
            targetHandle.get()->DecRefCount();
            continue;
        }

        // Optional: LOS check could be added back if working
        bool hasLos = true;

        float distance = player->GetPosition().GetDistance(target->GetPosition());
        if (distance >= visionRange) {
            targetHandle.get()->DecRefCount();
            continue;
        }

        if (target->IsDead()) {
            actorLabel += " (dead)";
        } else if (target->IsHostileToActor(player)) {
            actorLabel += " (hostile)";
        } else if (target->GetCurrentScene()) {
            actorLabel += " (busy)";
        } else if (target->IsInCombat()) {
            actorLabel += " (in combat)";
        } else if (distance > farAwayLimit) {
            actorLabel += " (far away)";
        } else if (target->AsActorState()->GetLifeState() == RE::ACTOR_LIFE_STATE::kRestrained) {
            actorLabel += " (restrained)";
        } 

        results.push_back(actorLabel);
        targetHandle.get()->DecRefCount();
    }

    // Shuffle the results
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(results.begin(), results.end(), g);

    // Join using separator
    std::string buffer;
    for (size_t i = 0; i < results.size(); ++i) {
        buffer += results[i];
        if (i < results.size() ) {
            buffer += separator;
        }
    }

    return buffer;
}

std::string InspectManagedAgents(RE::TESObjectREFR* reference, float visionRange, const std::string& separator,
                                 float farAwayLimit, bool includeNarrator) {
    struct ManagedAgentContext {
        float distance = 0.0f;
        std::string label;
    };

    std::vector<ManagedAgentContext> results;

    RE::Actor* source = reference ? reference->As<RE::Actor>() : nullptr;
    if (!source) {
        source = RE::PlayerCharacter::GetSingleton();
    }
    if (!source) {
        logger::error("InspectManagedAgents: source actor is null.");
        return "";
    }

    auto* sourceCell = source->GetParentCell();
    if (!sourceCell || !sourceCell->IsAttached()) {
        logger::warn("InspectManagedAgents: source cell is unavailable or detached.");
        return "";
    }

    const bool sourceInterior = sourceCell->IsInteriorCell();
    const auto sourcePosition = source->GetPosition();
    AIAgentManager& aiam = AIAgentManager::getInstance();

    for (const auto& agent : aiam.getAgents()) {
        if (!agent) {
            continue;
        }

        if (agent->isNarrator() && !includeNarrator) {
            continue;
        }

        auto* target = agent->getActor();
        if (!target) {
            target = agent->getActorByFormId();
        }
        if (!target || target->GetFormID() == source->GetFormID()) {
            continue;
        }

        if (target->IsDeleted() || target->IsDisabled()) {
            continue;
        }

        if (!target->GetActorRuntimeData().currentProcess) {
            continue;
        }

        auto* targetCell = target->GetParentCell();
        if (!targetCell || !targetCell->IsAttached()) {
            continue;
        }

        const bool targetInterior = targetCell->IsInteriorCell();
        if (sourceInterior != targetInterior) {
            continue;
        }
        if (sourceInterior && targetCell != sourceCell) {
            continue;
        }

        const float distance = sourcePosition.GetDistance(target->GetPosition());
        if (!std::isfinite(distance) || distance >= visionRange) {
            continue;
        }

        std::string actorLabel = agent->getActorName();
        if (actorLabel.empty()) {
            try {
                actorLabel = target->GetDisplayFullName();
            } catch (...) {
                actorLabel.clear();
            }
        }
        if (actorLabel.empty()) {
            continue;
        }

        if (target->IsDead()) {
            actorLabel += " (dead)";
        } else if (target->IsHostileToActor(source)) {
            actorLabel += " (hostile)";
        } else if (target->GetCurrentScene()) {
            actorLabel += " (busy)";
        } else if (target->IsInCombat()) {
            actorLabel += " (in combat)";
        } else if (distance > farAwayLimit) {
            actorLabel += " (far away)";
        } else if (target->AsActorState()->GetLifeState() == RE::ACTOR_LIFE_STATE::kRestrained) {
            actorLabel += " (restrained)";
        }

        results.push_back({distance, std::move(actorLabel)});
    }

    std::sort(results.begin(), results.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.distance == rhs.distance) {
            return lhs.label < rhs.label;
        }
        return lhs.distance < rhs.distance;
    });

    std::string buffer;
    for (size_t i = 0; i < results.size(); ++i) {
        buffer += results[i].label;
        if (i + 1 < results.size()) {
            buffer += separator;
        }
    }

    return buffer;
}

SpatialAwareness::Settings GetPlayerSpeechSpatialSettings(RE::Actor* speaker, float visionRange) {
    SpatialAwareness::Settings spatialSettings = SpatialAwareness::GetSettings();
    if (visionRange > 0.0f) {
        spatialSettings.maxAirDistance = visionRange;
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (speaker && player && speaker->GetFormID() == player->GetFormID()) {
        const float distanceMultiplier = PrismaUIBridge::GetPlayerSpeechDistanceMultiplier();
        spatialSettings.maxAirDistance *= distanceMultiplier;
        spatialSettings.interiorMaxDistance *= distanceMultiplier;
        spatialSettings.exteriorMaxDistance *= distanceMultiplier;
        spatialSettings.immediateDistance = SpatialAwareness::kPlayerAutoIncludeDistance;
    }

    return spatialSettings;
}

namespace {
struct AudibleActorsCacheEntry {
    RE::FormID speakerFormId = 0;
    RE::FormID speakerCellFormId = 0;
    float visionRange = 0.0f;
    float distanceMultiplier = 1.0f;
    RE::NiPoint3 speakerPosition{};
    std::chrono::steady_clock::time_point timestamp{};
    std::vector<AudibleActorDescriptor> actors;
};

struct AudibleActorCandidate {
    RE::Actor* actor = nullptr;
    float airDistance = 0.0f;
};

std::mutex g_audibleActorsCacheMutex;
AudibleActorsCacheEntry g_audibleActorsCache;
constexpr auto kAudibleActorsCacheTtl = std::chrono::milliseconds(2500);
constexpr float kAudibleActorsCacheMoveTolerance = 64.0f;
constexpr std::size_t kMaxSpatialEvaluationsPerAudibleScan = 6;

float GetAudibleActorsDistanceMultiplier(RE::Actor* speaker)
{
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (speaker && player && speaker->GetFormID() == player->GetFormID()) {
        return PrismaUIBridge::GetPlayerSpeechDistanceMultiplier();
    }

    return 1.0f;
}

bool CanReuseAudibleActorsCache(RE::Actor* speaker, RE::TESObjectCELL* speakerCell, float visionRange,
                                float distanceMultiplier, const RE::NiPoint3& speakerPosition,
                                const std::chrono::steady_clock::time_point now)
{
    if (!speaker || !speakerCell) {
        return false;
    }

    if (g_audibleActorsCache.speakerFormId != speaker->GetFormID()) {
        return false;
    }

    if (g_audibleActorsCache.speakerCellFormId != speakerCell->GetFormID()) {
        return false;
    }

    if (std::abs(g_audibleActorsCache.visionRange - visionRange) > 0.001f) {
        return false;
    }

    if (std::abs(g_audibleActorsCache.distanceMultiplier - distanceMultiplier) > 0.001f) {
        return false;
    }

    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_audibleActorsCache.timestamp) >
        kAudibleActorsCacheTtl) {
        return false;
    }

    return g_audibleActorsCache.speakerPosition.GetDistance(speakerPosition) <= kAudibleActorsCacheMoveTolerance;
}

std::vector<AudibleActorDescriptor> CollectAudibleActorsUncached(RE::Actor* speaker, float visionRange)
{
    std::vector<AudibleActorDescriptor> results;
    if (!speaker) {
        return results;
    }

    auto* speakerCell = speaker->GetParentCell();
    if (!speakerCell || !speakerCell->IsAttached()) {
        logger::warn("CollectAudibleActors: speaker cell is unavailable or detached.");
        return results;
    }

    const SpatialAwareness::Settings spatialSettings = GetPlayerSpeechSpatialSettings(speaker, visionRange);
    const bool speakerInterior = speakerCell->IsInteriorCell();
    const RE::NiPoint3 speakerPosition = speaker->GetPosition();
    std::vector<AudibleActorCandidate> candidates;
    candidates.reserve(32);

    AIAgentManager& aiam = AIAgentManager::getInstance();
    for (const auto& agent : aiam.getAgents()) {
        if (!agent || agent->isNarrator()) {
            continue;
        }

        auto* target = agent->getActor();
        if (!target) {
            auto* targetForm = RE::TESForm::LookupByID(agent->GetFormId());
            target = targetForm ? targetForm->As<RE::Actor>() : nullptr;
        }

        if (!target) {
            continue;
        }

        if (speaker->GetFormID() == target->GetFormID()) {
            continue;
        }

        if (!target->GetActorRuntimeData().currentProcess || !target->Is3DLoaded()) {
            continue;
        }

        auto* targetCell = target->GetParentCell();
        if (!targetCell || !targetCell->IsAttached()) {
            continue;
        }

        const bool targetInterior = targetCell->IsInteriorCell();
        if (speakerInterior != targetInterior) {
            continue;
        }

        if (speakerInterior && targetCell != speakerCell) {
            continue;
        }

        const float airDistance = speakerPosition.GetDistance(target->GetPosition());
        if (spatialSettings.maxAirDistance > 0.0f && airDistance >= spatialSettings.maxAirDistance) {
            continue;
        }

        candidates.push_back(AudibleActorCandidate{target, airDistance});
    }

    std::sort(candidates.begin(), candidates.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.airDistance < rhs.airDistance;
    });

    std::size_t spatialEvaluations = 0;
    std::size_t budgetSkipped = 0;

    for (auto& candidate : candidates) {
        auto* target = candidate.actor;
        if (!target) {
            continue;
        }

        const auto releaseTarget = [&]() {
            target = nullptr;
        };

        if (spatialEvaluations >= kMaxSpatialEvaluationsPerAudibleScan) {
            ++budgetSkipped;
            releaseTarget();
            continue;
        }

        ++spatialEvaluations;

        SpatialAwareness::Result spatialResult{};
        try {
            spatialResult = SpatialAwareness::Evaluate(speaker, target, spatialSettings);
        } catch (...) {
            logger::warn("CollectAudibleActors: spatial evaluation failed for {:08X}", target->GetFormID());
            releaseTarget();
            continue;
        }

        if (!spatialResult.canCommunicate) {
            releaseTarget();
            continue;
        }

        std::string actorLabel;
        try {
            actorLabel = target->GetDisplayFullName();
        } catch (...) {
            releaseTarget();
            continue;
        }

        if (actorLabel.empty()) {
            releaseTarget();
            continue;
        }

        AudibleActorDescriptor audibleActor{};
        audibleActor.formId = target->GetFormID();
        audibleActor.label = std::move(actorLabel);
        audibleActor.airDistance = candidate.airDistance;
        audibleActor.volume = spatialResult.volume;
        audibleActor.pathDistance = spatialResult.pathDistance;
        audibleActor.pathRatio = spatialResult.pathRatio;
        audibleActor.reason = spatialResult.reason;
        audibleActor.openDoorCount = spatialResult.openDoorCount;
        audibleActor.closedDoorCount = spatialResult.closedDoorCount;
        audibleActor.navmeshPathUsed = spatialResult.navmeshPathUsed;
        audibleActor.navmeshPathFound = spatialResult.navmeshPathFound;
        audibleActor.losFallbackUsed = spatialResult.losFallbackUsed;
        audibleActor.hasLineOfSight = spatialResult.hasLineOfSight;
        audibleActor.hostile = target->IsHostileToActor(speaker);
        audibleActor.busy = !audibleActor.hostile && target->GetCurrentScene();
        audibleActor.inCombat = !audibleActor.hostile && !audibleActor.busy && target->IsInCombat();
        audibleActor.restrained = !audibleActor.hostile && !audibleActor.busy && !audibleActor.inCombat &&
                                  target->AsActorState()->GetLifeState() == RE::ACTOR_LIFE_STATE::kRestrained;
        audibleActor.canCommunicate = true;

        results.push_back(std::move(audibleActor));
        releaseTarget();
    }

    if (budgetSkipped > 0) {
        logger::debug("CollectAudibleActors: skipped {} low-priority candidates after {} spatial evaluations",
                      budgetSkipped, spatialEvaluations);
    }

    return results;
}

}  // namespace

std::vector<AudibleActorDescriptor> CollectAudibleActors(RE::TESObjectREFR* reference, float visionRange) {
    RE::Actor* speaker = reference ? reference->As<RE::Actor>() : nullptr;
    if (!speaker) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            logger::error("CollectAudibleActors: PlayerCharacter is null.");
            return {};
        }
        speaker = player->As<RE::Actor>();
    }

    auto* speakerCell = speaker->GetParentCell();
    if (!speakerCell || !speakerCell->IsAttached()) {
        logger::warn("CollectAudibleActors: speaker cell is unavailable or detached.");
        return {};
    }

    const float distanceMultiplier = GetAudibleActorsDistanceMultiplier(speaker);
    const RE::NiPoint3 speakerPosition = speaker->GetPosition();
    const auto now = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(g_audibleActorsCacheMutex);
        if (CanReuseAudibleActorsCache(speaker, speakerCell, visionRange, distanceMultiplier, speakerPosition, now)) {
            return g_audibleActorsCache.actors;
        }
    }

    auto results = CollectAudibleActorsUncached(speaker, visionRange);

    {
        std::lock_guard<std::mutex> lock(g_audibleActorsCacheMutex);
        g_audibleActorsCache.speakerFormId = speaker->GetFormID();
        g_audibleActorsCache.speakerCellFormId = speakerCell->GetFormID();
        g_audibleActorsCache.visionRange = visionRange;
        g_audibleActorsCache.distanceMultiplier = distanceMultiplier;
        g_audibleActorsCache.speakerPosition = speakerPosition;
        g_audibleActorsCache.timestamp = now;
        g_audibleActorsCache.actors = results;
    }

    return results;
}

std::string DescribeAudibleActors(const std::vector<AudibleActorDescriptor>& audibleActors, const std::string& separator)
{
    std::vector<std::string> results;
    results.reserve(audibleActors.size());

    for (const auto& audibleActor : audibleActors) {
        std::string actorLabel = audibleActor.label;
        if (audibleActor.hostile) {
            actorLabel += " (hostile)";
        } else if (audibleActor.busy) {
            actorLabel += " (busy)";
        } else if (audibleActor.inCombat) {
            actorLabel += " (in combat)";
        } else if (audibleActor.restrained) {
            actorLabel += " (restrained)";
        }

        results.push_back(std::move(actorLabel));
    }

    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(results.begin(), results.end(), g);

    std::string buffer;
    for (size_t i = 0; i < results.size(); ++i) {
        buffer += results[i];
        if (i + 1 < results.size()) {
            buffer += separator;
        }
    }

    return buffer;
}

std::string InspectAudibleActors(RE::TESObjectREFR* reference, bool useCache, float visionRange,
                                 std::string separator) {
    (void) useCache;
    return DescribeAudibleActors(CollectAudibleActors(reference, visionRange), separator);
}

std::string InspectSurroundingsNavmesh(RE::TESObjectREFR* reference, bool useCache, float visionRange,
                                       std::string separator) {
    (void) reference;
    (void) useCache;
    std::vector<std::string> results;

    auto player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        logger::error("InspectSurroundingsNavmesh: PlayerCharacter is null.");
        return "";
    }

    auto cell = player->GetParentCell();
    if (!cell) {
        logger::warn("InspectSurroundingsNavmesh: Player's parent cell is null.");
        return "";
    }

    auto processLists = RE::ProcessLists::GetSingleton();
    if (!processLists) {
        logger::warn("InspectSurroundingsNavmesh: ProcessLists is null.");
        return "";
    }

    SpatialAwareness::Settings spatialSettings = SpatialAwareness::GetSettings();
    if (visionRange > 0.0f) {
        spatialSettings.maxAirDistance = visionRange;
    }
    const float farAwayLimit =
        cell->IsInteriorCell() ? spatialSettings.interiorMaxDistance : spatialSettings.exteriorMaxDistance;

    for (auto& targetHandle : processLists->highActorHandles) {
        if (!targetHandle || !targetHandle.get()) {
            continue;
        }

        targetHandle.get()->IncRefCount();
        auto target = targetHandle.get().get();
        if (!target) {
            targetHandle.get()->DecRefCount();
            continue;
        }

        if (!target->GetActorRuntimeData().currentProcess) {
            targetHandle.get()->DecRefCount();
            continue;
        }

        std::string actorLabel;
        try {
            actorLabel = target->GetDisplayFullName();
        } catch (...) {
            targetHandle.get()->DecRefCount();
            continue;
        }

        if (actorLabel.empty()) {
            targetHandle.get()->DecRefCount();
            continue;
        }

        const auto navPath = SpatialAwareness::EvaluatePath(player, target, spatialSettings);
        if (navPath.status != SpatialAwareness::PathStatus::kSuccess || navPath.pathDistance < 0.0f) {
            targetHandle.get()->DecRefCount();
            continue;
        }

        if (target->IsDead()) {
            actorLabel += " (dead)";
        } else if (target->IsHostileToActor(player)) {
            actorLabel += " (hostile)";
        } else if (target->GetCurrentScene()) {
            actorLabel += " (busy)";
        } else if (target->IsInCombat()) {
            actorLabel += " (in combat)";
        } else if (navPath.pathDistance > farAwayLimit) {
            actorLabel += " (far away)";
        } else if (target->AsActorState()->GetLifeState() == RE::ACTOR_LIFE_STATE::kRestrained) {
            actorLabel += " (restrained)";
        }

        results.push_back(actorLabel);
        targetHandle.get()->DecRefCount();
    }

    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(results.begin(), results.end(), g);

    std::string buffer;
    for (size_t i = 0; i < results.size(); ++i) {
        buffer += results[i];
        if (i < results.size()) {
            buffer += separator;
        }
    }

    return buffer;
}

std::string InspectSurroundingsOld(RE::TESObjectREFR* reference, bool useCache, float visionRange,
                                   std::string separator) {
    std::string buffer;
    auto cell = RE::PlayerCharacter::GetSingleton()->GetParentCell();

    // Actors lurking around
    if (const auto processLists = RE::ProcessLists::GetSingleton(); processLists) {
        for (auto& targetHandle : processLists->highActorHandles) {
            if (auto target = targetHandle.get(); target && target->GetActorRuntimeData().currentProcess) {
                auto player = RE::PlayerCharacter::GetSingleton();
                bool hasLos = true;
                // player->HasLineOfSight(targetHandle.get().get()->AsReference(), hasLos);
                // std::string actorLabel(target->GetName());
                std::string actorLabel(target->GetDisplayFullName());
                if (hasLos && !actorLabel.empty()) {
                    float distance = player->GetPosition().GetDistance(target->GetPosition());

                    if (distance < visionRange) {
                        auto actor = targetHandle.get().get();

                        if (actor->IsDead())
                            actorLabel.append(" (dead)");
                        else if (actor->IsHostileToActor(player))
                            actorLabel.append(" (hostile)");
                        else if (actor->GetCurrentScene())
                            actorLabel.append(" (busy)");
                        else if (actor->IsInCombat())
                            actorLabel.append(" (in combat)");
                        else if (distance > 2048)
                            actorLabel.append(" (far away)");

                        /* if (buffer.empty())
                            buffer.append("This beings are currently visible:");*/ // redundant?
                        buffer.append(actorLabel + separator);
                    }
                    // return RE::BSContainer::ForEachResult::kStop;
                }
            }
        }
    }
    // logger::debug("InspectSurroundings {} , range: {} ", buffer,visionRange);
    return buffer;
}

std::string InspectSurroundingsInCell(RE::TESObjectCELL* cell, bool useCache) {
    std::string buffer;

    if (cell) {
        cell->ForEachReference([&buffer](RE::TESObjectREFR& object) -> RE::BSContainer::ForEachResult {
            RE::TESForm* baseForm = object.GetBaseObject();

            // logger::info("Loaded {},{},{}",object.GetName(),baseForm->GetName(),baseForm->GetFormEditorID());
            if (baseForm->formType == RE::FormType::NPC) {
                const char* currentNPC = object.GetName();
                // logger::info("{}", currentNPC);
                RE::Actor* actorNpc = object.As<RE::Actor>();
                if (actorNpc) {
                    std::string actorLabel(actorNpc->GetName());
                    if (actorNpc->IsDisabled()) return RE::BSContainer::ForEachResult::kContinue;

                    bool hasLos = false;
                    RE::PlayerCharacter::GetSingleton()->HasLineOfSight(actorNpc->AsReference(), hasLos);
                    if (hasLos && !actorLabel.empty()) {
                        if (actorNpc->IsDead()) actorLabel.append("(dead)");

                        buffer.append(actorLabel + ",");
                    }
                    // logger::info("Actor {} ", actorLabel);
                } else {
                    std::string actorLabel(object.GetName());

                    bool hasLos = false;
                    RE::PlayerCharacter::GetSingleton()->HasLineOfSight(actorNpc->AsReference(), hasLos);
                    if (hasLos && !actorLabel.empty()) {
                        buffer.append(actorLabel + ",");
                        // return RE::BSContainer::ForEachResult::kStop;
                    }

                    // logger::info("NPC {} ", actorLabel);
                }
            } /* else if (baseForm->formType == RE::FormType::Furniture) {
                auto furniture = std::string(object.GetName());
                if (!furniture.empty()) buffer.append(furniture + ",");
            }*/
            return RE::BSContainer::ForEachResult::kContinue;
        });
    }

    return buffer;
}

bool WouldBeStealing(RE::TESObjectREFR* itemRef, RE::Actor* player) {
    if (!itemRef || !player) return false;
    
    // Check direct ownership (NPC or Faction)
    auto owner = itemRef->GetOwner();
    if (owner) {
        // Item has an owner - check if player is allowed to take it
        auto ownerActor = owner->As<RE::TESNPC>();
        auto ownerFaction = owner->As<RE::TESFaction>();
        
        if (ownerActor) {
            // Check if owned by player
            auto playerBase = player->GetActorBase();
            if (playerBase && ownerActor->GetFormID() == playerBase->GetFormID()) {
                return false; // Player owns it
            }
            return true; // Owned by someone else
        }
        
        if (ownerFaction) {
            // Check if player is in the faction using IsInFaction
            if (player->IsInFaction(ownerFaction)) {
                return false; // Player is in faction, has permission
            }
            return true; // Player not in faction
        }
    }
    
    // Check cell/location ownership
    auto cell = itemRef->GetParentCell();
    if (cell) {
        auto cellOwner = cell->GetOwner();
        if (cellOwner) {
            auto cellOwnerActor = cellOwner->As<RE::TESNPC>();
            auto cellOwnerFaction = cellOwner->As<RE::TESFaction>();
            
            if (cellOwnerActor) {
                auto playerBase = player->GetActorBase();
                if (!playerBase || cellOwnerActor->GetFormID() != playerBase->GetFormID()) {
                    return true; // Cell owned by someone else
                }
            }
            
            if (cellOwnerFaction) {
                // Check if player is in the cell's faction
                if (!player->IsInFaction(cellOwnerFaction)) {
                    return true; // Player doesn't have permission in this cell
                }
            }
        }
    }
    
    return false; // No ownership detected, safe to take
}

std::string InspectNearbyItems(RE::TESObjectREFR* reference, float visionRange) {
    std::vector<std::string> results;
    
    auto player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        logger::error("[InspectNearbyItems] PlayerCharacter is null!");
        return "";
    }
    
    auto cell = player->GetParentCell();
    if (!cell) {
        logger::warn("[InspectNearbyItems] Player's parent cell is null!");
        return "";
    }
    
    // Get what the player is currently looking at via crosshair
    RE::TESObjectREFR* crosshairTarget = nullptr;
    auto crosshairPickData = RE::CrosshairPickData::GetSingleton();
    if (crosshairPickData && crosshairPickData->target) {
        crosshairTarget = crosshairPickData->target.get().get();
    }
    
    // Scan cell references for items
    cell->ForEachReference([&results, &player, &crosshairTarget, visionRange](RE::TESObjectREFR& object) -> RE::BSContainer::ForEachResult {
        
        RE::TESForm* baseForm = object.GetBaseObject();
        if (!baseForm) {
            return RE::BSContainer::ForEachResult::kContinue;
        }
        
        // Only consider TESBoundObjects (items that can be in inventory)
        auto boundObject = baseForm->As<RE::TESBoundObject>();
        if (!boundObject) {
            return RE::BSContainer::ForEachResult::kContinue;
        }
        
        // Exclude non-pickupable types
        auto formType = baseForm->GetFormType();
        if (formType == RE::FormType::NPC || 
            formType == RE::FormType::ActorCharacter ||
            formType == RE::FormType::Container ||
            formType == RE::FormType::Door ||
            formType == RE::FormType::Furniture ||
            formType == RE::FormType::Activator ||
            formType == RE::FormType::Static ||
            formType == RE::FormType::Tree ||
            formType == RE::FormType::Flora) {
            return RE::BSContainer::ForEachResult::kContinue;
        }
        
        // Also exclude any Actor references (including dead NPCs)
        if (object.As<RE::Actor>()) {
            return RE::BSContainer::ForEachResult::kContinue;
        }
        
        // Check if item is disabled or already taken
        if (object.IsDisabled() || object.IsDeleted()) {
            return RE::BSContainer::ForEachResult::kContinue;
        }
        
        // Check if it's a container reference (even if base form isn't a container type)
        auto refContainer = object.As<RE::TESObjectCONT>();
        if (refContainer) {
            return RE::BSContainer::ForEachResult::kContinue;
        }
        
        // Check distance
        float distance = player->GetPosition().GetDistance(object.GetPosition());
        if (distance >= visionRange) {
            return RE::BSContainer::ForEachResult::kContinue;
        }
        
        // Get item name and FormIDs (both RefID and BaseID)
        std::string itemName = object.GetDisplayFullName();
        if (itemName.empty()) {
            itemName = baseForm->GetName();
        }
        
        if (itemName.empty()) {
            return RE::BSContainer::ForEachResult::kContinue;
        }
        
        // Check if taking this item would be stealing
        bool isStealing = WouldBeStealing(&object, player);
        
        // Check if player is looking at this item
        bool isLookingAt = (crosshairTarget && crosshairTarget->GetFormID() == object.GetFormID());
        
        // Format as "RefID:BaseID:ItemName" with optional markers
        uint32_t refFormID = object.GetFormID();
        uint32_t baseFormID = baseForm->GetFormID();
        std::string itemEntry = std::format("0x{:X}:0x{:X}:{}", refFormID, baseFormID, itemName);
        
        // Add markers
        if (isLookingAt) {
            itemEntry += " (LOOKING AT)";
        }
        if (isStealing) {
            itemEntry += " (STEALING)";
        }
        
        results.push_back(itemEntry);
        
        return RE::BSContainer::ForEachResult::kContinue;
    });
    
    // Join using comma separator
    std::string buffer;
    for (size_t i = 0; i < results.size(); ++i) {
        buffer += results[i];
        if (i < results.size() - 1) {
            buffer += ",";
        }
    }
    
    return buffer;
}

RE::TESObjectREFR* findActorInCell(std::string targetName, RE::TESObjectCELL* cell, RE::Actor* sourceActor,
                                   float radius, bool allowDead) {
    RE::TESObjectREFR* target = nullptr;
    bool interior = PlayerIsInInterior();
    auto prd = RE::PlayerCharacter::GetSingleton()->GetParentCell();

    auto player = RE::PlayerCharacter::GetSingleton();
    auto playerLoc = player->GetCurrentLocation();
    RE::Actor* result = nullptr;
    RE::TESObjectREFR* from = sourceActor->AsReference();

    // Validate source actor
    if (!sourceActor || !from) {
        logger::warn("findActorInCell: Invalid source actor");
        return nullptr;
    }

    float lastDistance = 10000;
    if (cell) {
        cell->ForEachReference([&target, &targetName, &from, &sourceActor, allowDead,
                                &lastDistance](RE::TESObjectREFR& object) -> RE::BSContainer::ForEachResult {
            float distance = 10000;
            // logger::info("[findActorInCell], found reference {} , type  {:X}", object.GetName(), object.GetFormID());

            RE::TESForm* baseForm = object.GetBaseObject();

            if (!baseForm) {
                return RE::BSContainer::ForEachResult::kContinue;
            }

            // logger::info("[findActorInCell], found reference {} , type  {:X}", object.GetName(),
            // baseForm->GetFormID());
            if (baseForm->formType == RE::FormType::NPC) {
                const char* currentNPC = object.GetName();
                // logger::info("[findActorInCell], found NPC {}", currentNPC);

                RE::Actor* actorNpc = object.As<RE::Actor>();
                if (actorNpc) {
                    if (actorNpc->IsDead() && !allowDead) return RE::BSContainer::ForEachResult::kContinue;
                    if (actorNpc->IsDisabled()) {
                        logger::info("[findActorInCell], disabled {}", currentNPC);
                        return RE::BSContainer::ForEachResult::kContinue;
                    }

                    std::string actorLabel(actorNpc->GetDisplayFullName());

                    if (containsCaseInsensitive(actorLabel, targetName)) {
                        bool hasLos = false;
                        if (sourceActor && actorNpc) {
                            sourceActor->HasLineOfSight(actorNpc->AsReference(), hasLos);
                        }
                        if (hasLos) {
                            distance = sourceActor->GetPosition().GetDistance(actorNpc->GetPosition());
                            if (distance < lastDistance) {
                                lastDistance = distance;
                                target = &object;
                            }

                            // return RE::BSContainer::ForEachResult::kStop;
                        }
                    }
                    // logger::info("Actor {} ", actorLabel);
                } else {
                    // Reference found, but no actor

                    std::string actorLabel(object.GetDisplayFullName());
                    logger::info("[findActorInCell], reference {}", actorLabel);
                    if (containsCaseInsensitive(actorLabel, targetName)) {
                        bool hasLos = true;
                        if (sourceActor && actorNpc) {
                            sourceActor->HasLineOfSight(actorNpc->AsReference(), hasLos);
                        }
                        if (hasLos) {
                            target = &object;
                            // return RE::BSContainer::ForEachResult::kStop;
                        }
                    }

                    // logger::info("NPC {} ", actorLabel);
                }
            }

            return RE::BSContainer::ForEachResult::kContinue;
        });
    }

    if (target)
        return target;

    else {
        // Actors lurking around
        if (const auto processLists = RE::ProcessLists::GetSingleton(); processLists) {
            for (auto& targetHandle : processLists->highActorHandles) {
                if (auto targetLocal = targetHandle.get();
                    targetLocal && targetLocal->GetActorRuntimeData().currentProcess) {
                    bool hasLos = false;
                    if (sourceActor && targetLocal) {
                        // sourceActor->HasLineOfSight(targetHandle.get().get()->AsReference(), hasLos);
                        if (targetHandle.get().get()) {
                            targetHandle.get().get()->HasLineOfSight(player->AsReference(), hasLos);
                        } else {
                            ;
                        }
                    }
                    std::string actorLabel(targetLocal.get()->GetDisplayFullName());
                    logger::debug("[findActorInCell], found actor {}, haslos: {}", actorLabel, hasLos ? "yes" : "no");
                    if (hasLos && !actorLabel.empty()) {
                        if (containsCaseInsensitive(actorLabel, targetName) && (!targetLocal->IsDead()) &&
                            (!targetLocal->IsDisabled())) {
                            // if (agentActor->GetPosition().GetDistance(targetLocal->GetPosition())
                            // <HERIKA_MAX_VISION_RANGE) {
                            target = targetHandle.get().get()->AsReference();
                            return target;
                            //}
                        }
                    }
                }
            }
        }
    }
    return nullptr;
}

RE::FormID findFurnitureInCell(RE::TESObjectCELL* cell, RE::Actor* herika, int mode) {
    RE::FormID foundTarget = 0;
    float currentDistance = 100000;

    std::vector<RE::FormID> formIDList;
    // formIDList hold list of occupied furniture
    if (const auto processLists = RE::ProcessLists::GetSingleton(); processLists) {
        for (auto& targetHandle : processLists->highActorHandles) {
            if (auto target = targetHandle.get(); target && target->GetActorRuntimeData().currentProcess) {
                if (target->GetOccupiedFurniture()) {
                    formIDList.push_back(target->GetOccupiedFurniture().get().get()->GetFormID());
                    logger::debug("{} is using  0x{:08x}", target->GetDisplayFullName(),
                                  target->GetOccupiedFurniture().get().get()->GetFormID());
                }
            }
        }
    }

    auto player = RE::PlayerCharacter::GetSingleton();
    if (player->GetOccupiedFurniture()) {
        formIDList.push_back(player->GetOccupiedFurniture().get().get()->GetFormID());
    }

    RE::BSFurnitureMarker::AnimationType furnitureMode = RE::BSFurnitureMarker::AnimationType::kSit;
    if (mode == 1) {
        furnitureMode = RE::BSFurnitureMarker::AnimationType::kSleep;
        logger::info("[TAKEASEAT] Using BED mode");

    } else {
        furnitureMode = RE::BSFurnitureMarker::AnimationType::kSit;
    }

    if (cell) {
        cell->ForEachReference([&foundTarget, &herika, &currentDistance, furnitureMode,
                                &formIDList](RE::TESObjectREFR& object) -> RE::BSContainer::ForEachResult {
            RE::TESForm* baseForm = object.GetBaseObject();

            if (baseForm->formType == RE::FormType::Furniture) {
                const char* currentFurniture = object.GetName();
                //logger::info("[TAKEASEAT] Posible target {}", currentFurniture);

                if (object.IsDisabled()) return RE::BSContainer::ForEachResult::kContinue;
                if (object.IsMarkedForDeletion()) return RE::BSContainer::ForEachResult::kContinue;
                if (object.IsDeleted()) return RE::BSContainer::ForEachResult::kContinue;

                auto furnitureForm = baseForm->As<RE::TESFurniture>();
                if (furnitureForm) {
                    if (furnitureForm->workBenchData.benchType != RE::TESFurniture::WorkBenchData::BenchType::kNone) {
                        logger::info("[TAKEASEAT] Posible sit target {} is a workbench 0x{:08x}", currentFurniture,
                                     object.GetFormID());
                        return RE::BSContainer::ForEachResult::kContinue;
                    }
                }
                bool found = false;

                auto it = std::find(formIDList.begin(), formIDList.end(), object.GetFormID());

                if (it != formIDList.end()) {
                    //logger::info("[TAKEASEAT] Posible sit target {} is blocked 0x{:08x}", currentFurniture,object.GetFormID());
                    return RE::BSContainer::ForEachResult::kContinue;
                } else {
                    // logger::info("[TAKEASEAT] Posible sit target {} is free 0x{:08x}", currentFurniture,object.GetFormID());
                }

                if (object.IsActivationBlocked()) {
                    logger::info("[TAKEASEAT] Possible sit target {} is IsActivationBlocked 0x{:08x}", currentFurniture,object.GetFormID());
                    return RE::BSContainer::ForEachResult::kContinue;
                }

                // https://github.com/VersuchDrei/OStimNG/blob/0440ae951089e0c27d2ae6b01cfcbdb640f91c6d/skse/src/Furniture/Furniture.cpp#L12
                auto root = object.Get3D();
                if (root) {
                    auto extra = root->GetExtraData("FRN");
                    if (extra) {
                        auto node2 = netimmerse_cast<RE::BSFurnitureMarkerNode*>(extra);
                        bool allMarkersToSit = true;
                        bool someMarkersToSit = false;
                        for (auto it = node2->markers.begin(); it != node2->markers.end(); ++it) {
                            RE::BSFurnitureMarker* marker = (it);

                            // if (element->animationType.get() == RE::BSFurnitureMarker::AnimationType::kSit) {
                            //if (marker->animationType.all(furnitureMode)) {
                            if (marker->animationType.get() == furnitureMode) {
                                if (std::abs(marker->offset.z - 34) < 1) {
                                    allMarkersToSit = allMarkersToSit && true;
                                    someMarkersToSit = someMarkersToSit || true;
                                } else {
                                    allMarkersToSit = allMarkersToSit && false;
                                    someMarkersToSit = someMarkersToSit || false;  

                                    logger::info("[TAKEASEAT] Possible sit target {}, discarded by z offset {} ",
                                                 currentFurniture, marker->offset.z);
                                }
                            } else
                                allMarkersToSit = false;
                        }

                        if (allMarkersToSit) {
                            found = true;
                            float localcurrentDistance = object.GetPosition().GetDistance(herika->GetPosition());
                            if (localcurrentDistance < 1) {  // occupied
                                found = false;
                                logger::info(
                                    "[TAKEASEAT] allMarkersToSit Possible sit target {}, discarded by low distance {} ",
                                    currentFurniture, localcurrentDistance);
                            } else {
                                logger::info("[TAKEASEAT] allMarkersToSit Possible sit target {}, distance {} ",
                                             currentFurniture, localcurrentDistance);
                            }
                        } else if (someMarkersToSit) {
                            found = false;
                            float localcurrentDistance = object.GetPosition().GetDistance(herika->GetPosition());
                            if (localcurrentDistance < 1) {  // occupied
                                found = false;
                                logger::info(
                                    "[TAKEASEAT] someMarkersToSit Possible sit target {}, discarded by low distance {} ",
                                    currentFurniture, localcurrentDistance);
                            } else {
                                logger::info(
                                    "[TAKEASEAT] someMarkersToSit Possible sit target {}, seems valid low distance {} ",
                                    currentFurniture, localcurrentDistance);
                            }
                        } else {
                            logger::info("[TAKEASEAT] Possible sit target {}, none markers allow sitting ",
                                         currentFurniture);
                        }
                    } else {
                        logger::info("[TAKEASEAT] Possible sit target {} has no extra data, 0x{:08x}", currentFurniture,
                                     object.GetFormID());
                    }
                } else {
                    logger::info("[TAKEASEAT] Possible sit target {} has no 3d, 0x{:08x}", currentFurniture,
                                 object.GetFormID());
                }
                // if (containsCaseSensitive(std::string(currentFurniture), "Chair")) found = found & true;
                // if (containsCaseSensitive(std::string(currentFurniture), "Bench")) found = found & true;
                if (found) {
                    float localcurrentDistance = object.GetPosition().GetDistance(herika->GetPosition());
                    if (localcurrentDistance < currentDistance) {
                        foundTarget = object.GetFormID();
                        currentDistance = localcurrentDistance;
                        logger::info("Chosen target {}, distance {},  0x{:08x} ", object.GetName(), currentDistance,
                                     object.GetFormID());
                    }
                }
            } else if (baseForm->formType == RE::FormType::IdleMarker) {
                const char* currentIdleMarker = object.GetName();
                
                logger::info("[TAKEASEAT] Possible sit target <{}> IdleMarker 0x{:08x}", currentIdleMarker,
                             object.GetFormID());
                
            }

            return RE::BSContainer::ForEachResult::kContinue;
        });
    }
    if (foundTarget) logger::info("[TAKEASEAT] Return final selected FormID {:08x} ", foundTarget);
    return foundTarget;
}

void StartSneakTo(std::string targetName) {}

RE::Actor* findClosestAgent() {
    AIAgentManager& aiam = AIAgentManager::getInstance();
    int index = -1;
    int i = 0;

    float minDistance = MIN_DISTANCE;
    auto player = RE::PlayerCharacter::GetSingleton();
    auto localPlayerCell = player->GetParentCell();
    std::string beings =
        InspectSurroundings(player->AsReference(), true, HERIKA_MAX_VISION_RANGE, ",", HERIKA_MAX_VISION_RANGE);

    auto cameraObject = RE::CrosshairPickData::GetSingleton()->target;

    RE::TESObjectREFRPtr refUnderCrossHair;
    if (cameraObject) {
        refUnderCrossHair = cameraObject.get();
        if (refUnderCrossHair) logger::info("Ref under crosshair {} ", refUnderCrossHair->GetDisplayFullName());
    }

    if (cameraObject) {
        if (cameraObject.get()->GetFormType() == RE::FormType::ActorCharacter) {
            auto targetActor = cameraObject.get()->As<RE::Actor>();
            AIAgentManager& aiam = AIAgentManager::getInstance();
            for (const auto& agent : aiam.getAgents()) {
                if (agent->getActor()->GetFormID() == targetActor->GetFormID()) {
                    if (agent->getActor()->GetParentCell() == localPlayerCell) {
                        if (agent->isPresent(beings)) return agent->getActor();
                    }
                }
            }

        } else {
            logger::info("Object under camera {} is not an NPC", cameraObject.get()->GetName());
        }
    }

    int n = aiam.getAgents().size();
    for (const auto& agent : aiam.getAgents()) {
        logger::info("Checking Actor {} {}/{}", i, agent->getActorName(), n);
        float distance = player->GetPosition().GetDistance(agent->getActor()->GetPosition());

        auto debugActor = agent->getActor();

        if (!debugActor) {
            logger::error("ERROR {} {} actor is unreachable, try to readd", i, agent->getActorName());
            i++;
            continue;
        }
        auto localAgentCell = agent->getActor()->GetParentCell();

        if (beings.find(agent->getActorName()) == std::string::npos) {
            logger::info("Discarding {} {} because actor is not around", i, agent->getActorName());
            i++;
            continue;
        }

        if (agent->getActorName() == NARRATOR_NAME) {
            logger::info("Discarding {} {} because actor is Narrator", i, NARRATOR_NAME);
            i++;
            continue;
        }

        if (!localAgentCell || !agent->getActor()) {
            logger::info("Discarding {} because no actor/no cell", i);
            i++;
            continue;
        }

        if (debugActor->IsOffLimits()) {
            logger::info("Discarding {} because offlimit", i);
            i++;
            continue;
        }

        if (distance < minDistance && distance > 1) {
            index = i;
            minDistance = distance;
            logger::debug("Selecting {} because distance ({})", agent->getActor()->GetDisplayFullName(), distance);
        } else {
            logger::debug("Discarding {} because distance ({})", agent->getActor()->GetDisplayFullName(), distance);
        }

        i++;
    }

    if (index == -1) {
        logger::info("No non-narrator agent available; returning null actor");
        return nullptr;

    } else {
        return aiam.getAgents().at(index)->getActor();
    }
}

void StartAttack(std::string targetName, RE::Actor* actor, bool lethal) {
    if (!actor) {
        logger::warn("StartAttack: actor is null");
        return;
    }
    RE::TESObjectCELL* cell = actor->GetParentCell();

    auto target = findActorInCell(targetName, cell, actor, 0, false);
    AIAgentManager& aiam = AIAgentManager::getInstance();
    auto agentPtr = aiam.getAgentByName(actor->GetDisplayFullName());

    if (target == nullptr) {
        if (aiam.getPlayerName() == targetName) target = RE::PlayerCharacter::GetSingleton()->AsReference();
    }
    if (!agentPtr) {
        logger::warn("StartAttack: agentptr is null");
        return;
    }
    if (target != nullptr) {
        agentPtr.get()->setAttackTarget(target);
        auto targetActor = target->As<RE::Actor>();
        std::string resolvedTargetName = targetActor ? targetActor->GetDisplayFullName() : targetName;
        if (resolvedTargetName.empty()) resolvedTargetName = targetName;
        const std::string notificationText =
            lethal ? std::format("[CHIM] {} attacks {}", actor->GetDisplayFullName(), resolvedTargetName)
                   : std::format("[CHIM] {} brawls with {}", actor->GetDisplayFullName(), resolvedTargetName);
        RE::DebugNotification(notificationText.c_str());

        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
        auto args = RE::MakeFunctionArguments(std::move(actor), std::move(target->AsReference()), std::move(lethal));
        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentAIMind", "AttackTarget",
                                                                                   args, callback);

        SpeakManager::getInstance().deleteQueue();  // 1.0.12
        // I think some functions should interrupt speaking

        EndCommand("Attack", actor->GetDisplayFullName());  // Payrus will take care of ending

    } else {
        /* HTTPLogger->info(
            "combat|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
            "(Context: Herika issued {Attack(" + targetName + " )})( Herika cannot see " + targetName + ", target not
           found)");*/
        if (!lethal)
            HTTPManager::stream(
                std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                            "command@Brawl@" + targetName + "@Error. target " + targetName + " not found "),
                agentPtr->getActor());
        else
            HTTPManager::stream(
                std::format("funcret|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                            "command@Attack@" + targetName + "@Error. target " + targetName + " not found "),
                agentPtr->getActor());

        RE::DebugNotification(std::string("[CHIM] Target not found: ").append(targetName).append(".").c_str());
        EndCommandError("Attack", actor->GetDisplayFullName());
    }
}

void Follow(std::string targetName) {}

void StopCurrent(RE::Actor* npc) {
    auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
    auto args = RE::MakeFunctionArguments(std::move(npc));
    RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentAIMind", "StopCurrent", args,
                                                                               callback);
    clearQueue("command");
}

void EndCommand(std::string command, std::string actor) {
    if (command.contains("TravelTo") || command.contains("MoveTo")) {
        AIAgentManager& aiam = AIAgentManager::getInstance();
        auto agentPtr = aiam.getAgentByName(actor);

        if (!agentPtr) {
            logger::info("No AI actor found, can't end command");
            return;
        } else {
            logger::info("Releasing actor, command done {} for ", command, actor);
        }

        agentPtr.get()->setCurrentCommand("");
        agentPtr.get()->setCommandBusy(false);
    }
}

void EndCommandError(std::string command, std::string actor) {}

RE::BSEventNotifyControl HerikaAnimGraphEventSink::ProcessEvent(
    const RE::BSAnimationGraphEvent* a_event, RE::BSTEventSource<RE::BSAnimationGraphEvent>* a_eventSource) {
    // logger::info("INto HerikaAnimGraphEventSink {}", a_event->tag);

    return RE::BSEventNotifyControl::kContinue;
}

bool commandAnimation(std::string anim, RE::Actor* actor) {
    if (!GlobalAnimations) {
        logger::info("[ANIMATION]  Skipping animation by GlobalAnimations");
        return false;
    }

    // PlayIdle

    /*
    auto idle= RE::TESForm::LookupByEditorID(anim.c_str());

    if (idle) {
        logger::info("Commanding animation: {} for {}", anim, actor->GetDisplayFullName());
        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
        auto args = RE::MakeFunctionArguments(std::move(actor),std::move(idle->GetFormID()));
        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentAIMind", "PlayIdle", args,
                                                                                   callback);
    } else {
        logger::info("Not commanding animation: {} for {}", anim, actor->GetDisplayFullName());
    }
    */

    AIAgentManager& aiam = AIAgentManager::getInstance();
    auto npcAgent = aiam.getAgentByName(actor->GetDisplayFullName());
    if (!npcAgent->isAvailableforAnimation()) {
        auto currentcommand = npcAgent->getCurrentCommand();
        auto currentanimation = npcAgent->getCurrentAnimation();

        logger::warn("[ANIMATION]  Agent {} is not available for animation, current command {},current animation {}",
                     npcAgent->getActorName(), currentcommand, currentanimation);
        return false;
    }
    if (!npcAgent) {
        logger::warn("[ANIMATION]  npcAgent is null");
        return false;
    } else {
        logger::warn("[ANIMATION]  setCurrentAnimation {} stored for {}", npcAgent->getActorName(), anim);
        npcAgent->setCurrentAnimation(anim);
    }

    auto npcName = npcAgent->getActorName();
    std::string command = "AnimationEvent";
    std::string parameter = anim;

    auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
    auto args = RE::MakeFunctionArguments(std::move(npcName), std::move(command), std::move(parameter));

    RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentAIMind", "SendInternalEvent",
                                                                               args, callback);
    /*
    auto actorForm = RE::TESForm::LookupByID(npcAgent->GetFormId());
    if (!actorForm) {
        logger::warn("[ANIMATION]  actorForm is null");
        return false;
    }
    auto newActor = actorForm->As<RE::Actor>();

    if (!newActor) {
        logger::warn("[ANIMATION]  newActor is null");
        return false;
    }

    if (actor->IsInCombat() || actor->IsOnMount() || actor->IsHorse() || actor->IsPlayer() ) {
        logger::warn("[ANIMATION]  newActor is IsHorse or IsOnMount or IsInCombat or IsPlayer");
        return false;
    }




    logger::info("[ANIMATION] Commanding animation: {} for {}", anim, actor->GetDisplayFullName());

    auto args = RE::MakeFunctionArguments(std::move(newActor), std::move(anim));


    SKSE::GetTaskInterface()->AddTask([args]() {
        //actor->NotifyAnimationGraph(anim);
        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentNpcUtil", "NpcPlayIdle",
                                                                                   args, callback);
        }
    );
    */
    return true;
}

void resetAnimation() {}
