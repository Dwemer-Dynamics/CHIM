#include "Replacements.h"
#include "SPGResponse.h"



namespace logger = SKSE::log;


std::string ReplacementFor(std::string actorName, std::string originalString) {
    // AADTRWDYTA       AASPGDialogueHerika1WhatTopic
    // AADTRWWSD        AASPGDialogueHerika2Branch1Topic
    // AADTRWDYKATP     AASPGDialogueHerika3Branch1Topic
    // AADTRSOMETHING   AASPGQuestDialogue2Topic1B1Topic

    if (originalString.find("AADTRWDYTA") != std::string::npos) {  //  Response for what do you think about?

        SPGResponse& spgResponse = SPGResponse::getInstance();
        ResponseItem newResponse = spgResponse.getLastItem(actorName+"AASPGDialogueHerika1WhatTopic");

        // Check here for expiration

        if (newResponse.text.length() < 2) {
            newResponse.text = "I don't have much more to say";
        }

        return newResponse.text;
    } else if (originalString.find("AADTRWWSD") != std::string::npos) {  //  Response for What we should do?

        SPGResponse& spgResponse = SPGResponse::getInstance();
        ResponseItem newResponse = spgResponse.getLastItem(actorName+"AASPGDialogueHerika2Branch1Topic");

        // Check here for expiration

        if (newResponse.text.length() < 2) {
            newResponse.text = "Why do you ask me?";
        }

        return newResponse.text;
    } else if (originalString.find("AADTRWDYKATP") !=
               std::string::npos) {  //  Response for What do You know about this place?

        SPGResponse& spgResponse = SPGResponse::getInstance();
        ResponseItem newResponse = spgResponse.getLastItem(actorName+"AASPGDialogueHerika3Branch1Topic");

        // Check here for expiration

        if (newResponse.text.length() < 2) {
            newResponse.text = "What place?";
        }

        return newResponse.text;
    } else if (originalString.find("AADTRSOMETHING") !=
               std::string::npos) {  //  Herika's comments

        SPGResponse& spgResponse = SPGResponse::getInstance();
        ResponseItem newResponse = spgResponse.getFirstItem(actorName + "AASPGQuestDialogue2Topic1B1Topic"); 

        // Check here for expiration

        if (newResponse.text.length() < 2) {
            newResponse.text = "What a day!";
        }

        return newResponse.text;
    }

    return originalString;
}

void responsePop(std::string action) {
    SPGResponse& spgResponse = SPGResponse::getInstance();
    std::string qName = action;

    if (qName.compare("animation") == 0) {  // Standard queue
        spgResponse.dequeue(action);
        spgResponse.eraseOldItems(action);
    } else if (qName.compare("command") == 0) {  // Standard queue inversed
        spgResponse.dequeueFirst(action);
        spgResponse.eraseOldItems(action);
    } else if (qName.compare("rolecommand") == 0) {  // Standard queue inversed
        spgResponse.dequeueFirst(action);
        spgResponse.eraseOldItems(action);
    } else if (qName.compare("animationscripted") == 0) {  // Standard queue
        spgResponse.dequeue(action);
        spgResponse.eraseOldItems(action);
    } else if (qName.compare("ScriptQueue") == 0) {  // Standard queue
        spgResponse.dequeueFirst(action);
        spgResponse.eraseOldItems(action);
    
    }  else {  // Round robin behaviour
        spgResponse.eraseOldItems(action);
        spgResponse.moveFirstToLast(action);
    }

    //
  
}

void clearQueue(std::string action) {
    SPGResponse& spgResponse = SPGResponse::getInstance();
    std::string qName = action;

    spgResponse.clearQueue(action);

    

    //
}

void replace1(RE::TESObjectREFR* actor, RE::BGSSoundDescriptorForm *soundToReplace) {
    const auto mtManager = RE::MenuTopicManager::GetSingleton();
    RE::TESTopicInfo* currentDialog = mtManager->currentTopicInfo;

    const auto player = RE::PlayerCharacter::GetSingleton();

    if (!currentDialog) return;

    RE::DialogueItem it = currentDialog->GetDialogueData(player->As<RE::Actor>());
    for (RE::DialogueResponse* response : it.responses) {
        RE::BSString itt = response->text;

        std::string str1(itt.c_str());
        RE::BSString itt2(ReplacementFor(actor->GetName(), response->text.c_str()));
        response->text = itt2;
        //response->voiceSound=soundToReplace;
    }
}

void replace2(RE::TESObjectREFR* actor, RE::BGSSoundDescriptorForm* soundToReplace) {
    const auto mtManager = RE::MenuTopicManager::GetSingleton();
    RE::TESTopicInfo* currentDialog = mtManager->currentTopicInfo;

    // Dialogue replacement here

    RE::MenuTopicManager::Dialogue* dialogue = mtManager->lastSelectedDialogue;

    if (dialogue != nullptr) {
        for (RE::DialogueResponse* response : dialogue->responses) {
            RE::BSString itt = response->text;
            std::string str1(itt.c_str());
            RE::BSString itt2(ReplacementFor(actor->GetName(), response->text.c_str()));

            response->text = itt2;
           // response->voiceSound = soundToReplace;

            // logger::info("Substitution!");
        }
    }
}

void replace3() {
    const auto mtManager = RE::MenuTopicManager::GetSingleton();

    RE::BSSimpleList<RE::MenuTopicManager::Dialogue*>* dialogueList = mtManager->dialogueList;

    if (dialogueList) {
        for (auto iter = dialogueList->begin(); iter != dialogueList->end(); ++iter) {
            RE::MenuTopicManager::Dialogue* dialogueit = *iter;
            for (RE::DialogueResponse* response : dialogueit->responses) {
                RE::BSString itt = response->text;
                std::string str1(itt.c_str());

                RE::BSString itt2(ReplacementFor("", response->text.c_str()));
                response->text = itt2;
            }
        }
    }
}

void replace4() {
    const auto mtManager = RE::MenuTopicManager::GetSingleton();
    RE::TESTopicInfo* currentDialog = mtManager->currentTopicInfo;

    RE::TESObjectREFR* dialog_target = nullptr;

    if (mtManager->speaker && mtManager->speaker.get()) {
        dialog_target = mtManager->speaker.get().get();
    }

    if (dialog_target && currentDialog) {
        RE::DialogueItem it = currentDialog->GetDialogueData(dialog_target->As<RE::Actor>());
        for (RE::DialogueResponse* response : it.responses) {
            RE::BSString itt = response->text;
            std::string str1(itt.c_str());
            RE::BSString itt2(ReplacementFor("", response->text.c_str()));
            response->text = itt2;
        }
    }
}