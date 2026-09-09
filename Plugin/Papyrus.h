#pragma once

#include "PlayerConversationRouter.h"

extern bool sttBindedKey;

struct LocationKeywordInfo {
    RE::FormID formID;
    std::string_view tag;
};

#ifndef LOCATION_KEYWORDS_H
#define LOCATION_KEYWORDS_H

constexpr std::array<LocationKeywordInfo, 36> kLocationKeywords{
    {{0x000130EF, "Cave"},          {0x000130DB, "Dungeon"},       {0x0001CB87, "Inn"},
     {0x00013166, "Town"},          {0x00013168, "City"},          {0x00016771, "Hold"},
     {0x00018EF0, "Farm"},          {0x00018EF1, "Mine"},          {0x0001CD59, "Jail"},
     {0x0001CD5B, "Ship"},          {0x0001CB85, "House"},         {0x0001CB86, "Store"},
     {0x0001CD5A, "Guild"},         {0x0001CD56, "Temple"},        {0x0001CD57, "Castle"},
     {0x000130F2, "Nordic Ruin"},   {0x000130DC, "Dwelling"},      {0x000130DF, "Bandit Camp"},
     {0x000130E0, "Dragon Lair"},   {0x000130E4, "Falmer Hive"},   {0x000130F0, "Dwarven Ruin"},
     {0x00013167, "Settlement"},    {0x00018EF2, "Lumber Mill"},   {0x00039793, "Habitation"},
     {0x000130E2, "Draugr Crypt"},  {0x000130EB, "Vampire Lair"},  {0x000130EC, "Warlock Lair"},
     {0x000130E7, "Military Fort"}, {0x000130E8, "Military Camp"}, {0x000130ED, "Werewolf Lair"},
     {0x000130EE, "Forsworn Camp"}, {0x000130E5, "Giant Camp"},    {0x000130DE, "Animal Den"},
     {0x0001CD58, "Cemetery"},      {0x0001929F, "Shipwreck"},     {0x000FC1A3, "Player House"}}};

inline std::string_view GetLocationTag(RE::FormID formID) {
    for (const auto& e : kLocationKeywords) {
        if (e.formID == formID) {
            return e.tag;
        }
    }

    return {};
}

#endif  // LOCATION_KEYWORDS_H
		
int sendMessageReal(
    std::string msg,
    std::string type,
    const PlayerConversationRoutingContext& routingContext = {});

// Open mic helper functions
void triggerOpenMicRecording();
void triggerOpenMicRecordingEnd();
void openMicMonitoringLoop();

namespace Papyrus {
    namespace logger = SKSE::log;
    // Mutex to ensure only one Papyrus function executes at a time
    extern std::mutex papyrusMutex;

    // Helper class for mutex logging
    class ScopedPapyrusLock {
    public:
        explicit ScopedPapyrusLock(const char* functionName) : _functionName(functionName) {
            // logger::debug("Papyrus::{}  - Acquiring mutex", _functionName);
            papyrusMutex.lock();
            // logger::debug("Papyrus::{} - Mutex acquired", _functionName);
        }
        ~ScopedPapyrusLock() {
            papyrusMutex.unlock();
            // logger::debug("Papyrus::{} - Mutex released", _functionName);
        }

    private:
        const char* _functionName;
    };

    int sendMessage(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                    std::string msg, std::string type);

    int sendMessageToActor(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                           RE::StaticFunctionTag*, std::string msg, std::string type, RE::Actor* targetActor);

    int commandEnded(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                     std::string commmand);

    int commandEndedForActor(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                             RE::StaticFunctionTag*, std::string commmand, std::string npc);

    int getHerikaFormId(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);

    int recordSoundEx(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                      int bindedKey);
    int stopRecording(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                      int bindedKey);
    int stopAllDialogue(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);
    bool isGameFocused(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);

    int startOpenMicMonitoring(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                               RE::StaticFunctionTag*);
    int stopOpenMicMonitoring(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                              RE::StaticFunctionTag*);
    int setOpenMicMuted(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                        bool muted);
    std::string getCurrentRecordingDeviceName(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                               RE::StaticFunctionTag*);

    int beginChimMcmSnapshot(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                             RE::StaticFunctionTag*);
    int publishChimMcmEntry(
        RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
        std::string page, std::string section, std::string key, std::string label, std::string description,
        std::string type, std::string value, std::string options);
    int commitChimMcmSnapshot(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                              RE::StaticFunctionTag*, int revision);
    int beginChimMcmAgents(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                           RE::StaticFunctionTag*);
    int publishChimMcmAgent(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                            RE::StaticFunctionTag*, std::string bucket, int formId, std::string name);
    int commitChimMcmAgents(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                            RE::StaticFunctionTag*);
    int publishChimMcmCommandResult(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                    RE::StaticFunctionTag*, std::string request, bool ok, std::string message);

    bool RegisterSGPFuncs(RE::BSScript::IVirtualMachine* a_vm);

    int sendRequest(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);

    int logMessage(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                   std::string msg, std::string type);

    int logMessageForActor(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                           RE::StaticFunctionTag*, std::string msg, std::string type, std::string npc);

    int requestMessage(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                       std::string msg, std::string type);

    int requestMessageForActor(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                               RE::StaticFunctionTag*, std::string msg, std::string type, std::string npc);

    int setAnimationBusy(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                         int busy, std::string actor);

    int setLocked(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                  int busy, std::string actor);

    int isActorTalking(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                       RE::StaticFunctionTag*, std::string actor);

    int getPlayerBountyForGuard(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                RE::StaticFunctionTag*, std::string guardName);

    int requestMoveInventoryItemConfirmation(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                             RE::StaticFunctionTag*, RE::Actor* source, RE::Actor* target,
                                             RE::TESForm* itemForm, int amount, std::string realName);

    int requestArrestConfirmation(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                  RE::StaticFunctionTag*, RE::Actor* player, RE::Actor* guard,
                                  RE::TESFaction* crimeFaction);

    int hardResetExpression(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                            RE::StaticFunctionTag*);

    int shotAndUpload(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                      std::string hints, int mode);

    int startSoulgazeCapture(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                             RE::StaticFunctionTag*, std::string hints, int captureType, int renderMode,
                             RE::Actor* target);

    int isGameVR(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);

    int setConf(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                std::string code, float f_Value, int i_value, std::string s_value);

    int setConfReal(std::string code, float f_Value, int i_value, std::string s_value);

    int setDrivenByAI(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);

    int setNewActionMode(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                         int mode);

    RE::Actor* getClosestAgent(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                               RE::StaticFunctionTag*);

    std::vector<RE::Actor*> findAllNearbyAgents(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                                RE::StaticFunctionTag*);
    std::vector<RE::Actor*> findAllAgents(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                          RE::StaticFunctionTag*);
    std::vector<RE::Actor*> findAllNearbyNonAgents(RE::BSScript::Internal::VirtualMachine* a_vm,
                                                   RE::VMStackID a_stackID, RE::StaticFunctionTag*);

    std::vector<RE::Actor*> findAllNearbyActors(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                                RE::StaticFunctionTag*, bool onlyBgL);


    int sendFactionFast(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                        RE::TESFaction* faction, std::string name);

    int sendNPCFast(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                         RE::Actor* ActorList);

    RE::TESObjectREFR* findLocationsToSafeSpawn(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                                RE::StaticFunctionTag*, float distance, bool restriction);

    RE::Actor* getAgentByName(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                              RE::StaticFunctionTag*, std::string npcName);

    int get_conf_i(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                   std::string code);

    int setDrivenByAIA(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                       RE::Actor* forcedActor, bool salutation);

    int removeAgentByName(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                          std::string name);

    int addBasicProfile(RE::BSScript::IVirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                                 RE::Actor* target);
    RE::TESObjectREFR* getLocationMarkerFor(RE::BSScript::IVirtualMachine* a_vm, RE::VMStackID a_stackID,
                                            RE::StaticFunctionTag*, RE::BGSLocation* a_loc);

    RE::TESObjectREFR* getWorldLocationMarkerFor(RE::BSScript::IVirtualMachine* a_vm, RE::VMStackID a_stackID,
                                                 RE::StaticFunctionTag*, RE::BGSLocation* a_loc);

    RE::TESObjectREFR* getLocationCenterMarker(RE::BSScript::IVirtualMachine* a_vm, RE::VMStackID a_stackID,
                                                        RE::StaticFunctionTag*, RE::BGSLocation* a_loc, int modifier);

    int scanActorsAroundOffline(RE::BSScript::IVirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                                RE::Actor* target);

    int updateRemoteInventory(RE::BSScript::IVirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                                RE::Actor* target);

    int updateRemoteCombatSnapshot(RE::BSScript::IVirtualMachine* a_vm, RE::VMStackID a_stackID,
                                   RE::StaticFunctionTag*, RE::Actor* target);

    RE::TESObjectREFR* getNearestDoor(RE::BSScript::IVirtualMachine* a_vm, RE::VMStackID a_stackID,
                                      RE::StaticFunctionTag*);

    int sendAllVoices(RE::BSScript::IVirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);

    int setAIKeyWord(RE::BSScript::IVirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                     RE::Actor* target);

    int testAddAllNPCAround(RE::BSScript::IVirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);

    int testRemoveAll(RE::BSScript::IVirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);

    int recipeManager(RE::BSScript::IVirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*, RE::FormID);

    int isUsingFurniture(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                         RE::Actor* actor);

    int getInt(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
               std::string key, std::string jsonStr);
    RE::FormID getForm(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                       std::string key, std::string jsonStr);
    std::string getString(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                          std::string key, std::string jsonStr);
    float getFloat(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                   std::string key, std::string jsonStr);

    RE::Actor* getActor(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                 RE::StaticFunctionTag*, std::string key, std::string jsonStr);

    RE::TESObjectREFR* getReference(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                            RE::StaticFunctionTag*,
                        std::string key, std::string jsonStr);

    RE::BGSListForm* getFormList(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                 RE::StaticFunctionTag*, std::string key, std::string jsonStr);

    RE::TESEffectShader* getEffectShader(RE::BSScript::Internal::VirtualMachine* a_vm,
                                                      RE::VMStackID a_stackID,
                                 RE::StaticFunctionTag*, std::string key, std::string jsonStr);

    int SayTo(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
              RE::Actor* source, RE::Actor* target, RE::TESForm* topic);

    int isInContainer(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                               RE::StaticFunctionTag*, RE::TESObjectREFR* item);

    std::string GetDoorActivationText(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                      RE::StaticFunctionTag*, RE::TESObjectREFR* ref);

    RE::TESObjectREFR* loadReference(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                  int FormId);

    int PostGameData(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                     RE::StaticFunctionTag*, std::string jsondata);


    int sendLocationFast(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                         RE::BGSLocation* a_loc, std::string tags, RE::TESObjectCELL *referenceCell);

    // Prisma UI History Panel functions
    int toggleHistoryPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);
    int showHistoryPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);
    int hideHistoryPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);
    int focusHistoryPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*, bool pauseGame);
    int unfocusHistoryPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);
    int toggleHistoryPanelFocus(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*, bool pauseGame);
    
    int toggleOverlayPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);
    int cycleOverlayStatusPanels(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);
    int cycleHistoryDiariesPanels(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);
    int showOverlayPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);
    int hideOverlayPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);
    
    int toggleDiariesPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);
    int showDiariesPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);
    int hideDiariesPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);
    
    int toggleBrowserPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);
    int showBrowserPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);
    int hideBrowserPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);
    
    int toggleAIViewPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);
    int showAIViewPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);
    int hideAIViewPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);
    
    int toggleDebuggerPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);
    int showDebuggerPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);
    int hideDebuggerPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);

    // Status HUD panel
    int toggleStatusHUDPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);
    int showStatusHUDPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);
    int hideStatusHUDPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);

    // Chatbox panel
    int toggleChatboxPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);
    int showChatboxPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);
    int hideChatboxPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);
    int focusChatboxPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);
    int unfocusChatboxPanel(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);
    int isChatboxPanelVisible(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);
    int isChatboxPanelFocused(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);
    int isAnyPrismaHotkeyPanelFocused(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);

    std::vector<RE::FormID> findAllAgentsFormId(RE::BSScript::Internal::VirtualMachine* a_vm,
                                                         RE::VMStackID a_stackID, RE::StaticFunctionTag*);

    std::string GetLocationSpecialRefsString(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                             RE::StaticFunctionTag*, RE::FormID a_locationFormID);
    // Settings Menu panel
    int toggleSettingsMenu(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);
    std::string getSettingsMenuPendingAction(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);
    int clearSettingsMenuPendingAction(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);
    
    int toggleMasterMenu(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*);

    int startPlayerMenuDialogueTTS(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                   RE::StaticFunctionTag*, std::string fallbackText);
    int startPlayerMenuDialogueTTSNative(std::string fallbackText);
    void releasePlayerMenuTopicTimer(std::string reason);

    // Experiments

    int startMusicScene(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                         std::string songName, std::string singerName );
    int stopMusicScene(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID, RE::StaticFunctionTag*,
                        std::string singerName);

    int removeFromRenamedNPCList(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMStackID a_stackID,
                                 RE::StaticFunctionTag*, RE::Actor *actor);
}
    
