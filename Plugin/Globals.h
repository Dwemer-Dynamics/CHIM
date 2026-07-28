#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "RE/Skyrim.h"
#include "SpatialAwareness.h"

#define HERIKA_MAX_VISION_RANGE 5000

namespace logger = SKSE::log;


extern std::string InspectSurroundings(RE::TESObjectREFR* reference, bool useCache, float visionRange,std::string separator,float farAwayLimit);
extern void ExtendPlayerSpeechMaintenanceSuppress(std::chrono::milliseconds duration);
extern bool IsPlayerSpeechMaintenanceSuppressed();
extern std::string InspectManagedAgents(RE::TESObjectREFR* reference, float visionRange, const std::string& separator,
                                        float farAwayLimit, bool includeNarrator);
#ifndef AUDIBLE_ACTOR_DESCRIPTOR_DEFINED
#define AUDIBLE_ACTOR_DESCRIPTOR_DEFINED
struct AudibleActorDescriptor {
    RE::FormID formId = 0;
    std::string label;
    float airDistance = 0.0f;
    float volume = 0.0f;
    float pathDistance = 0.0f;
    float pathRatio = 0.0f;
    std::string reason;
    int openDoorCount = 0;
    int closedDoorCount = 0;
    bool navmeshPathUsed = false;
    bool navmeshPathFound = false;
    bool losFallbackUsed = false;
    bool hasLineOfSight = false;
    bool hostile = false;
    bool busy = false;
    bool inCombat = false;
    bool restrained = false;
    bool canCommunicate = false;
};
#endif

extern std::vector<AudibleActorDescriptor> CollectAudibleActors(RE::TESObjectREFR* reference, float visionRange);
extern std::string DescribeAudibleActors(const std::vector<AudibleActorDescriptor>& audibleActors,
                                         const std::string& separator);
extern SpatialAwareness::Settings GetPlayerSpeechSpatialSettings(RE::Actor* speaker, float visionRange);
extern std::string InspectAudibleActors(RE::TESObjectREFR* reference, bool useCache, float visionRange,
                                        std::string separator);

extern RE::TESForm* NullVoiceType;

extern bool CombatDialogueEnabled;
extern bool PreserveQueueDuringAction;
extern bool PauseDialogueWhenMenuOpen;
extern bool PlayerTtsTraditionalDialogueEnabled;
extern bool AIQuestProgressionEnabled;
extern bool AllowActorsOnScene;
extern bool GodMode;
extern bool AutoAddHostile;
extern bool AutoAddAllRaces;
extern void ResetQuestProgressionBridgeState();
extern void ScheduleQuestProgressionFullResync(const char* reason, int delayMs, bool includeInventory);

#define _DELAY_SECONDS_RESEND 3

#ifndef AIAGENT_H
    #define AIAGENT_H

// Narrator configuration
inline constexpr const char* NARRATOR_NAME = "The Narrator";

#define MIN_DISTANCE 3000.0f

#include <iostream>
#include <unordered_map>
#include <string>


extern float DISTANCE_ACTIVATING_NPC_OUT ;
extern float DISTANCE_ACTIVATING_NPC_IN;
extern bool ENABLE_AUTOADDNPC ;

// Shouting distance threshold - if distance exceeds this, use loud voice
constexpr float SHOUTING_DISTANCE_THRESHOLD = 800.0f;


class AIAgent {
public:
    AIAgent(const AIAgent&) = delete;
    AIAgent& operator=(const AIAgent&) = delete;
    AIAgent() = default;  // Default constructor

    RE::Actor* getActor() {
        if (!this) {
            logger::error("getActor called on null AIAgent");
            return nullptr;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        return resolveActorUnsafe();
    }

    RE::Actor* getActorByFormId() {
        if (!this) {
            logger::error("getActor called on null AIAgent");
            return nullptr;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        return resolveActorUnsafe();
    }


    std::string getActorName() {
        if (!this) {
            logger::error("getActorName called on null AIAgent");
            return "";
        }
        std::lock_guard<std::mutex> lock(mutex_);
        return name;

    }

    bool isPresent(const std::string& presentActors) {
        std::string actorName = getActorName();
        std::transform(actorName.begin(), actorName.end(), actorName.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        auto trim = [](std::string value) {
            const auto first = value.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) return std::string{};
            const auto last = value.find_last_not_of(" \t\r\n");
            return value.substr(first, last - first + 1);
        };

        std::stringstream stream(presentActors);
        std::string token;
        while (std::getline(stream, token, ',')) {
            std::transform(token.begin(), token.end(), token.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            token = trim(token);

            if (token == actorName) {
                return true;
            }

            const std::string statusPrefix = actorName + " (";
            if (token.rfind(statusPrefix, 0) == 0) {
                return token.find("(far away)") == std::string::npos;
            }
        }

        return false;
    }

    bool isAvailableforAnimation() {
        auto actorHerika = getActor();
        if (!actorHerika) {
            return false;
        }
        if (!actorHerika->IsSneaking())
            if (!actorHerika->GetOccupiedFurniture())
                if (!actorHerika->IsOnMount())
                    if (!actorHerika->IsRunning())
                        if (!isCommandBusy())
                            if (!actorHerika->IsInCombat()) 
                                if (!getAnimationBusy()) return true;

        return false;
    }

    bool isAvailableforDialog(bool allowOnScene) {
        auto form = RE::TESForm::LookupByID(GetFormId());
        if (!form) {
            logger::warn("Actor form not found for ID: {}", GetFormId());
            return false;
        }

        // Check conversation cooldown
        if (hasConversationCooldown()) {
            logger::debug("{} is on conversation cooldown", getActorName());
            return false;
        }

        /* Can talk but can issue actions
        if (isExternalLocked()) {
            logger::debug("{} is externally locked", getActorName());
            return false;
        }*/

        auto actorHerika = form->As<RE::Actor>();

        if (!actorHerika) {
            logger::warn("Form is not an actor: {}", GetFormId());
            return false;
        }
        if (actorHerika->IsInCombat() && !CombatDialogueEnabled) {
            logger::debug("{} is in combat and combat dialogue is disabled", getActorName());
            return false;
        }
        if (actorHerika->IsAttacking() && !CombatDialogueEnabled) {
            logger::debug("{} is attacking and combat dialogue is disabled", getActorName());
            return false;
        }
        if (actorHerika->IsInKillMove() && !CombatDialogueEnabled) {
            logger::debug("{} is in kill move and combat dialogue is disabled", getActorName());
            return false;
        }

        /*if (actorHerika->IsOffLimits()) {
            logger::debug("{} is off limits", getActorName());
            return false;
        }*/

        if (actorHerika->GetCurrentScene() && allowOnScene==false) {
            logger::warn("ACTOR IN SCENE (not allowed) by conf: {} {}", actorHerika->GetDisplayFullName(),
                         actorHerika->GetCurrentScene()->GetFormEditorID());
            // Note: Previously returned false here, but GetCurrentScene() catches routine
            // sandbox/idle AI scenes (not just scripted quest scenes), blocking most NPCs
            // in taverns, shops, etc. Changed to log-only to preserve dialog availability.
            return false;
        }

        
        auto dialogueItemTarget = actorHerika->GetActorRuntimeData().dialogueItemTarget;
        if (dialogueItemTarget && dialogueItemTarget.get()) {
            logger::info("SEEMS {} is talking to {} ", actorHerika->GetDisplayFullName(),
                         dialogueItemTarget.get()->GetName());
        }
        


        if (actorHerika->GetOccupiedFurniture()) {
            auto furnHandl = actorHerika->GetOccupiedFurniture();
            auto furnPointer = actorHerika->GetOccupiedFurniture().get();
            RE::TESObjectREFR *furnRef = furnPointer->AsReference();
            if (furnRef) {
                RE::TESFurniture* furniture = furnRef->GetBaseObject()->As<RE::TESFurniture>();
                if (furniture && furniture->formType == RE::FormType::Furniture) {
                    if (furniture->furnFlags.any(RE::TESFurniture::ActiveMarker::kCanSleep)) {
                        logger::debug("{} is sleeping", getActorName());
                        return false;
                    }
                }
            }
                        
        }
            
        logger::debug("{} is available for dialog", getActorName());
        return true;
    }

    void setActor(RE::Actor* actor) {
        if (!actor) return;
        std::lock_guard<std::mutex> lock(mutex_);
        this->actor = actor;
        this->formID = actor->GetFormID();
        name = actor->GetDisplayFullName();
        name.erase(0, name.find_first_not_of(' '));
        name.erase(name.find_last_not_of(' ') + 1);
        
    }


    void setAnimationBusy(bool busy) {
        std::lock_guard<std::mutex> lock(mutex_);
        inAnimation = busy;
    }

    bool getAnimationBusy() {
        std::lock_guard<std::mutex> lock(mutex_);
        return inAnimation;
    }
    void setCommandBusy(bool busy) {
        std::lock_guard<std::mutex> lock(mutex_);
        commandBusy = busy;
    }

    void setAttackTarget(RE::TESObjectREFR *target) {
        std::lock_guard<std::mutex> lock(mutex_);
        attackTarget = target;
    }

    RE::TESObjectREFR* getAttackTarget() {
        std::lock_guard<std::mutex> lock(mutex_);
        return attackTarget;
    }


    bool isWaitinForRes() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto currentTime = std::chrono::steady_clock::now();
        auto timeElapsed = currentTime - lastAccessTime;
        if (timeElapsed > std::chrono::seconds(10)) {
            waitingForRes = false;
        }

        // Update the last access time
        lastAccessTime = currentTime;
        return waitingForRes;
    }

    void setWaitingForRes(bool busy) {
        std::lock_guard<std::mutex> lock(mutex_);
        waitingForRes = busy;
    }

    bool isTalking() {
        try {
            if (!this) {
                logger::error("isTalking called on null AIAgent");
                return false;
            }
            
            // Verify mutex is valid before locking
            if (&mutex_ == nullptr) {
                logger::error("Invalid mutex in isTalking");
                return false;
            }

            std::lock_guard<std::mutex> lock(mutex_);
            return talking;
        } catch (const std::system_error& e) {
            logger::error("Mutex lock failed in isTalking(): {}", e.what());
            return false;
        } catch (const std::exception& e) {
            logger::error("Exception in isTalking(): {}", e.what());
            return false;
        } catch (...) {
            logger::error("Unknown exception in isTalking()");
            return false;
        }
    }

    void setTalking(bool talkingNow) {
        try {
            if (!this) {
                logger::error("setTalking called on null AIAgent");
                return;
            }

            // Verify mutex is valid before locking  
            if (&mutex_ == nullptr) {
                logger::error("Invalid mutex in setTalking");
                return;
            }

            std::lock_guard<std::mutex> lock(mutex_);
            talking = talkingNow;
        } catch (const std::system_error& e) {
            logger::error("Mutex lock failed in setTalking(): {}", e.what());
        } catch (const std::exception& e) {
            logger::error("Exception in setTalking(): {}", e.what());
        } catch (...) {
            logger::error("Unknown exception in setTalking()");
        }
    }

    bool isCommandBusy() {
        std::lock_guard<std::mutex> lock(mutex_);
        return commandBusy;
    }

    void setCurrentTarget(RE::TESObjectREFR* t) {
        std::lock_guard<std::mutex> lock(mutex_);
        currentTarget = t;
    }

    void setCurrentCommand(std::string command) {
        std::lock_guard<std::mutex> lock(mutex_);
        currentCommand = command;
    }
    RE::TESObjectREFR* getCurrentTarget() {
        std::lock_guard<std::mutex> lock(mutex_);
        return currentTarget;
    }
    std::string getCurrentCommand() {
        std::lock_guard<std::mutex> lock(mutex_);
        return currentCommand;
    }

    bool isReadingBook() {
        std::lock_guard<std::mutex> lock(mutex_);
        return readingBook;
    }
    void setReadingBook(bool t) {
        std::lock_guard<std::mutex> lock(mutex_);
        readingBook = t;
    }

    std::string getCurrentAnimation() {
        std::lock_guard<std::mutex> lock(mutex_);
        return currentAnimation;
    }
    void setCurrentAnimation(std::string a) {
        std::lock_guard<std::mutex> lock(mutex_);
        currentAnimation.clear();
        currentAnimation.append(a);
    }

    std::chrono::steady_clock::time_point GetLastTimeTalk() {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastTimeTalk;
    }
    void SetLastTimeTalk() {
        std::lock_guard<std::mutex> lock(mutex_);
        lastTimeTalk = std::chrono::steady_clock::now() ;
    }

    bool isClean() {
        std::lock_guard<std::mutex> lock(mutex_);
        return cleaned;
    } 

    void setClean(bool clean) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!clean)
            lastTimeTalk = std::chrono::steady_clock::now();
        cleaned = clean;
    }


    bool isRestored() {
        std::lock_guard<std::mutex> lock(mutex_);
        return restored;
    }

    void setRestored(bool t) {
        std::lock_guard<std::mutex> lock(mutex_);
        restored = t;
    }

     bool isNarrator() {
        std::lock_guard<std::mutex> lock(mutex_);
        return narrator;
    }

    void setNarrator(bool isNarrator) {
        std::lock_guard<std::mutex> lock(mutex_);
        narrator = isNarrator;
    }

    void setAvailable(bool clean) {
        std::lock_guard<std::mutex> lock(mutex_);
        available = clean;
    }
    bool getAvailable() {
        std::lock_guard<std::mutex> lock(mutex_);
        return available;
    }

    void setOriginalVoice(RE::BGSVoiceType* o) {
        std::lock_guard<std::mutex> lock(mutex_);
        originalVoice = o;
    }

    void increaseFarAwayCounter() {
        std::lock_guard<std::mutex> lock(mutex_);
        farAwayCounter++;
    }

    int getFarAwayCounter() {
        std::lock_guard<std::mutex> lock(mutex_);
        return farAwayCounter;
    }

    void resetFarAwayCounter() {
        std::lock_guard<std::mutex> lock(mutex_);
        farAwayCounter = 0;
    }

    void increaseMissingPresenceCounter() {
        std::lock_guard<std::mutex> lock(mutex_);
        missingPresenceCounter++;
    }

    int getMissingPresenceCounter() {
        std::lock_guard<std::mutex> lock(mutex_);
        return missingPresenceCounter;
    }

    void resetMissingPresenceCounter() {
        std::lock_guard<std::mutex> lock(mutex_);
        missingPresenceCounter = 0;
    }

    int getForgetCleanCounter() {
        std::lock_guard<std::mutex> lock(mutex_);
        return forgetCleanCounter;
    }

    void resetForgetCleanCounter() {
        std::lock_guard<std::mutex> lock(mutex_);
        forgetCleanCounter = 0;
    }


    int getBoredEventsFired() {
        std::lock_guard<std::mutex> lock(mutex_);
        return boredEventsFired;
    }

    void incBoredEventsFired() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (boredEventsFired > 5) 
            boredEventsFired = 0;
        else
            boredEventsFired++;
    }

    void increaseForgetCleanCounter() {
        std::lock_guard<std::mutex> lock(mutex_);
        forgetCleanCounter++;
    }


    RE::BGSVoiceType* getOriginalVoice() {
        std::lock_guard<std::mutex> lock(mutex_);
        return originalVoice;
    }

    void overrideActorName(std::string newName) {
        std::lock_guard<std::mutex> lock(mutex_);
        name = newName;

    }

    void markToBeDeleted() {
        std::lock_guard<std::mutex> lock(mutex_);
        deleteMark = true;
    }

    bool mustBeDeleted() {
        std::lock_guard<std::mutex> lock(mutex_);
        return deleteMark;
    }


    void setOnScene(bool isOnScene) {
        std::lock_guard<std::mutex> lock(mutex_);
        wasOnScene = isOnScene;
    }

    bool getWasOnScene() {
        std::lock_guard<std::mutex> lock(mutex_);
        return wasOnScene;
    }

    RE::FormID GetFormId() {
        std::lock_guard<std::mutex> lock(mutex_);
        return formID;
    }

    void setManuallyAdded(bool v) {
        std::lock_guard<std::mutex> lock(mutex_);
        manuallyAdded = v;
    }

    bool isManuallyAdded() {
        std::lock_guard<std::mutex> lock(mutex_);
        return manuallyAdded;
    }
    
    void setExternalLocked(bool v) {
        std::lock_guard<std::mutex> lock(mutex_);
        externalLocked = v;
    }

    bool isExternalLocked() {
        std::lock_guard<std::mutex> lock(mutex_);
        return externalLocked;
    }

    // Voice sample tracking for deferred upload
    void setNeedsVoiceSample(bool needs) {
        std::lock_guard<std::mutex> lock(mutex_);
        needsVoiceSample = needs;
    }

    bool getNeedsVoiceSample() {
        std::lock_guard<std::mutex> lock(mutex_);
        return needsVoiceSample;
    }

    void setVoiceSamplePath(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        voiceSamplePath = path;
    }

    std::string getVoiceSamplePath() {
        std::lock_guard<std::mutex> lock(mutex_);
        return voiceSamplePath;
    }

    // Conversation cooldown tracking
    void setConversationEnded() {
        std::lock_guard<std::mutex> lock(mutex_);
        conversationEndedTime = std::chrono::high_resolution_clock::now();
    }

    bool hasConversationCooldown() const {
        std::lock_guard<std::mutex> lock(mutex_);
        extern int GlobalEndConversationCooldown;
        auto now = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - conversationEndedTime);
        return elapsed.count() < GlobalEndConversationCooldown;
    }

private:
    RE::Actor* resolveActorUnsafe() {
        if (formID == 0) {
            return actor;
        }

        auto actorForm = RE::TESForm::LookupByID(formID);
        actor = actorForm ? actorForm->As<RE::Actor>() : nullptr;
        return actor;
    }

    RE::Actor* actor = nullptr;
    RE::TESObjectREFR* attackTarget = nullptr;
    mutable std::mutex mutex_;
    bool commandBusy = false;
    bool waitingForRes = false;
    bool talking = false;
    bool inAnimation = false;
    bool readingBook = false;
    bool cleaned = true;
    int restored = true;
    bool available = true;
    bool narrator = false;
    bool deleteMark = false;
    bool wasOnScene = false;
    bool manuallyAdded = false;
    int farAwayCounter = 0;
    int missingPresenceCounter = 0;
    int forgetCleanCounter = 0;
    int boredEventsFired = 0;
    bool externalLocked = false;
    bool needsVoiceSample = false;
    std::string voiceSamplePath;

    RE::BGSVoiceType* originalVoice;
    RE::TESObjectREFR* currentTarget;
    RE::FormID formID;
    std::string currentCommand;
    std::string currentAnimation;
    std::string name;
    std::chrono::steady_clock::time_point lastAccessTime = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastTimeTalk = std::chrono::steady_clock::now();
    std::chrono::high_resolution_clock::time_point conversationEndedTime;
};

class AIAgentManager {
public:
    static AIAgentManager& getInstance() {
        static AIAgentManager instance;  // Static instance of AIAgentManager
        return instance;
    }

    std::shared_ptr<AIAgent> createAgent() {
        return std::make_shared<AIAgent>();  // Create an instance of AIAgent using the default constructor
    }

    void addAgent(std::shared_ptr<AIAgent> &agent) {
        std::lock_guard<std::mutex> lock(mutex_);
        agents.push_back(agent);
    }

    /*
    std::vector<std::shared_ptr<AIAgent>>& getAgents() {
        std::lock_guard<std::mutex> lock(mutex_);
        return agents;
    }
    */

    std::vector<std::shared_ptr<AIAgent>> getAgents() {
        std::lock_guard<std::mutex> lock(mutex_);
        return agents;
    }

    std::shared_ptr<AIAgent> getAgentByName(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);

        // Convert the input name to lower case for case-insensitive comparison
        // Trim leading and trailing whitespace from the input name
        std::string lowerName = name;
        lowerName.erase(0, lowerName.find_first_not_of(" \t\n\r\f\v"));
        lowerName.erase(lowerName.find_last_not_of(" \t\n\r\f\v") + 1);

        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                       [](unsigned char c) { return std::tolower(c); });


        auto it = std::find_if(agents.begin(), agents.end(), [&lowerName](const std::shared_ptr<AIAgent>& agent) {
            std::string agentName(agent->getActorName());
            

            // Convert the agent's name to lower case for comparison
            std::transform(agentName.begin(), agentName.end(), agentName.begin(),
                           [](unsigned char c) { return std::tolower(c); });

            //logger::info("Searching agent by name <{}> <{}>", agentName, lowerName);

            // Patch RealNames Extended. Some LLMs are refering to generic NPCs without the brackets.
            
            if (agentName != lowerName) {
                auto actor = agent->getActorByFormId();
                if (actor) {
                    auto baseActor = actor->GetActorBase();
                    if (baseActor) {
                        std::string fullname = lowerName + " [" + baseActor->GetName() + "]";
                        std::transform(fullname.begin(), fullname.end(), fullname.begin(),
                                       [](unsigned char c) { return std::tolower(c); });

                        if (agentName == fullname) {
                            logger::debug("Matched {} after extending name {}: {}", agentName, lowerName, fullname);
                            return true;
                        }
                    }
                }
            }

            return agentName == lowerName;
        });

        if (it != agents.end()) {
            return *it;  // Return the found agent
        } else {
            return nullptr;  // Return nullptr if agent with given name is not found
        }
    }


    std::shared_ptr<AIAgent> getAgentByFormId(RE::FormID formId) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = std::find_if(agents.begin(), agents.end(), [&formId](const std::shared_ptr<AIAgent>& agent) {
            
            RE::FormID actorFormId = agent->GetFormId();

            return actorFormId == formId;
        });

        if (it != agents.end()) {
            return *it;  // Return the found agent
        } else {
            return nullptr;  // Return nullptr if agent with given name is not found
        }
    }

    void deleteAgent(std::shared_ptr<AIAgent>& agentToDelete) {
        std::lock_guard<std::mutex> lock(mutex_);
        logger::info("Delete agent {}", agentToDelete->getActorName());
        agents.erase(std::remove(agents.begin(), agents.end(), agentToDelete), agents.end());
    }

    void deleteAgentByName(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);

        // Convert the input name to lower case for case-insensitive comparison
        std::string lowerName = name;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        auto it = std::find_if(agents.begin(), agents.end(), [&lowerName](const std::shared_ptr<AIAgent>& agent) {
            std::string agentName = agent->getActorName();

            // Convert the agent's name to lower case for comparison
            std::transform(agentName.begin(), agentName.end(), agentName.begin(),
                           [](unsigned char c) { return std::tolower(c); });

            return agentName == lowerName;  // Compare in a case-insensitive manner
        });

        if (it != agents.end()) {
            logger::info("Delete agent {}", (*it)->getActorName());
            agents.erase(it);  // Remove the agent from the vector
        }
    }

    std::vector<std::string> getAgentsNamesFollowing() {
        std::vector<std::shared_ptr<AIAgent>> localAgents;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            localAgents = agents;
        }

        std::vector<std::string> agentNames;

        std::string beings = InspectManagedAgents(RE::PlayerCharacter::GetSingleton()->AsReference(),
                                                  HERIKA_MAX_VISION_RANGE, ",", DISTANCE_ACTIVATING_NPC_OUT, false);
        for (const auto& agent : localAgents) {
            if (agent) {  // Check if the pointer is not null
                if (agent->isPresent(beings)) {
                    agentNames.push_back(agent->getActorName());
                }
            }
        }



        return agentNames;
    }

    void removeAllAgents() {
        std::lock_guard<std::mutex> lock(mutex_);
        agents.clear();  // Clear the vector of agents
    }

    std::shared_ptr<AIAgent> getRandomAgent() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (agents.empty()) {
            return nullptr;  // Return nullptr if agents vector is empty
        }

        // Generate a random index within the range of agents vector size
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, agents.size() - 1);
        int randomIndex = dis(gen);

        return agents[randomIndex];
    }

    /*
    std::shared_ptr<AIAgent> getRandomAgentNearby(std::string beings) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (agents.empty()) {
            return nullptr;  // Return nullptr if agents vector is empty
        }

        
        std::random_device rd;
        std::default_random_engine rng(rd());
        std::shuffle(agents.begin(), agents.end(), rng);
        int i = -1;
        for (const auto& agent : agents) {
            i++;
            if (!agent->isPresent(beings)) continue;
            if (!agent->isAvailableforDialog(AllowActorsOnScene)) continue;
            if (!agent->getActor()) continue;
            return agents[i];
        }

        return nullptr;
    }*/

    std::shared_ptr<AIAgent> getRandomAgentNearby(const std::string& beings) {
        std::vector<std::shared_ptr<AIAgent>> localAgents;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            localAgents = agents;
        }

        std::random_device rd;
        std::mt19937 rng(rd());
        std::shuffle(localAgents.begin(), localAgents.end(), rng);

        for (auto& agent : localAgents) {
            if (!agent) continue;
            if (!agent->isPresent(beings)) continue;
            if (!agent->isAvailableforDialog(AllowActorsOnScene)) continue;
            if (!agent->getActor()) continue;
            return agent;
        }

        return nullptr;
    }


    std::shared_ptr<AIAgent> getLessBoredAgentNearby(const std::string& beings) {
        std::vector<std::shared_ptr<AIAgent>> localAgents;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            localAgents = agents;
        }

        std::vector<std::shared_ptr<AIAgent>> candidates;
        for (const auto& agent : localAgents) {
            if (!agent) continue;
            if (!agent->isPresent(beings)) continue;
            if (!agent->isAvailableforDialog(false)) continue;
            if (!agent->getActor()) continue;
            candidates.push_back(agent);
        }

        if (candidates.empty()) return nullptr;

        const auto leastBored = std::min_element(
            candidates.begin(), candidates.end(),
            [](const std::shared_ptr<AIAgent>& a, const std::shared_ptr<AIAgent>& b) {
                return a->getBoredEventsFired() < b->getBoredEventsFired();
            });
        const auto minimumBoredEvents = (*leastBored)->getBoredEventsFired();

        std::vector<std::shared_ptr<AIAgent>> leastBoredCandidates;
        for (const auto& agent : candidates) {
            if (agent->getBoredEventsFired() == minimumBoredEvents) {
                leastBoredCandidates.push_back(agent);
            }
        }

        std::random_device rd;
        std::mt19937 rng(rd());
        std::uniform_int_distribution<std::size_t> distribution(0, leastBoredCandidates.size() - 1);
        return leastBoredCandidates[distribution(rng)];
    }

    // Other member functions...

    void setPlayerName(std::string name) {
        std::lock_guard<std::mutex> lock(mutex_);
        playerName.assign(name);
    }

    std::string getPlayerName() {
        std::lock_guard<std::mutex> lock(mutex_);
        return playerName;
    }

    void addRenamedNpc(RE::FormID formId, const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it =
            std::find_if(renamedNpcs.begin(), renamedNpcs.end(),
                         [formId](const std::pair<RE::FormID, std::string>& entry) { return entry.first == formId; });
        if (it == renamedNpcs.end()) {
            renamedNpcs.emplace_back(formId, name);
        } else {
            it->second = name;
        }
    }

    void removeRenamedNpcByFormId(RE::FormID formId) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it =
            std::find_if(renamedNpcs.begin(), renamedNpcs.end(),
                         [formId](const std::pair<RE::FormID, std::string>& entry) { return entry.first == formId; });
        if (it != renamedNpcs.end()) {
            renamedNpcs.erase(it);
        }
    }

    // Returns a copy of the renamedNpcs list (thread-safe)
    std::vector<std::pair<RE::FormID, std::string>> getRenamedNpcs() const {
        
        return renamedNpcs;
    }

    void resetRenamedNpcs() {
        std::lock_guard<std::mutex> lock(mutex_);
        renamedNpcs.clear();
    }
    // Returns the renamed NPC name for a given FormID, or an empty string if not found
    std::string getRenamedNpcNameByFormId(RE::FormID formId)  {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it =
            std::find_if(renamedNpcs.begin(), renamedNpcs.end(),
                         [formId](const std::pair<RE::FormID, std::string>& entry) { return entry.first == formId; });
        if (it != renamedNpcs.end()) {
            return it->second;
        }
        return "";
    }


private:
    AIAgentManager() {}                              // Private constructor to enforce singleton pattern
    std::mutex mutex_;                               // Mutex to ensure thread safety
    std::vector<std::shared_ptr<AIAgent>> agents;  // Vector to hold instances of AIAgent
    std::string playerName;
    std::vector<std::pair<RE::FormID, std::string>> renamedNpcs;
};
#endif  // AIAGENT_H

#ifndef LOCATIONLIST_H
    #define LOCATIONLIST_H
class LocationList {
public:
    using Iterator = std::unordered_map<std::string, RE::TESObjectREFR*>::iterator;
    using ConstIterator = std::unordered_map<std::string, RE::TESObjectREFR*>::const_iterator;

    static LocationList& GetInstance() {
        static LocationList instance;
        return instance;
    }

    inline void AddLocation(const std::string& name, RE::TESObjectREFR* location) {
        std::lock_guard<std::mutex> lock(mutex);
        locationMap[name] = location;
    }

    inline void RemoveLocation(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex);
        locationMap.erase(name);
    }

     inline void Clear() {
        std::lock_guard<std::mutex> lock(mutex);
        locationMap.clear();
    }

    RE::TESObjectREFR* GetLocation(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex);
        // auto it = locationMap.find(name);
        // return it != locationMap.end() ? it->second : nullptr;
        for (const auto& pair : locationMap) {
            if (pair.first.find(name) != std::string::npos) {
                return pair.second;
            }
        }

        return nullptr;
    }

    Iterator begin() { return locationMap.begin(); }

    ConstIterator begin() const { return locationMap.begin(); }

    Iterator end() { return locationMap.end(); }

    ConstIterator end() const { return locationMap.end(); }

private:
    LocationList() = default;
    ~LocationList() = default;
    LocationList(const LocationList&) = delete;
    LocationList& operator=(const LocationList&) = delete;

    std::unordered_map<std::string, RE::TESObjectREFR*> locationMap;
    std::mutex mutex;
};

#endif

#ifndef NPCLIST_H
    #define NPCLIST_H
class NPCList {
public:
    using Iterator = std::unordered_map<std::string, RE::TESObjectREFR*>::iterator;
    using ConstIterator = std::unordered_map<std::string, RE::TESObjectREFR*>::const_iterator;

    static NPCList& GetInstance() {
        static NPCList instance;
        return instance;
    }

    inline void AddNPC(const std::string& name, RE::TESObjectREFR* npc) {
        std::lock_guard<std::mutex> lock(mutex);
        npcMap[name] = npc;
    }

    inline void RemoveNPC(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex);
        npcMap.erase(name);
    }

    RE::TESObjectREFR* GetNPC(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = npcMap.find(name);
        return it != npcMap.end() ? it->second : nullptr;
    }

    Iterator begin() { return npcMap.begin(); }

    ConstIterator begin() const { return npcMap.begin(); }

    Iterator end() { return npcMap.end(); }

    ConstIterator end() const { return npcMap.end(); }

private:
    NPCList() = default;
    ~NPCList() = default;
    NPCList(const NPCList&) = delete;
    NPCList& operator=(const NPCList&) = delete;

    std::unordered_map<std::string, RE::TESObjectREFR*> npcMap;
    std::mutex mutex;
};



#endif

