#include <iostream>
#include <vector>
#include <Windows.h>
#include <mmdeviceapi.h>
#include <mmsystem.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <fstream>
#include <sstream>
#include <WinInet.h>
#include <iomanip>
#include <sstream>
#include "Misc.h"
#include "HTTPUploader.h"
#include "HTTPManager.h"
#include "AudioManager.h"
#include "SpeakManager.h"
#include "Globals.h"
#include "Conf.h"
#include "KeyMap.h"
#include "Commands.h"
#include "PrismaUIBridge.h"
#include "SPGResponse.h"
#include "ThreadPool.h"

#include "json.hpp"
using json = nlohmann::json;

#pragma comment(lib, "Wininet.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")

namespace logger = SKSE::log;

extern std::chrono::high_resolution_clock::time_point controlLastBoredTriggerTS;
extern std::chrono::high_resolution_clock::time_point controlPlayerSpeechSuppressUntilTS;

namespace {
    void HardStopDialogueForPlayerVoiceInput() {
        logger::info("[VOICERECORD] Hard-stopping active dialogue before recording");

        const auto now = std::chrono::high_resolution_clock::now();
        controlLastBoredTriggerTS = now;
        controlPlayerSpeechSuppressUntilTS = now + std::chrono::seconds(10);
        PrismaUIBridge::BumpDialogueStopGeneration();

        SpeakManager& speakManager = SpeakManager::getInstance();
        speakManager.setLastUsedTime();
        speakManager.abortPendingUtterances("player_interrupt");
        speakManager.deleteQueue();
        speakManager.deleteQueuedPlayerLines();
        if (speakManager.getProcessing()) {
            logger::warn("[VOICERECORD] SpeakManager was still processing during hard stop; forcing processing state clear");
            speakManager.abortPlay(true);
            speakManager.setProcessing(false);
        }
        speakManager.stopRechatForNseconds(3);

        SPGResponse::getInstance().clearAllQueues();

        ThreadPool::getInstance().cancelTasksByType("HTTPStream");
        ThreadPool::getInstance().cancelTasksByType("HTTPStreamRechat");
    }

    WAVEFORMATEX CreateRecordingWaveFormat() {
        WAVEFORMATEX wfx = {};
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = 1;
        wfx.nSamplesPerSec = 16000;
        wfx.wBitsPerSample = 16;
        wfx.nBlockAlign = wfx.wBitsPerSample * wfx.nChannels / 8;
        wfx.nAvgBytesPerSec = wfx.nBlockAlign * wfx.nSamplesPerSec;
        return wfx;
    }

    std::string WideToUtf8(const wchar_t* wideText) {
        if (!wideText || wideText[0] == L'\0') {
            return "";
        }

        const int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wideText, -1, nullptr, 0, nullptr, nullptr);
        if (sizeNeeded <= 1) {
            return "";
        }

        std::string result(static_cast<size_t>(sizeNeeded), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wideText, -1, result.data(), sizeNeeded, nullptr, nullptr);
        result.resize(static_cast<size_t>(sizeNeeded - 1));
        return result;
    }

    std::string GetWaveMapperRecordingDeviceName() {
        const WAVEFORMATEX wfx = CreateRecordingWaveFormat();

        HWAVEIN waveInHandle = nullptr;
        MMRESULT openResult = waveInOpen(&waveInHandle, WAVE_MAPPER, &wfx, NULL, NULL, CALLBACK_NULL | WAVE_FORMAT_DIRECT);
        if (openResult != MMSYSERR_NOERROR) {
            openResult = waveInOpen(&waveInHandle, WAVE_MAPPER, &wfx, NULL, NULL, CALLBACK_NULL);
        }
        if (openResult != MMSYSERR_NOERROR) {
            logger::warn("[VOICERECORD] Failed to open mapper capture device for fallback name lookup: {}", static_cast<int>(openResult));
            return "Unavailable";
        }

        UINT deviceId = 0;
        MMRESULT getIdResult = waveInGetID(waveInHandle, &deviceId);
        if (getIdResult != MMSYSERR_NOERROR) {
            logger::warn("[VOICERECORD] Failed to resolve fallback capture device id: {}", static_cast<int>(getIdResult));
            waveInClose(waveInHandle);
            return "Unavailable";
        }

        WAVEINCAPSW caps = {};
        MMRESULT capsResult = waveInGetDevCapsW(deviceId, &caps, sizeof(caps));
        waveInClose(waveInHandle);
        if (capsResult != MMSYSERR_NOERROR) {
            logger::warn("[VOICERECORD] Failed to query fallback capture device caps for id {}: {}", deviceId, static_cast<int>(capsResult));
            return "Unavailable";
        }

        std::string deviceName = WideToUtf8(caps.szPname);
        if (deviceName.empty()) {
            return "Unavailable";
        }

        return deviceName;
    }
}


const int SILENCE_THRESHOLD = 500;  // Adjust this value based on your needs

std::string GetCurrentRecordingDeviceName() {
    HRESULT initResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize = SUCCEEDED(initResult);
    if (FAILED(initResult) && initResult != RPC_E_CHANGED_MODE) {
        logger::warn("[VOICERECORD] Failed to initialize COM for endpoint name lookup: {}", static_cast<unsigned long>(initResult));
        return GetWaveMapperRecordingDeviceName();
    }

    IMMDeviceEnumerator* deviceEnumerator = nullptr;
    IMMDevice* endpoint = nullptr;
    IPropertyStore* propertyStore = nullptr;
    PROPVARIANT friendlyName;
    PropVariantInit(&friendlyName);

    std::string deviceName = "Unavailable";

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&deviceEnumerator));
    if (SUCCEEDED(hr)) {
        hr = deviceEnumerator->GetDefaultAudioEndpoint(eCapture, eMultimedia, &endpoint);
    }
    if (SUCCEEDED(hr)) {
        hr = endpoint->OpenPropertyStore(STGM_READ, &propertyStore);
    }
    if (SUCCEEDED(hr)) {
        hr = propertyStore->GetValue(PKEY_Device_FriendlyName, &friendlyName);
    }
    if (SUCCEEDED(hr) && friendlyName.vt == VT_LPWSTR) {
        std::string endpointName = WideToUtf8(friendlyName.pwszVal);
        if (!endpointName.empty()) {
            deviceName = endpointName;
        }
    } else {
        logger::warn("[VOICERECORD] Failed to resolve default capture endpoint friendly name: {}", static_cast<unsigned long>(hr));
        deviceName = GetWaveMapperRecordingDeviceName();
    }

    PropVariantClear(&friendlyName);
    if (propertyStore) {
        propertyStore->Release();
    }
    if (endpoint) {
        endpoint->Release();
    }
    if (deviceEnumerator) {
        deviceEnumerator->Release();
    }
    if (shouldUninitialize) {
        CoUninitialize();
    }

    return deviceName;
}


const wchar_t* StringToWideString(std::string& str) {
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], static_cast<int>(str.size()), NULL, 0);
    wchar_t* buffer = new wchar_t[size_needed + 1];
    MultiByteToWideChar(CP_UTF8, 0, &str[0], static_cast<int>(str.size()), buffer, size_needed);
    buffer[size_needed] = L'\0';
    const wchar_t* wide_str = buffer;
    return wide_str;
}

bool startsWithDRAGON(const std::string& msg) {
    std::string upperMsg = msg;
    std::string target = "INVOKING";

    // Convert the message to uppercase
    std::transform(upperMsg.begin(), upperMsg.end(), upperMsg.begin(), [](unsigned char c) { return std::toupper(c); });

    // Check if the message starts with "DRAGON"
    if (upperMsg.size() >= target.size()) {
        return upperMsg.compare(0, target.size(), target) == 0;
    }

    return false;
}

std::string removeWhitespaces(const std::string& str) {
    std::string result = str;
    result.erase(std::remove_if(result.begin(), result.end(), [](unsigned char c) { return std::isspace(c); }),
                 result.end());
    return result;
}

// Function to check if a word exists in a string, case-insensitive and whitespace-stripped
bool containsWord(const std::string& msg, const std::string& word) {
    // Remove all white spaces from both strings
    std::string cleanedMsg = removeWhitespaces(msg);
    std::string cleanedWord = removeWhitespaces(word);

    // Convert both strings to uppercase
    std::string upperMsg = cleanedMsg;
    std::string upperWord = cleanedWord;

    std::transform(upperMsg.begin(), upperMsg.end(), upperMsg.begin(), [](unsigned char c) { return std::toupper(c); });
    std::transform(upperWord.begin(), upperWord.end(), upperWord.begin(),
                   [](unsigned char c) { return std::toupper(c); });

    // Check if the word is found in the cleaned message
    return upperMsg.find(upperWord) != std::string::npos;
}

std::string makeSTT(std::string wavData) {
    if (wavData.empty()) {
        logger::error("makeSTT received empty wav data");
        return "";
    }

    if (!Conf::getInstance().isOk()) {
        logger::error("AIAgent.ini file not present or invalid");
        RE::DebugNotification("AIAgent.ini file not present or invalid");
        return "";
    }

    logger::info("Sending audio to server (size: {} bytes)", wavData.size());
    HTTPUploader &uploader = HTTPUploader::getInstance();

    std::string buffer = uploader.UploadFile(wavData);

    if (buffer.empty()) {
        logger::error("No response received from server, empty audio or STT service error");
        return "";
    }

    logger::info("Response received from STT service (size: {} bytes)", buffer.size());

    controlPlayerSpeechSuppressUntilTS = std::chrono::high_resolution_clock::now() + std::chrono::seconds(10);

    auto player = RE::PlayerCharacter::GetSingleton();
    logger::debug("Processing response and gathering context information...");
    
    auto result = InspectLocations(player->AsReference());

    char timeDateString[200];
    RE::Calendar::GetSingleton()->GetTimeDateString(timeDateString, 200, true);

    HTTPManager::log(std::format("infoloc|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                 "(Context location: " + std::string(GetPlayerLocation()) + ", Buildings to go:" +
                                     result + ", Current Date in Skyrim World: " + timeDateString + ")"));

    std::string type;
    
    std::string typeRevised;


    if (type.empty())
        if (player->IsSneaking())
            typeRevised.assign("inputtext_s");
        else
            typeRevised.assign("inputtext");
     else
        typeRevised.assign(type);

     /* If not is animation busy, some plugin said shen can't call functions atm. To be revised*/
     /*
     if (typeRevised == "inputext" || typeRevised == "inputtext_s") {
        if (AIAgent::getInstance().getAnimationBusy()) typeRevised.assign("chatnf");
     }
     */

    if (startsWithDRAGON(buffer)) {
        bool found = false;
        RE::TESForm *spellFire;
        if (containsWord(buffer, "FLAMES")) {
            spellFire = RE::TESForm::LookupByID(0x012FCD);
            found = true;
        
        } else if (containsWord(buffer, "FIREBOLT")) {
            spellFire = RE::TESForm::LookupByID(0x012FD0);
            found = true;
        } else if (containsWord(buffer, "HEAL MY SELF")) {
            spellFire = RE::TESForm::LookupByID(0x012FCC);
            found = true;
        } else if (containsWord(buffer, "GREAT FIRE") || containsWord(buffer, "FIREBALL")) {
            spellFire = RE::TESForm::LookupByID(0x01C789);
            found = true;
        } else if (containsWord(buffer, "ICE SPIKE")) {
            spellFire = RE::TESForm::LookupByID(0x02B96C);
            found = true;
        } else if (containsWord(buffer, "FROST BITE")) {
            spellFire = RE::TESForm::LookupByID(0x02B96B);
            found = true;
        } else if (containsWord(buffer, "LIGHTNING BOLT")) {
            spellFire = RE::TESForm::LookupByID(0x02DD29);
            found = true;
        } else if (containsWord(buffer, "SPARKS")) {
            spellFire = RE::TESForm::LookupByID(0x02DD2A);
            found = true;
        } else if (containsWord(buffer, "FAST HEAL")) {
            spellFire = RE::TESForm::LookupByID(0x02F3B8);
            found = true;
        }

        if (found) {
            auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
            auto args = RE::MakeFunctionArguments(std::move(spellFire->GetFormID()));
            RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall(
                "AIAgentAIMind", "EquipSpellOnPlayer", args, callback);

        }
        return "";
    }
    
     HTTPManager::stream(std::format("{}|{}|{}|{}:{}", typeRevised, getCurrentTimeMillis(), GetGameTimeStamp(),
                                    RE::PlayerCharacter::GetSingleton()->GetName(), buffer));
     SpeakManager::getInstance().deleteQueue();

     AIAgentManager& aiam = AIAgentManager::getInstance();
     json sData;
     sData["speaker"] = RE::PlayerCharacter::GetSingleton()->GetName();
     sData["location"] = GetPlayerLocation();
     sData["speech"] = buffer;
     sData["listener"] = "#HERIKA_NPC1#";
     sData["companions"] = aiam.getAgentsNamesFollowing();

     HTTPManager::log(std::format("_speech|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(), sData.dump()));

    //AIAgent::getInstance().setWaitingForRes(true);
    
    auto originalName = aiam.getPlayerName();
    if (originalName == "Prisoner") {
        originalName = RE::PlayerCharacter::GetSingleton()->GetName();
        aiam.setPlayerName(originalName);
    }
    RE::PlayerCharacter::GetSingleton()->SetDisplayName(originalName.c_str(), true);

    return buffer;
}


int VoiceRecordThread(int bindedKey) {
    // Fill the WAVEFORMATEX struct to indicate the format of our recorded audio
    WAVEFORMATEX wfx = CreateRecordingWaveFormat();

    // Open the 'waveIn' recording device
    HWAVEIN wi;
    waveInOpen(&wi, WAVE_MAPPER, &wfx, NULL, NULL, CALLBACK_NULL | WAVE_FORMAT_DIRECT);

    // Create buffers for audio data
    const int BUFFER_SIZE = 16000 * 2 * 2 / 8;  // Half a second of audio data
    char buffers[8][BUFFER_SIZE] = {};
    WAVEHDR headers[8] = {};

    // Initialize the headers and add them to the queue
    for (int i = 0; i < 8; ++i)
    {
        headers[i].lpData = buffers[i];
        headers[i].dwBufferLength = BUFFER_SIZE;
        waveInPrepareHeader(wi, &headers[i], sizeof(headers[i]));
        waveInAddBuffer(wi, &headers[i], sizeof(headers[i]));
    }

    // Start recording
    waveInStart(wi);

    logger::info("[VOICRECORD] Now recording audio. ");

    HardStopDialogueForPlayerVoiceInput();

    bool saveAudio = false;
    std::vector<short> audioData;
    DWORD startTime = timeGetTime();
    signed long  silenceTime = -500;

    // Get the key mapping and translate the Skyrim key code to Windows VK code
    std::unordered_map<int, int> keyMapping = PopulateKeyMapping();
    int windowsKeyCode;

    // First try direct mapping from Skyrim key code to Windows VK code
    auto it = keyMapping.find(bindedKey);
    if (it != keyMapping.end()) {
        windowsKeyCode = it->second;
        logger::debug("Using mapped key code: {} -> {}", bindedKey, windowsKeyCode);
    } else {
        // If no mapping found, try to map common virtual keys
        switch(bindedKey) {
            case 256: windowsKeyCode = VK_LBUTTON; break;  // Left mouse button
            case 257: windowsKeyCode = VK_RBUTTON; break;  // Right mouse button
            case 258: windowsKeyCode = VK_MBUTTON; break;  // Middle mouse button
            case 264: windowsKeyCode = VK_PRIOR; break;    // Mouse wheel up
            case 265: windowsKeyCode = VK_NEXT; break;     // Mouse wheel down
            default:
                // For unmapped keys, try using the key code directly if it's in a reasonable range
                if (bindedKey >= 0 && bindedKey < 256) {
                    windowsKeyCode = bindedKey;
                    logger::debug("Using direct key code: {}", bindedKey);
                } else {
                    logger::error("Unsupported key code: {}", bindedKey);
                    windowsKeyCode = bindedKey;  // Fall back to original code
                }
        }
    }

    // Check if key was initially pressed
    bool wasInitiallyPressed = (GetAsyncKeyState(windowsKeyCode) & 0x8000) != 0;
    logger::debug("Key initially pressed: {}", wasInitiallyPressed);

    while ((silenceTime < 4000) && ((timeGetTime() - startTime) < 60000))
    {
        // Only check key release if it was initially pressed
        if (wasInitiallyPressed && !(GetAsyncKeyState(windowsKeyCode) & 0x8000) && (REL::Module::GetRuntime() != REL::Module::Runtime::VR)) {
            logger::debug("Recording stopped - key released");
            break;
        }

        // Check if headers are done
        if (!VoiceRecordControl::getInstance().getRecording())
            break;

        for (auto& h : headers)
        {
            if (h.dwFlags & WHDR_DONE)
            {
                if (!saveAudio)
                {
                    // First time a header is done, set the saveAudio flag to start saving data
                    saveAudio = true;
                    logger::info("Audio data available");
                }

                // Append the recorded audio data to the vector
                short* data = reinterpret_cast<short*>(h.lpData);
                size_t numSamples = h.dwBytesRecorded / sizeof(short);
                for (size_t i = 0; i < numSamples; ++i)
                {
                    audioData.push_back(data[i]);
                }

                // Check for silence
                bool silent = true;
                for (size_t i = 0; i < numSamples; ++i)
                {
                    if (abs(data[i]) > SILENCE_THRESHOLD)
                    {
                        silent = false;
                        break;
                    }
                }

                if (silent)
                {
                    silenceTime += 250;
                }
                else
                {
                    // Reset the silence timer if there is audio above the threshold
                    silenceTime = 0;
                }
            }

            // Re-add the header to the queue
            waveInPrepareHeader(wi, &h, sizeof(h));
            waveInAddBuffer(wi, &h, sizeof(h));
        }

        // Sleep for a short duration to avoid excessive CPU usage
        Sleep(10);
    }

    // Stop recording and clean up
    waveInStop(wi);
    for (auto& h : headers)
    {
        waveInUnprepareHeader(wi, &h, sizeof(h));
    }
    waveInClose(wi);

    // Write the accumulated audio data to a WAV file
    //
    std::stringstream outputFile;
    logger::info("About to create wav file");

    if (outputFile)
    {
        // Write the WAV file header
        // RIFF chunk descriptor
        const char* riffHeader = "RIFF";
        outputFile.write(riffHeader, 4);

        // File size (temporary value)
        DWORD fileSize = 0;
        outputFile.write(reinterpret_cast<const char*>(&fileSize), 4);

        // WAVE format
        const char* waveHeader = "WAVE";
        outputFile.write(waveHeader, 4);

        // Format subchunk
        const char* formatHeader = "fmt ";
        outputFile.write(formatHeader, 4);

        DWORD fmtSize = sizeof(wfx);
       

        outputFile.write(reinterpret_cast<const char*>(&fmtSize), 4);

        outputFile.write(reinterpret_cast<const char*>(&wfx), sizeof(wfx));

        // Data subchunk
        const char* dataHeader = "data";
        outputFile.write(dataHeader, 4);

        // Data size
        DWORD dataSize = audioData.size() * sizeof(short);
        outputFile.write(reinterpret_cast<const char*>(&dataSize), 4);

        // Write the audio data
        outputFile.write(reinterpret_cast<const char*>(audioData.data()), dataSize);

        // Update the file size in the WAV header
        fileSize = dataSize + 36;
        outputFile.seekp(4);
        outputFile.write(reinterpret_cast<const char*>(&fileSize), 4);

        // Calculate audio duration in seconds (16000 Hz sample rate, 16-bit samples, mono)
        float audioDurationSecs = static_cast<float>(dataSize) / (16000.0f * sizeof(short));
        
        if (audioDurationSecs < 0.5f) {
            logger::info("Audio too short: {:.2f} seconds", audioDurationSecs);
            return 0;
        }

        if (fileSize < 32000) {
            logger::info("Audio file too small: {} bytes", fileSize);
            return 0;
        }

        std::string data = outputFile.str();

        /*
        // Write the data to outputFile2
        std::ofstream outputFile2("recorded_audio.wav", std::ios::binary);

        outputFile2.write(data.c_str(), data.size());

        // Close the output file
        outputFile2.close();
        */
        //std::cout << "Audio recording saved as 'recorded_audio.wav'." << std::endl;
        logger::info("Audio recording saved in memory.");
        
        makeSTT(data);
        VoiceRecordControl::getInstance().setRecording(false);  // Reset recording control
    }
    else
    {
        std::cout << "Failed to open output file." << std::endl;
    }

    return 0;
}

int VoiceRecord(int bindedKey) {
    ThreadPool::getInstance().enqueue("VoiceRecord", [bindedKey]() { 
        VoiceRecordThread(bindedKey);
        logger::info("Voice thread ended");
    }, "", std::chrono::seconds(60));
    return 0;
}
