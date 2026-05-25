#include "MusicManager.h"

#include <Windows.h>
#include <WinInet.h>
#include <winhttp.h>
#include <mmsystem.h>
#include <iostream>
#include <filesystem>
#include <chrono>
#include <thread>
#include "AudioManager.h"
#include "HTTPManager.h"
#include "Misc.h"
#include "Conf.h"
#include "md5.h"
#include "json.hpp"
#include "SpeakManager.h"


#pragma comment(lib, "Wininet.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "winmm.lib")

namespace logger = SKSE::log;
using json = nlohmann::json;
extern const wchar_t* StringToWideString(std::string& str);
extern RE::TESFaction *AIAgentRoleMasterFaction;

typedef unsigned long DWORD;
typedef unsigned char BYTE;
typedef unsigned int DWORD_;
typedef short SHORT;
typedef BYTE* LPBYTE;

typedef struct __WAVEDESCR {
    BYTE riff[4];
    DWORD_ size;
    BYTE wave[4];
} _WAVEDESCR, *_LPWAVEDESCR;

typedef struct __WAVEFORMAT {
    BYTE id[4];
    DWORD_ size;
    SHORT format;
    SHORT channels;
    DWORD_ sampleRate;
    DWORD_ byteRate;
    SHORT blockAlign;
    SHORT bitsPerSample;
} _WAVEFORMAT, *_LPWAVEFORMAT;



MusicManager* MusicManager::instance = nullptr;

MusicManager::MusicManager() : isPlaying(false), stopped(true), paused(false) {}

MusicManager::~MusicManager() {
    stopSong();
    if (playThread.joinable()) playThread.join();
}

MusicManager& MusicManager::getInstance() {
    if (!instance) {
        instance = new MusicManager();
    }
    return *instance;
}

bool MusicManager::downloadSong(const std::string& songName) {
    // Download JSON
    HINTERNET hSession = WinHttpOpen(L"WinHTTP Example/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!hSession) {
        logger::error("[MusicManager] Failed to open WinHTTP session. Error: {}", GetLastError());
        return false;
    }

    auto server = Conf::getInstance().getServer();
    auto port = std::stoi(Conf::getInstance().getPort());
    std::wstring wideServer = StringToWideString(server);

    HINTERNET hConnect = WinHttpConnect(hSession, wideServer.c_str(), port, 0);
    if (!hConnect) {
        logger::error("[MusicManager] Failed to connect to server. Error: {}", GetLastError());
        WinHttpCloseHandle(hSession);
        return false;
    }

    std::string jsonPath = "/music/" + songName + ".json";
    std::wstring wideJsonPath = StringToWideString(jsonPath);

    HINTERNET hRequestJson = WinHttpOpenRequest(hConnect, L"GET", wideJsonPath.c_str(), NULL, WINHTTP_NO_REFERER,
                                                WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_REFRESH);
    if (!hRequestJson) {
        logger::error("[MusicManager] Failed to create JSON HTTP request: {}", GetLastError());
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    if (!WinHttpSendRequest(hRequestJson, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        logger::error("[MusicManager] Failed to send JSON HTTP request: {}", GetLastError());
        WinHttpCloseHandle(hRequestJson);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    if (!WinHttpReceiveResponse(hRequestJson, NULL)) {
        logger::error("[MusicManager] Failed to receive JSON HTTP response: {}", GetLastError());
        WinHttpCloseHandle(hRequestJson);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD jsonContentLength = 0;
    DWORD lengthSize = sizeof(jsonContentLength);
    if (!WinHttpQueryHeaders(hRequestJson, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, NULL, &jsonContentLength,
                             &lengthSize, NULL)) {
        logger::error("[MusicManager] Failed to query JSON content length: {}", GetLastError());
        WinHttpCloseHandle(hRequestJson);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    std::vector<char> jsonBuffer(jsonContentLength);
    DWORD totalBytesRead = 0;
    DWORD bytesRead = 0;
    while (WinHttpReadData(hRequestJson, jsonBuffer.data() + totalBytesRead, jsonContentLength - totalBytesRead, &bytesRead) &&
           bytesRead > 0) {
        totalBytesRead += bytesRead;
    }

    if (totalBytesRead != jsonContentLength) {
        logger::error("[MusicManager] Incomplete JSON download");
        WinHttpCloseHandle(hRequestJson);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    WinHttpCloseHandle(hRequestJson);

    std::string jsonStr(jsonBuffer.begin(), jsonBuffer.end());
    json j = json::parse(jsonStr);
    currentSong.audio_file = j["audio_file"];

    try {
        currentSong.alignments.clear();
        for (auto& a : j["alignments"]) {
            Alignment al;
            al.start_time = a["start_time"];
            al.end_time = a["end_time"];
            al.duration = a["duration"];
            al.transcribed = a["transcribed"];
            al.matched_lyrics = a["matched_lyrics"];
            al.relative_power = a["relative_power"];
            al.sentiment = a["sentiment"];
            al.animation1 = a["animation1"];
            al.animation2 = a["animation2"];
            currentSong.alignments.push_back(al);
        }

        currentSong.predominance.clear();
        for (auto& p : j["predominance"]) {
            Predominance pr;
            pr.start_time = p["start_time"];
            pr.end_time = p["end_time"];
            pr.predominant_stem = p["predominant_stem"];
            pr.secondary_stem = p["secondary_stem"];
            pr.bpms = p["bpms"];
            pr.powers = p["powers"];
            currentSong.predominance.push_back(pr);
        }
    } catch (const std::exception& e) {
        logger::error("[MusicManager] Failed to parse JSON: {}", e.what());
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // Download audio
    std::string audioPath = "/music/" + songName + ".wav";
    std::wstring wideAudioPath = StringToWideString(audioPath);

    HINTERNET hRequestAudio = WinHttpOpenRequest(hConnect, L"GET", wideAudioPath.c_str(), NULL, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_REFRESH);
    if (!hRequestAudio) {
        logger::error("[MusicManager] Failed to create audio HTTP request: {}", GetLastError());
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    if (!WinHttpSendRequest(hRequestAudio, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        logger::error("[MusicManager] Failed to send audio HTTP request: {}", GetLastError());
        WinHttpCloseHandle(hRequestAudio);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    if (!WinHttpReceiveResponse(hRequestAudio, NULL)) {
        logger::error("[MusicManager] Failed to receive audio HTTP response: {}", GetLastError());
        WinHttpCloseHandle(hRequestAudio);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD audioContentLength = 0;
    lengthSize = sizeof(audioContentLength);
    if (!WinHttpQueryHeaders(hRequestAudio, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, NULL, &audioContentLength,
                             &lengthSize, NULL)) {
        logger::error("[MusicManager] Failed to query audio content length: {}", GetLastError());
        WinHttpCloseHandle(hRequestAudio);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    currentSong.audioBuffer.resize(audioContentLength);
    totalBytesRead = 0;
    bytesRead = 0;
    while (WinHttpReadData(hRequestAudio, currentSong.audioBuffer.data() + totalBytesRead, audioContentLength - totalBytesRead, &bytesRead) &&
           bytesRead > 0) {
        totalBytesRead += bytesRead;
    }

    if (totalBytesRead != audioContentLength) {
        logger::error("[MusicManager] Incomplete audio download");
        WinHttpCloseHandle(hRequestAudio);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    currentSong.audioSize = audioContentLength;

    // Calculate duration
    _LPWAVEFORMAT waveHeader = reinterpret_cast<_LPWAVEFORMAT>(currentSong.audioBuffer.data() + sizeof(_WAVEDESCR));
    int32_t audioDataSize = currentSong.audioSize - waveHeader->size;
    int32_t sampleRate = waveHeader->sampleRate;
    int16_t channels = waveHeader->channels;
    int16_t bitsPerSample = waveHeader->bitsPerSample;
    currentSong.duration = (double)audioDataSize / (sampleRate * channels * (bitsPerSample / 8.0));

    WinHttpCloseHandle(hRequestAudio);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    logger::info("[MusicManager] Downloaded song: {}", songName);
    return true;
}


// This will play song in a separate thread and update lyrics and camera based on the song's progress. It will also
// handle pausing when the game is paused.
// we assme NPCs are already spawned and ready to perform when this is called, and that the singer is one of them. 
// NPcs will be selected by name.
// * Bass: Dov McMackagern
// * Other (guitar1): Skaar Slashborn    
// * Other (guitar2):Izran Stalhrim
// * Drums: Morth Sorum
// We also assume that the song is already downloaded and currentSong is populated.
void MusicManager::playSong(AIAgent *singer) {
    std::lock_guard<std::mutex> lock(mtx);
    if (isPlaying) return;
    isPlaying = true;
    stopped = false;
    paused = false;
    logger::info("[MusicManager] Starting playback thread for song: {}", currentSong.audio_file);
    playThread = std::thread([this, singer]() {
        bool soundPlaying = false;
        bool soundStarted = false;
        auto start = std::chrono::steady_clock::now();
        std::string lastLyrics = "";
        std::string lastStem = "";
        auto lastCamChanged = std::chrono::steady_clock::now()-std::chrono::seconds(5);
        auto lastAnimCalled = std::chrono::steady_clock::now();
        auto& am = AudioManagerController::GetInstance();
        if (am.LoadWAV(reinterpret_cast<BYTE*>(currentSong.audioBuffer.data()), currentSong.audioBuffer.size())) {
            float currentVolume = am.defaultVolume;
            am.setVolume(currentVolume * 100.0f);
            
        }
        
        AIAgentManager& aiam = AIAgentManager::getInstance();
        RE::Actor *singerActor = singer->getActorByFormId();

        while (!stopped) {
            // Pause handing
            bool gamePaused = RE::UI::GetSingleton()->GameIsPaused();
            if (gamePaused) 
                paused = true;
            else
                paused = false;
            // Handle pause/resume

            if (!paused) {
                if (!soundPlaying) {
                    if (soundStarted) {
                        am.Resume();
                        soundPlaying = true;
                    } else if (am.Play()) {
                        soundPlaying = true;
                        soundStarted = true;
                    } else {
                        logger::error("[MusicManager] AudioManagerController Failed to play WAV");
                    }
                }
                auto now = std::chrono::steady_clock::now();
                double elapsed = am.GetElapsedTimeSeconds();
                auto totalElapsed = elapsed;
                elapsed = fmod(elapsed, currentSong.duration);
                if (!am.isPlaying())
                    stopped = true;

                LyricInfo lyrics = getCurrentLyrics(elapsed);
                if (lyrics.text != lastLyrics) {
                    if (!this->jusTrim(lyrics.text).empty()) {
                        SpeakManager::getInstance().deleteQueue();
                        if (SpeakManager::getInstance().getProcessing()) SpeakManager::getInstance().abortPlay();

                        std::string lyricsExpanded = lyrics.text;
                       /* if (!lyrics.text.empty()) {
                            lyricsExpanded = lyrics.text[0];
                            for (size_t i = 1; i < lyrics.text.size(); ++i) {
                                lyricsExpanded += 'A';
                                lyricsExpanded += lyrics.text[i];
                            }
                        }*/

                        std::string expresion = getSentiment(elapsed);
                        std::string animation;
                        auto relativePower = getRelativePower(elapsed);

                        // Function to generate a random float between 0.5 and 3.0
                        static std::mt19937 gen(std::random_device{}());
                        std::uniform_real_distribution<float> dist(0.5f, 3.0f);
                        relativePower = dist(gen);

                        if (relativePower > 2.5)
                            animation = "IdleMagic_01";
                        else if (relativePower > 2)
                            animation = "IdleCiceroDance2";
                        else if (relativePower > 1.5)
                            animation = "IdleRitualSpellStart";
                        else if (relativePower > 1)
                            animation = "IdleDialogueWelcomeHandGesture";
                        else
                            animation = "IdleRitualSkull3";

                        if (lyrics.animation1.empty() == false) 
                            animation = lyrics.animation1;
                        ScriptLine lyricline(lyrics.text, expresion, "", animation, singer->getActorName(), lyrics.text,
                                             1,
                                             lyrics.end_time - lyrics.start_time);
                        SpeakManager::getInstance().insertInQueue(lyricline);

                       lastAnimCalled = now;

                        // Log here to event 
                        HTTPManager::log(std::format(
                            "chat|{}|{}|(Context location: {}){}: sings \"{}\"", getCurrentTimeMillis(),
                            GetGameTimeStamp(), GetPlayerLocation(), singer->getActorName(), lyrics.text));
                        
                    }
                    lastLyrics = lyrics.text;
                }

                std::string currentStem=getPredominantStem(elapsed);
                if (currentStem != lastStem) {
                    
                    if (currentStem == "vocals") {
                        if (now - lastAnimCalled > std::chrono::seconds(5)) {

                            auto relativePower = getRelativePower(elapsed);
                            
                            bool bypass = true;
                            auto actor = singer->getActorByFormId();

                            int lyricLength = static_cast<int>(lyrics.duration);
                            std::string emotion = getSentiment(elapsed);

                            if (lastAnimCalled + std::chrono::seconds(lyricLength) > now)
                                emotion = "reanim";

                            
                            if (actor) {
                                auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                                auto args = RE::MakeFunctionArguments(std::move(actor), std::move(relativePower),
                                                                      std::move(lyricLength), std::move(emotion));
                                RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                                    "AIAgentNpcUtil", "SwordsAndNirnsFrontman", args, callback);
                            } 
                            lastStem = currentStem;
                            
                        }
                    } else {
                        if (now-lastCamChanged < std::chrono::seconds(5)) {
                            // Do nothing
                        } else {
                            lastCamChanged = now;
                            if (currentStem == "bass") {
                                
                                float bpm = getBPMsFor(elapsed, "bass");
                                auto agent = aiam.getAgentByName("Dov McKagarn");
                                if (agent) {
                                    agent->setExternalLocked(true);
                                    auto actor = agent->getActorByFormId();
                                    if (actor) {
                                        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                                        int mode = 1;
                                        bool bypass = true;
                                        std::string animation = "";
                                        auto args = RE::MakeFunctionArguments(std::move(actor), std::move(singerActor),
                                                                              std::move(mode), std::move(bpm), std::move(animation));
                                        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                                            "AIAgentNpcUtil", "PlaceMusicCam", args, callback);
                                    }
                                    lastStem = currentStem;
                                }
                            } else if (currentStem == "drums") {
                                auto agent = aiam.getAgentByName("Morth Sorum");
                                if (agent) {
                                    float bpm = getBPMsFor(elapsed, "drums");
                                    agent->setExternalLocked(true);
                                    auto actor = agent->getActorByFormId();
                                    if (actor) {
                                        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                                        int mode = 2;
                                        bool bypass = true;
                                        std::string animation = "";
                                        auto args = RE::MakeFunctionArguments(std::move(actor), std::move(singerActor),
                                                                              std::move(mode), std::move(bpm), std::move(animation));
                                        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                                            "AIAgentNpcUtil", "PlaceMusicCam", args, callback);
                                    }
                                    lastStem = currentStem;
                                }
                            } else if (currentStem == "other") {
                                auto agent = aiam.getAgentByName("Skaar Slashborn");
                                if (agent) {
                                    float bpm = getBPMsFor(elapsed, "other");
                                    agent->setExternalLocked(true);
                                    auto actor = agent->getActorByFormId();
                                    if (actor) {
                                        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                                        int mode = 3;
                                        bool bypass = true;
                                        std::string animation;
                                        auto args = RE::MakeFunctionArguments(std::move(actor), std::move(singerActor),
                                                                              std::move(mode), std::move(bpm), std::move(animation));
                                        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                                            "AIAgentNpcUtil", "PlaceMusicCam", args, callback);
                                    }
                                    lastStem = currentStem;
                                }
                                agent = aiam.getAgentByName("Izran Stalhrim");
                                if (agent) {
                                    
                                    float bpm = getBPMsFor(elapsed, "other");
                                    agent->setExternalLocked(true);
                                    auto actor = agent->getActorByFormId();
                                    if (actor) {
                                        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                                        int mode = 4;
                                        bool bypass = true;
                                        std::string animation;
                                        auto args = RE::MakeFunctionArguments(std::move(actor), std::move(singerActor),
                                                                              std::move(mode), std::move(bpm), std::move(animation));
                                        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                                            "AIAgentNpcUtil", "PlaceMusicCam", args, callback);
                                    }
                                }
                            }
                        }
                    }
                    
                } else {
                    if (currentStem == "vocals") {
                        if (now - lastAnimCalled > std::chrono::seconds(6)) {
                            auto relativePower = getRelativePower(elapsed);
                            bool bypass = true;
                            auto actor = singer->getActorByFormId();
                            if (actor) {
                                int lyricLength = static_cast<int>(lyrics.duration);
                                std::string emotion = getSentiment(elapsed);
                                float power = 0.0f;
                                if (lastAnimCalled + std::chrono::seconds(lyricLength) > now) power = 6;
                                
                                std::string animation;
                                 // Function to generate a random float between 0.5 and 3.0
                                static std::mt19937 gen(std::random_device{}());
                                std::uniform_real_distribution<float> dist(0.5f, 3.0f);
                                relativePower = dist(gen);

                                if (relativePower > 2.5)
                                    animation = "IdleMagic_01";
                                else if (relativePower > 2)
                                    animation = "IdleCiceroDance2";
                                else if (relativePower > 1.5)
                                    animation = "IdleRitualSpellStart";
                                else if (relativePower > 1)
                                    animation = "IdleDialogueWelcomeHandGesture";
                                else
                                    animation = "IdleRitualSkull3";

                                if (lyrics.animation2.empty() == false) animation = lyrics.animation2;


                                auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                                auto args = RE::MakeFunctionArguments(std::move(actor), std::move(singerActor),
                                                                      std::move(0), std::move(power), std::move(animation));
                                RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                                    "AIAgentNpcUtil", "PlaceMusicCam", args, callback);
                            }
                            
                            lastAnimCalled = now;
                        }
                    } else {
                        // To avoid cam fixed too much on predominant stem. if the stem is the same as before, we can
                        // call anim every 10 seconds to make it more dynamic using secondary stem info. If the
                        // predominant stem changed, we will change cam immediately and call anim immediately, but if
                        // the predominant stem is the same as before, we will only call anim if the secondary stem is
                        // different and 10 seconds have passed since last anim call.
                        if (now - lastCamChanged > std::chrono::seconds(10)) {
                            std::string secondaryStem = getSecondaryStem(elapsed);
                            if (secondaryStem == "bass") {
                                float bpm = getBPMsFor(elapsed, "bass");
                                auto agent = aiam.getAgentByName("Dov McKagarn");
                                if (agent) {
                                    agent->setExternalLocked(true);
                                    auto actor = agent->getActorByFormId();
                                    if (actor) {
                                        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                                        int mode = 1;
                                        bool bypass = true;
                                        std::string animation;
                                        auto args = RE::MakeFunctionArguments(std::move(actor), std::move(singerActor),
                                                                              std::move(mode), std::move(bpm), std::move(animation));
                                        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                                            "AIAgentNpcUtil", "PlaceMusicCam", args, callback);
                                    }
                                    lastStem = secondaryStem;
                                    
                                    lastCamChanged = now;
                                }
                            } else if (secondaryStem == "drums") {
                                auto agent = aiam.getAgentByName("Morth Sorum");
                                if (agent) {
                                    float bpm = getBPMsFor(elapsed, "drums");
                                    agent->setExternalLocked(true);
                                    auto actor = agent->getActorByFormId();
                                    if (actor) {
                                        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                                        int mode = 2;
                                        bool bypass = true;
                                        std::string animation;
                                        auto args = RE::MakeFunctionArguments(std::move(actor), std::move(singerActor),
                                                                              std::move(mode), std::move(bpm),
                                                                              std::move(animation));
                                        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                                            "AIAgentNpcUtil", "PlaceMusicCam", args, callback);
                                    }
                                    lastStem = secondaryStem;
                                    
                                    lastCamChanged = now;
                                }
                            } else if (secondaryStem == "other") {
                                auto agent = aiam.getAgentByName("Skaar Slashborn");
                                if (agent) {
                                    float bpm = getBPMsFor(elapsed, "other");
                                    agent->setExternalLocked(true);
                                    auto actor = agent->getActorByFormId();
                                    if (actor) {
                                        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                                        int mode = 3;
                                        bool bypass = true;
                                        std::string animation;
                                        auto args = RE::MakeFunctionArguments(std::move(actor), std::move(singerActor),
                                                                              std::move(mode), std::move(bpm),
                                                                              std::move(animation));
                                        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                                            "AIAgentNpcUtil", "PlaceMusicCam", args, callback);
                                    }
                                    lastStem = secondaryStem;
                                    
                                    lastCamChanged = now;
                                }
                                agent = aiam.getAgentByName("Izran Stalhrim");
                                if (agent) {
                                    float bpm = getBPMsFor(elapsed, "other");
                                    agent->setExternalLocked(true);
                                    auto actor = agent->getActorByFormId();
                                    if (actor) {
                                        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
                                        int mode = 4;
                                        bool bypass = true;
                                        std::string animation;
                                        auto args = RE::MakeFunctionArguments(std::move(actor), std::move(singerActor),
                                                                              std::move(mode), std::move(bpm), std::move(animation));
                                        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                                            "AIAgentNpcUtil", "PlaceMusicCam", args, callback);
                                    }
                                }
                            }
                        }
                    }
                }

                auto bpm = getBPM(elapsed);
                auto powers=getPowers(elapsed);
                if (powers.find("vocals") != powers.end()) {
                    float vocalPower = powers["vocals"];
                    
                }
                if (powers.find("bass") != powers.end()) {
                    float bassPower = powers["bass"];
                    auto agent = aiam.getAgentByName("Dov McKagarn");

                    if (agent) {
                        agent->setConversationEnded();
                        auto actor = agent->getActorByFormId();
                        if (actor) {
                            if (bassPower < 0.05) {
                                actor->EnableAI(false);
                            } else {
                                actor->EnableAI(true);
                            }
                            if (bpm > 140)
                                actor->AddToFaction(AIAgentRoleMasterFaction, 2);
                            else
                                actor->AddToFaction(AIAgentRoleMasterFaction, 1);
                        }
                    }

                }
                if (powers.find("drums") != powers.end()) {
                    float drumsPower = powers["drums"];
                    auto agent = aiam.getAgentByName("Morth Sorum");
                    //logger::info("drumsPower power: {}", drumsPower);
                    if (agent) {
                        agent->setConversationEnded();
                        auto actor = agent->getActorByFormId();
                        if (actor) {
                            if (drumsPower < 0.05) {
                                actor->EnableAI(false);
                            } else {
                                actor->EnableAI(true);
                            }
                            if (bpm > 150)
                                actor->AddToFaction(AIAgentRoleMasterFaction, 2);
                            else
                                actor->AddToFaction(AIAgentRoleMasterFaction, 1);
                        }
                    }
                }
                if (powers.find("other") != powers.end()) {
                    float guitarPower = powers["other"];
                    auto agent = aiam.getAgentByName("Skaar Slashborn");
                    
                    if (agent) {
                        agent->setConversationEnded();
                        auto actor = agent->getActorByFormId();
                        if (actor) {
                            if (guitarPower < 0.05) {
                                actor->EnableAI(false);
                            } else {
                                actor->EnableAI(true);
                            }

                            if (bpm > 110)
                                actor->AddToFaction(AIAgentRoleMasterFaction, 2);
                            else
                                actor->AddToFaction(AIAgentRoleMasterFaction, 1);
                        }
                    }

                    agent = aiam.getAgentByName("Izran Stalhrim");
                    if (agent) {
                        agent->setConversationEnded();
                        auto actor = agent->getActorByFormId();
                        if (actor) {
                            if (guitarPower < 0.05) {
                                actor->EnableAI(false);
                            } else {
                                actor->EnableAI(true);
                            }

                            if (bpm > 110)
                                actor->AddToFaction(AIAgentRoleMasterFaction, 2);
                            else
                                actor->AddToFaction(AIAgentRoleMasterFaction, 1);
                        }
                    }
                }
               

            } else {
                if (soundPlaying) {
                    am.Pause();
                    // Add 100 milliseconds to the start time to account for the pause duration
                    start += std::chrono::milliseconds(100);
                    soundPlaying = false;
                }
            }
            // AudioManager::Update only uses heading + listener position here.
            // Avoid querying GetLookingAtLocation() in this playback loop.
            auto headingAngle = RE::PlayerCharacter::GetSingleton()->GetAngleZ();

            am.Update(
                AudioManager::ConvertNiPoint3ToX3DAUDIO_VECTOR(RE::PlayerCharacter::GetSingleton()->GetPosition()),
                AudioManager::ConvertNiPoint3ToX3DAUDIO_VECTOR(RE::PlayerCharacter::GetSingleton()->GetPosition()),
                headingAngle);


            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (soundPlaying) {
            am.Stop();
        }
        isPlaying = false;
        logger::info("[MusicManager] Stopped playing song");
        
        auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
        auto actor = singer->getActor();
        auto args = RE::MakeFunctionArguments(std::move(actor));
        RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
            "AIAgentNpcUtil", "SwordsAndNirnsStop", args, callback);
    });
}

void MusicManager::stopSong() {
    std::lock_guard<std::mutex> lock(mtx);
    stopped = true;
    paused = false;
    if (playThread.joinable()) {
        playThread.join();
    }
    auto& am = AudioManagerController::GetInstance();
    am.Stop();
    isPlaying = false;
    
}

void MusicManager::pauseSong() {
    std::lock_guard<std::mutex> lock(mtx);
    paused = true;
}

void MusicManager::resumeSong() {
    std::lock_guard<std::mutex> lock(mtx);
    paused = false;
}

LyricInfo MusicManager::getCurrentLyrics(double time) {
    for (const auto& al : currentSong.alignments) {
        if (time >= al.start_time && time < al.end_time) {
            return {al.matched_lyrics, al.start_time, al.end_time, al.duration, al.animation1, al.animation2};
        }
    }
    return {"", 0.0, 0.0, 0.0, "", ""};
}

std::string MusicManager::getPredominantStem(double time) {
    for (const auto& pr : currentSong.predominance) {
        if (time >= pr.start_time && time < pr.end_time) {
            return pr.predominant_stem;
        }
    }
    return "";
}

float MusicManager::getBPM(double time) {
    for (const auto& pr : currentSong.predominance) {
        if (time >= pr.start_time && time < pr.end_time) {
            return pr.bpms.at(pr.predominant_stem);
        }
    }
    return 0.0f;
}

std::map<std::string, float> MusicManager::getBPMs(double time) {
    for (const auto& pr : currentSong.predominance) {
        if (time >= pr.start_time && time < pr.end_time) {
            return pr.bpms;
        }
    }
    return {};
}



float MusicManager::getRelativePower(double time) {
    for (const auto& al : currentSong.alignments) {
        if (time >= al.start_time && time < al.end_time) {
            return al.relative_power;
        }
    }
    return 0.0f;
}

std::map<std::string, float> MusicManager::getPowers(double time) {
    for (const auto& pr : currentSong.predominance) {
        if (time >= pr.start_time && time < pr.end_time) {
            return pr.powers;
        }
    }
    return {};
}

// Utility function to trim whitespace and newlines from a string
std::string MusicManager::jusTrim(const std::string& input) {
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

float MusicManager::getBPMsFor(double time, std::string stem) {
    for (const auto& pr : currentSong.predominance) {
        if (time >= pr.start_time && time < pr.end_time) {
            auto it = pr.bpms.find(stem);
            if (it != pr.bpms.end()) {
                return it->second;
            }
            return 0.0f;
        }
    }
    return 0.0f;
}

std::string MusicManager::getSecondaryStem(double time) {
    for (const auto& pr : currentSong.predominance) {
        if (time >= pr.start_time && time < pr.end_time) {
            return pr.secondary_stem;
        }
    }
    return "";
}

std::string MusicManager::getSentiment(double time) {
    for (const auto& al : currentSong.alignments) {
        if (time >= al.start_time && time < al.end_time) {
            return al.sentiment;
        }
    }
    return "";
}
