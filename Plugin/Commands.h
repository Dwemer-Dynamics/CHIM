


class HerikaAnimGraphEventSink : public RE::BSTEventSink<RE::BSAnimationGraphEvent> {
public:
    virtual RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* a_event,
                                                  RE::BSTEventSource<RE::BSAnimationGraphEvent>* a_eventSource);
};



RE::TESObjectREFR* findActorInCell(std::string targetName, RE::TESObjectCELL* cell, RE::Actor* soourceActor,
                                   float radius, bool allowDead);
void parseCommand(std::string comamnd, std::string actorname);
void parseRoleCommand(std::string rawCommand);
void commandIdleLookFar();
bool commandAnimation(std::string anim,RE::Actor *npc);
void resetAnimation();

void StartAttack(std::string targetName, RE::Actor *actor,bool lethal);
void Follow(std::string targetName);
void StopCurrent(RE::Actor *npc);
void StartSneakTo(std::string targetName);
void EndCommand(std::string command, std::string actor);
void EndCommandError(std::string command, std::string actor);

RE::Actor* findClosestAgent();

    // Info request
std::string InspectLocations(RE::TESObjectREFR *reference);
std::string InspectSurroundings(RE::TESObjectREFR* reference, bool useCache, float visionRange, std::string separator,
                                 float farAwayLimit);
std::string InspectManagedAgents(RE::TESObjectREFR* reference, float visionRange, const std::string& separator,
                                 float farAwayLimit, bool includeNarrator = false);
std::string InspectAudibleActors(RE::TESObjectREFR* reference, bool useCache, float visionRange,
                                  std::string separator);
std::string InspectSurroundingsNavmesh(RE::TESObjectREFR* reference, bool useCache, float visionRange,
                                       std::string separator);
std::string InspectNearbyItems(RE::TESObjectREFR* reference, float visionRange);

std::string InspectSurroundingsInCell(RE::TESObjectCELL* cell, bool useCache);
RE::FormID findFurnitureInCell(RE::TESObjectCELL* cell,RE::Actor *npc,int mode);

// Inventory refresh function (defined in Plugin.cpp)
void RefreshAIAgentInventory(RE::Actor* npc, const std::string& agentName, bool forceUpdate, bool synchronous = false);
RE::Actor* ResolvePendingBarterMerchant(RE::Actor* fallbackMerchant);
void SetPendingBarterMerchant(RE::Actor* merchant);

