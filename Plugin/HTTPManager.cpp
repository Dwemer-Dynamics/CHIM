#include "HTTPManager.h"

#include <algorithm>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include "Commands.h"
#include "Conf.h"
#include "Globals.h"
#include "Misc.h"
#include "PrismaUIBridge.h"
#include "SpatialSnapshotManager.h"
#include "SPGResponse.h"
#include "SpeakManager.h"
#include "SpatialAwareness.h"
#include "ThreadPool.h"
#include "json.hpp"
#include "md5.h"
#include <cmath> // Include cmath for M_PI

using json = nlohmann::json;

#pragma comment(lib, "ws2_32.lib")

#define AGENT_MAX_DISTANCE_CHAT 2000

namespace logger = SKSE::log;

static bool EqualsIgnoreCaseHttp(const std::string& left, const std::string& right)
{
    if (left.size() != right.size()) {
        return false;
    }

    for (std::size_t i = 0; i < left.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(left[i])) !=
            std::tolower(static_cast<unsigned char>(right[i]))) {
            return false;
        }
    }

    return true;
}

static bool IsPlayerStreamActor(const std::string& actorName)
{
    const std::string normalizedActorName = trim(actorName);
    if (normalizedActorName.empty()) {
        return false;
    }

    if (EqualsIgnoreCaseHttp(normalizedActorName, "Player")) {
        return true;
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (player) {
        const std::string playerName = trim(player->GetName());
        if (!playerName.empty() && EqualsIgnoreCaseHttp(normalizedActorName, playerName)) {
            return true;
        }

        const std::string playerDisplayName = trim(player->GetDisplayFullName());
        if (!playerDisplayName.empty() && EqualsIgnoreCaseHttp(normalizedActorName, playerDisplayName)) {
            return true;
        }
    }

    const std::string configuredPlayerName = trim(AIAgentManager::getInstance().getPlayerName());
    return !configuredPlayerName.empty() && EqualsIgnoreCaseHttp(normalizedActorName, configuredPlayerName);
}

static void QueueInterruptNPC(RE::Actor* actor, std::shared_ptr<AIAgent> agent, const std::string& listener)
{
    if (!actor || !agent || listener == NARRATOR_NAME) {
        return;
    }

    auto actorHandle = actor->GetHandle();
    SKSE::GetTaskInterface()->AddTask([actorHandle, agent, listener]() {
        auto* resolvedActor = actorHandle.get().get() ? actorHandle.get().get()->As<RE::Actor>() : nullptr;
        if (!resolvedActor || resolvedActor->IsDead()) {
            logger::debug("[HTTPStream] Skipping interrupt for {}; actor no longer valid", listener);
            return;
        }

        struct SEHTranslatorScope {
            _se_translator_function previous;
            SEHTranslatorScope() : previous(_set_se_translator(SEHTranslator)) {}
            ~SEHTranslatorScope() { _set_se_translator(previous); }
        } sehScope;

        try {
            InterruptNPC(resolvedActor, agent.get());
        } catch (const std::exception& e) {
            logger::error("[HTTPStream] InterruptNPC failed for {}: {}", listener, e.what());
        }
    });
}

static const std::string base64_chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

extern bool NewActionMode;
extern int GlobalRechatPolicyAsap;
int GlobalConfiguredTimeout = 30;
std::string lastEventType = "";  // Track last event type for narration detection

bool IsInPlayerFOV(RE::Actor* npc, float detectionRadius) {
    // Validate NPC pointer
    if (!npc) {
        logger::error("IsInPlayerFOV: npc is nullptr");
        return false;
    }

    // Get the player singleton
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        logger::error("IsInPlayerFOV: Player is nullptr");
        return false;
    }

    // Get player and NPC positions using GetPosition
    const RE::NiPoint3 playerPos = player->GetPosition();
    const RE::NiPoint3 npcPos = npc->GetPosition();

    // Calculate distance between player and NPC
    float distance = playerPos.GetDistance(npcPos);
    logger::debug("IsInPlayerFOV: Calculated distance = {}", distance);

    // Check if NPC is within the detection radius
    if (distance > detectionRadius) {
        logger::debug("IsInPlayerFOV: NPC is outside detection radius (>{}).", detectionRadius);
        return false;
    }

    // Get player's forward vector using GetLookingAtLocation
    float yaw = player->GetAngleZ();  // Or player->GetAngleZ();
    RE::NiPoint3 playerForward(std::sin(yaw), std::cos(yaw), 0.0f);


    // Normalize the player's forward vector
    float forwardLength = playerForward.Length();
    if (forwardLength > 0.0f) {
        playerForward /= forwardLength;
    } else {
        logger::debug("IsInPlayerFOV: Player's forward vector is zero-length (invalid).");
        return false;
    }

    // Calculate vector from player to NPC
    RE::NiPoint3 toNpc = npcPos - playerPos;
    float length = toNpc.Length();
    logger::debug("IsInPlayerFOV: Vector to NPC: [{}, {}, {}] (length = {})", toNpc.x, toNpc.y, toNpc.z, length);

    // Normalize the toNpc vector
    if (length > 0.0f) {
        toNpc /= length;
    } else {
        logger::debug("IsInPlayerFOV: NPC is at the player's position.");
        return true;
    }

    // Calculate the dot product between player's forward vector and the vector to NPC
    float dot = playerForward.Dot(toNpc);
    dot = std::clamp(dot, -1.0f, 1.0f);

    // Define the field of view cosine value (45 degrees half-FOV = 90° total)
    constexpr float kFovCosine = 0.7071f;  // cos(45°)

    logger::debug("IsInPlayerFOV: Calculated dot = {} (kFovCosine = {})", dot, kFovCosine);

    // Check if NPC is within the field of view
    if (dot >= kFovCosine) {
        logger::debug("IsInPlayerFOV: NPC is IN FOV cone.");
        return true;
    } else {
        logger::debug("IsInPlayerFOV: NPC is OUTSIDE FOV cone.");
        return false;
    }
}



namespace HTTPManager {

    std::string EscapeJson(const std::string& input) {
        std::string out;
        out.reserve(input.size());

        for (char c : input) {
            switch (c) {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    out += c;
                    break;
            }
        }
        return out;
    }

    std::string base64_encode(const char* bytes_to_encode, size_t in_len) {
        std::stringstream ss;
        int i = 0;
        int j = 0;
        unsigned char char_array_3[3];
        unsigned char char_array_4[4];

        while (in_len--) {
            char_array_3[i++] = *(bytes_to_encode++);
            if (i == 3) {
                char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
                char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
                char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
                char_array_4[3] = char_array_3[2] & 0x3f;

                for (i = 0; (i < 4); i++) {
                    ss << base64_chars[char_array_4[i]];
                }
                i = 0;
            }
        }

        if (i) {
            for (j = i; j < 3; j++) {
                char_array_3[j] = '\0';
            }

            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;

            for (j = 0; (j < i + 1); j++) {
                ss << base64_chars[char_array_4[j]];
            }

            while ((i++ < 3)) {
                ss << '=';
            }
        }

        return ss.str();
    }

    std::string base64_decode(const std::string& encoded_string) {
        size_t in_len = encoded_string.size();
        int i = 0, j = 0;
        int in_ = 0;
        unsigned char char_array_4[4], char_array_3[3];
        std::string decoded_string;

        auto is_base64 = [](unsigned char c) -> bool { return (isalnum(c) || (c == '+') || (c == '/')); };

        while (in_len-- && (encoded_string[in_] != '=') && is_base64(encoded_string[in_])) {
            char_array_4[i++] = encoded_string[in_];
            in_++;
            if (i == 4) {
                for (i = 0; i < 4; i++) char_array_4[i] = base64_chars.find(char_array_4[i]);

                char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
                char_array_3[1] = ((char_array_4[1] & 0x0F) << 4) + ((char_array_4[2] & 0x3C) >> 2);
                char_array_3[2] = ((char_array_4[2] & 0x03) << 6) + char_array_4[3];

                for (i = 0; (i < 3); i++) decoded_string += char_array_3[i];

                i = 0;
            }
        }

        if (i) {
            for (j = i; j < 4; j++) char_array_4[j] = 0;

            for (j = 0; j < 4; j++) char_array_4[j] = base64_chars.find(char_array_4[j]);

            char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
            char_array_3[1] = ((char_array_4[1] & 0x0F) << 4) + ((char_array_4[2] & 0x3C) >> 2);
            char_array_3[2] = ((char_array_4[2] & 0x03) << 6) + char_array_4[3];

            for (j = 0; (j < i - 1); j++) decoded_string += char_array_3[j];
        }

        return decoded_string;
    }

    std::vector<std::string> splitString(const std::string& input, char delimiter) {
        std::vector<std::string> tokens;
        std::string token;
        std::istringstream tokenStream(input);

        while (std::getline(tokenStream, token, delimiter)) {
            tokens.push_back(token);
        }

        return tokens;
    }

    std::string msg_decode(const std::string& input) {
        auto tokens = splitString(input, '|');
        if (tokens.size() >= 4) {
            return tokens[3];
        }

        size_t lastDelimPos = input.rfind('|');
        if (lastDelimPos != std::string::npos) {
            return input.substr(lastDelimPos + 1);
        }

        return "";
    }

    template <typename... Args>
    std::string prepareRequest(const std::string& format, Args... args) {
        int size_s = std::snprintf(nullptr, 0, format.c_str(), args...) + 1;  // Extra space for '\0'
        if (size_s <= 0) {
            throw std::runtime_error("Error during formatting.");
        }
        auto size = static_cast<size_t>(size_s);
        std::unique_ptr<char[]> buf(new char[size]);
        std::snprintf(buf.get(), size, format.c_str(), args...);
        return std::string(buf.get(), buf.get() + size - 1);  // We don't want the '\0' inside
    }

    bool startsWithHeyNarrator(const std::string& msg) {
        std::string upperMsg = msg;
        std::string target = ": HEY NARRATOR";
        std::string target2 = ":HEY NARRATOR";
        // Convert both strings to uppercase
        std::transform(upperMsg.begin(), upperMsg.end(), upperMsg.begin(),
                       [](unsigned char c) { return std::toupper(c); });

        // Check if it starts with "HEY NARRATOR"
        return (upperMsg.find(target) != std::string::npos) || (upperMsg.find(target2) != std::string::npos);
    }

    bool startsWithHey(const std::string& msg) {
        std::string upperMsg = msg;
        std::string target = ": HEY ";
        std::string target2 = ":HEY ";
        // Convert both strings to uppercase
        std::transform(upperMsg.begin(), upperMsg.end(), upperMsg.begin(),
                       [](unsigned char c) { return std::toupper(c); });

        // Check if it starts with "HEY"
        return (upperMsg.find(target) != std::string::npos) || (upperMsg.find(target2) != std::string::npos);
    }

    std::string getFirstWord(const std::string& agentname) {
        // Find the position of the first space
        size_t spacePos = agentname.find(' ');

        // If a space is found, return the substring before the space
        if (spacePos != std::string::npos) {
            return agentname.substr(0, spacePos);
        }

        // If no space is found, return the entire string
        return agentname;
    }

    std::string sendMsg(const char* msg, bool close_asap, std::string listener) {
        constexpr size_t MAX_RESPONSE_SIZE = 1024 * 1024 * 5;  // 5MB limit
        constexpr size_t BUFFER_SIZE = 1024;
        constexpr int TIMEOUT_SECONDS = 30;

        if (ThreadPool::getInstance().isCurrentTaskCancelled()) {
            logger::info("[sendMsg] Task cancelled before starting for listener: {}", listener);
            return "";
        }

        WSADATA wsaData;
        int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (iResult != 0) {
            logger::error("WSAStartup failed: {}", iResult);
            return "";
        }

        // Create a socket for the HTTP connection
        SOCKET rawSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (rawSocket == INVALID_SOCKET) {
            logger::error("Failed to create socket: {}", WSAGetLastError());
            WSACleanup();
            return "";
        }

        // Set the socket to non-blocking mode
        u_long mode = 1;
        iResult = ioctlsocket(rawSocket, FIONBIO, &mode);
        if (iResult == SOCKET_ERROR) {
            logger::error("Failed to set non-blocking mode: {}", WSAGetLastError());
            closesocket(rawSocket);
            WSACleanup();
            return "";
        }

        struct addrinfo hints;
        struct addrinfo* result = NULL;
        struct addrinfo* ptr = NULL;

        ZeroMemory(&hints, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        int adHres = getaddrinfo(Conf::getInstance().getServer().c_str(), Conf::getInstance().getPort().c_str(), &hints,
                                 &result);

        if (adHres != 0) {
            logger::error("getaddrinfo failed error:{} server:'{}' port:'{}'", adHres,
                          Conf::getInstance().getServer().c_str(), Conf::getInstance().getPort().c_str());
            closesocket(rawSocket);
            WSACleanup();
            return "";
        }

        // Ensure result is freed
        std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> resultGuard(result, freeaddrinfo);

        // Connect with timeout handling
        bool connected = false;
        auto startTime = std::chrono::steady_clock::now();

        for (ptr = result; ptr != NULL; ptr = ptr->ai_next) {
            if (ThreadPool::getInstance().isCurrentTaskCancelled()) {
                logger::info("[sendMsg] Task cancelled during connection for listener: {}", listener);
                closesocket(rawSocket);
                WSACleanup();
                return "";
            }

            iResult = connect(rawSocket, ptr->ai_addr, (int)ptr->ai_addrlen);

            if (iResult == SOCKET_ERROR) {
                if (WSAGetLastError() == WSAEWOULDBLOCK) {
                    fd_set writeSet;
                    FD_ZERO(&writeSet);
                    FD_SET(rawSocket, &writeSet);
                    timeval timeout;
                    timeout.tv_sec = 5;
                    timeout.tv_usec = 0;

                    iResult = select(0, NULL, &writeSet, NULL, &timeout);
                    if (iResult > 0 && FD_ISSET(rawSocket, &writeSet)) {
                        connected = true;
                        break;
                    }
                }
                continue;
            }
            connected = true;
            break;
        }

        if (!connected) {
            logger::error("Could not connect to server");
            closesocket(rawSocket);
            WSACleanup();
            return "";
        }

        // Prepare and send HTTP request
        std::string httpRealRequest;
        if (listener.empty()) {
            httpRealRequest = std::format("GET /{0}?DATA={1} HTTP/1.1\r\nHost: {2}\r\nConnection: close\r\n\r\n",
                                          Conf::getInstance().getPath(), msg, Conf::getInstance().getServer());
        } else {
            httpRealRequest =
                std::format("GET /{0}?DATA={1}&profile={2} HTTP/1.1\r\nHost: {3}\r\nConnection: close\r\n\r\n",
                            Conf::getInstance().getPath(), msg, md5(listener,true), Conf::getInstance().getServer());
        }

        if (!msg) {
            logger::error("[sendMsg] Message pointer is null");
            closesocket(rawSocket);
            WSACleanup();
            return "";
        }

        if (httpRealRequest.empty()) {
            logger::error("[sendMsg] HTTP request is empty");
            closesocket(rawSocket);
            WSACleanup();
            return "";
        }

        // Add connection state validation
        {
            int error = 0;
            int len = sizeof(error);
            if (getsockopt(rawSocket, SOL_SOCKET, SO_ERROR, (char*)&error, &len) < 0 || error != 0) {
                logger::error("[sendMsg] Socket in invalid state: {}", error);
                closesocket(rawSocket);
                WSACleanup();
                return "";
            }
        }

        // Validate socket is still valid before sending
        if (rawSocket == INVALID_SOCKET) {
            logger::error("[sendMsg] Socket became invalid before send");
            WSACleanup();
            return "";
        }

        // Add timeout for send operation
        DWORD timeout = 5000; // 5 second timeout
        if (setsockopt(rawSocket, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout)) == SOCKET_ERROR) {
            logger::error("[sendMsg] Failed to set send timeout: {}", WSAGetLastError());
        }

        // Store request size before sending
        size_t requestSize = httpRealRequest.size();
        if (requestSize == 0) {
            logger::error("[sendMsg] Request size is 0");
            closesocket(rawSocket);
            WSACleanup();
            return "";
        }

        // Send the request with proper error handling
        // logger::debug("[sendMsg] Sending - Data: {}, Size: {}, Socket: {}", httpRealRequest.c_str(), static_cast<int>(requestSize), rawSocket);
        iResult = send(rawSocket, httpRealRequest.c_str(), static_cast<int>(requestSize), 0);
        if (iResult == SOCKET_ERROR) {
            logger::error("Failed to send HTTP request: {} {}", httpRealRequest, WSAGetLastError());
            closesocket(rawSocket);
            WSACleanup();
            return "";
        }

        // Verify all data was sent
        if (iResult != static_cast<int>(requestSize)) {
            logger::error("Failed to send complete request. Sent {} of {} bytes", iResult, requestSize);
            closesocket(rawSocket);
            WSACleanup();
            return "";
        }

        // Receive response with size limits and timeout
        std::string responseBody;
        responseBody.reserve(8192);  // Reserve reasonable initial capacity
        char buffer[BUFFER_SIZE];

        startTime = std::chrono::steady_clock::now();
        bool breakloop = false;

        while (!breakloop) {
            if (ThreadPool::getInstance().isCurrentTaskCancelled()) {
                logger::info("[sendMsg] Task cancelled during response processing for listener: {}", listener);
                break;
            }

            // Check response size limit
            if (responseBody.size() > MAX_RESPONSE_SIZE) {
                logger::error("Response exceeded maximum size of {} bytes", MAX_RESPONSE_SIZE);
                break;
            }

            // Check timeout
            auto currentTime = std::chrono::steady_clock::now();
            auto elapsedTime = std::chrono::duration_cast<std::chrono::seconds>(currentTime - startTime).count();
            if (elapsedTime >= TIMEOUT_SECONDS) {
                logger::error("Response timeout after {} seconds", TIMEOUT_SECONDS);
                break;
            }

            iResult = recv(rawSocket, buffer, sizeof(buffer) - 1, 0);

            if (iResult > 0) {
                // Check for buffer overflow (shouldn't happen with fixed buffer)
                if (iResult >= sizeof(buffer)) {
                    logger::error("Buffer overflow prevented");
                    break;
                }
                responseBody.append(buffer, iResult);
                startTime = std::chrono::steady_clock::now();  // Reset timeout on successful receive
            } else if (iResult == 0) {
                break;  // Connection closed
            } else {
                if (WSAGetLastError() == WSAEWOULDBLOCK) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                break;
            }
        }

        // Cleanup socket
        closesocket(rawSocket);
        WSACleanup();

        // Extract body from response
        std::size_t headerEnd = responseBody.find("\r\n\r\n");
        if (headerEnd != std::string::npos) {
            responseBody = responseBody.substr(headerEnd + 4);
        }

        // Remove custom close marker if present
        size_t pos = responseBody.find("X-CUSTOM-CLOSE");
        if (pos != std::string::npos) {
            responseBody.erase(pos, std::string("X-CUSTOM-CLOSE").length());
        }

        return responseBody;
    }

int sendMsgStream(const char* msg, bool close_asap, std::string speaker, int rechatDepth = 0,
                  bool godmode = false, std::uint64_t dialogueStopGenerationSnapshot = 0) {
        constexpr size_t MAX_RESPONSE_SIZE = 1024 * 1024 * 20;  // 20MB limit
        constexpr size_t BUFFER_SIZE = 4096;
        constexpr int MAX_RECHAT_DEPTH = 10;  // Maximum allowed rechat depth
        const int TIMEOUT_SECONDS = GlobalConfiguredTimeout;

        logger::info("[sendMsgStream] Starting sendMsgStream for speaker: {}, rechatDepth: {}, godmode: {}, dialogueStopGenerationSnapshot: {}",
            speaker, rechatDepth, godmode, dialogueStopGenerationSnapshot);     

        // Parse event type from the incoming message for narration tracking
        std::string decodedMsg = base64_decode(msg);
        std::vector<std::string> msgParts = splitString(decodedMsg, '|');
        std::string requestEventType = "unknown";
        if (msgParts.size() > 0) {
            requestEventType = msgParts[0];  // First part is event type (rechat, narration, inputtext, etc.)
            logger::info("[EVENT_TYPE] Request event type: {}", requestEventType);
        }

        // Check rechat depth first
        if (rechatDepth > MAX_RECHAT_DEPTH) {
            logger::info("Preventing recursive rechat, depth limit reached for speaker: {}", speaker);
            return 0;
        }

        // Check if task is cancelled before starting
        if (ThreadPool::getInstance().isCurrentTaskCancelled()) {
            logger::info("Task cancelled before starting for speaker: {}", speaker);
            return 0;
        }

        WSADATA wsaData;
        int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (iResult != 0) {
            logger::info("WSAStartup failed: {}", iResult);
            return 1;
        }

        // Create a socket for the HTTP connection
        SOCKET rawSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (rawSocket == INVALID_SOCKET) {
            logger::error("Failed to create socket: {}", WSAGetLastError());
            WSACleanup();
            return 2;
        }

        // Set the socket to non-blocking mode
        u_long mode = 1;
        iResult = ioctlsocket(rawSocket, FIONBIO, &mode);
        if (iResult == SOCKET_ERROR) {
            logger::error("Failed to set non-blocking mode: {}", WSAGetLastError());
            closesocket(rawSocket);
            WSACleanup();
            return 3;
        }

        struct addrinfo hints;
        struct addrinfo* result = NULL;
        struct addrinfo* ptr = NULL;

        ZeroMemory(&hints, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        int adHres = getaddrinfo(Conf::getInstance().getServer().c_str(), Conf::getInstance().getPort().c_str(), &hints,
                                 &result);

        if (adHres != 0) {
            logger::error("getaddrinfo failed: {}", adHres);
            closesocket(rawSocket);
            WSACleanup();
            return 4;
        }

        // Ensure result is freed
        std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> resultGuard(result, freeaddrinfo);

        // Connect to the server
        bool connected = false;
        auto startTime = std::chrono::steady_clock::now();

        for (ptr = result; ptr != NULL; ptr = ptr->ai_next) {
            // Check for cancellation during connection attempts
            if (ThreadPool::getInstance().isCurrentTaskCancelled()) {
                logger::info("Task cancelled during connection attempt for speaker: {}", speaker);
                closesocket(rawSocket);
                WSACleanup();
                return 0;
            }

            iResult = connect(rawSocket, ptr->ai_addr, (int)ptr->ai_addrlen);

            if (iResult == SOCKET_ERROR) {
                if (WSAGetLastError() == WSAEWOULDBLOCK) {
                    fd_set writeSet;
                    FD_ZERO(&writeSet);
                    FD_SET(rawSocket, &writeSet);
                    timeval timeout;
                    timeout.tv_sec = 5;
                    timeout.tv_usec = 0;

                    iResult = select(0, NULL, &writeSet, NULL, &timeout);
                    if (iResult > 0 && FD_ISSET(rawSocket, &writeSet)) {
                        connected = true;
                        break;
                    }
                }
                continue;
            }
            connected = true;
            break;
        }

        if (!connected) {
            logger::error("Could not connect to server");
            closesocket(rawSocket);
            WSACleanup();
            return 5;
        }

        // Prepare and send HTTP request
        std::string destination = Conf::getInstance().getPathStream();
        
        if (godmode) {
            size_t pos = destination.find("stream.php");
            if (pos != std::string::npos) {
                destination.replace(pos, std::string("stream.php").length(), "godmode.php");
            }
        } else {
            if (NewActionMode) {
                size_t pos = destination.find("stream.php");
                if (pos != std::string::npos) {
                    destination.replace(pos, std::string("stream.php").length(), "streamv2.php");
                }
            }
        }
        

        

        std::string httpRealRequest =
            std::format("GET /{0}?DATA={1}&profile={2} HTTP/1.1\r\nHost: {3}\r\nConnection: close\r\n\r\n", destination,
                        msg, md5(speaker,true), Conf::getInstance().getServer());

        iResult = send(rawSocket, httpRealRequest.c_str(), httpRealRequest.size(), 0);
        if (iResult == SOCKET_ERROR) {
            logger::error("Failed to send HTTP request: {}", WSAGetLastError());
            closesocket(rawSocket);
            WSACleanup();
            return 6;
        }

        std::string response;
        response.reserve(8192);  // Reserve reasonable initial capacity
        std::vector<std::string> responseParts;
        bool firstSend = false;
        bool firstReceived = false;
        bool headersProcessed = false;
        bool rechatResponseReceived = false;
        RE::Actor* npc = nullptr;
        std::string targetFollower;
        std::size_t streamedLineCount = 0;
        std::string closeReason = "unknown";

        char buffer[BUFFER_SIZE];
        startTime = std::chrono::steady_clock::now();
        bool breakloop = false;

        auto tid = ThreadPool::getInstance().getCurrentTaskId();

        while (!breakloop) {
            // Check for cancellation periodically
            if (ThreadPool::getInstance().isCurrentTaskCancelled()) {
                logger::info("Task cancelled during response processing for speaker: {}", speaker);
                closeReason = "task_cancelled";
                breakloop = true;
                break;
            }

            // Check response size limit
            if (response.size() > MAX_RESPONSE_SIZE) {
                logger::error("Response exceeded maximum size of {} bytes", MAX_RESPONSE_SIZE);
                closeReason = "response_too_large";
                break;
            }

            // Check timeout
            auto currentTime = std::chrono::steady_clock::now();
            auto elapsedTime = std::chrono::duration_cast<std::chrono::seconds>(currentTime - startTime).count();
            if (elapsedTime >= TIMEOUT_SECONDS) {
                std::string originalrequest = base64_decode(msg);
                logger::info("No data received for {} seconds, breaking the loop, request was {}", TIMEOUT_SECONDS,
                             originalrequest);
                RE::DebugNotification("Seems there are connection issues. Check server log");
                closeReason = "timeout";
                break;
            }

            iResult = recv(rawSocket, buffer, sizeof(buffer) - 1, 0);

            if (iResult > 0) {
                response.append(buffer, iResult);
                startTime = std::chrono::steady_clock::now();  // Reset timeout on successful receive

                // Parse HTTP headers once we have them (marked by \r\n\r\n)
                if (!headersProcessed) {
                    size_t headerEnd = response.find("\r\n\r\n");
                    if (headerEnd != std::string::npos) {
                        std::string headers = response.substr(0, headerEnd);
                        
                        // Look for X-Event-Type header
                        // This tells us if the server converted the request (e.g., rechat -> narration)
                        size_t eventTypePos = headers.find("X-Event-Type:");
                        if (eventTypePos != std::string::npos) {
                            size_t headerStart = eventTypePos + 13; // Length of "X-Event-Type:"
                            size_t headerLineEnd = headers.find("\r\n", headerStart);
                            if (headerLineEnd != std::string::npos) {
                                std::string eventType = headers.substr(headerStart, headerLineEnd - headerStart);
                                // Trim whitespace
                                size_t firstNonSpace = eventType.find_first_not_of(" \t");
                                size_t lastNonSpace = eventType.find_last_not_of(" \t\r\n");
                                if (firstNonSpace != std::string::npos && lastNonSpace != std::string::npos) {
                                    lastEventType = eventType.substr(firstNonSpace, lastNonSpace - firstNonSpace + 1);
                                    logger::info("[EVENT_TYPE] Server actual event type: {}", lastEventType);
                                } else {
                                    lastEventType = requestEventType;
                                    logger::info("[EVENT_TYPE] Using request event type (empty header): {}", lastEventType);
                                }
                            }
                        } else {
                            // Fallback to request type if header not present
                            lastEventType = requestEventType;
                            logger::info("[EVENT_TYPE] Using request event type (no header): {}", lastEventType);
                        }
                        
                        // Strip headers from response to get body only
                        response = response.substr(headerEnd + 4);
                        headersProcessed = true;
                    }
                }

                // Process complete lines
                size_t pos = 0;
                
                while ((pos = response.find("\r\n")) != std::string::npos) {
                    
                    // Check for cancellation during line processing
                    if (ThreadPool::getInstance().isCurrentTaskCancelled()) {
                        logger::info("Task cancelled during line processing for speaker: {}", speaker);
                        breakloop = true;
                        break;
                    }

                    std::string line = response.substr(0, pos);
                    response.erase(0, pos + 2);  // Erase the processed line from the response

                    // Validate the line format "string|string|string"
                    std::vector<std::string> lineParts = splitString(line, '|');
                    if (lineParts.size() >= 3) {
                        if (PrismaUIBridge::GetDialogueStopGeneration() != dialogueStopGenerationSnapshot) {
                            logger::info("[HTTPMANAGER] Dropping stale response after Stop All Dialogue for speaker: {}",
                                         speaker);
                            breakloop = true;
                            break;
                        }

                        // Do something with the valid response parts
                        // lineParts[0], lineParts[1], lineParts[2]

                        SPGResponse& spgResponse = SPGResponse::getInstance();

                        logger::info("[HTTPMANAGER] Response ready, actor {}, queue {}, taskid {} ", lineParts[0],
                                     lineParts[1], tid);
                        if (rechatDepth > 0 && !rechatResponseReceived) {
                            SpeakManager::getInstance().completeRechatAttempt(speaker, true);
                            rechatResponseReceived = true;
                        }
                        spgResponse.decodeAndEnqueue(line.c_str());
                        streamedLineCount++;


                        spgResponse.markUnFinished(true); // Mark the queue as still receving data

                        std::vector<std::string> lineParts2 = splitString(lineParts[2], '/');
                        if (lineParts2.size() >= 7) {
                            const std::string explicitRechatTarget = trim(lineParts2[6]);
                            if (!explicitRechatTarget.empty()) {
                                targetFollower.assign(explicitRechatTarget);
                            } else if (lineParts2.size() >= 3) {
                                targetFollower.assign(lineParts2[2]);
                            }
                        } else if (lineParts2.size() >= 3) {
                            targetFollower.assign(lineParts2[2]);
                        }

                        if (!firstReceived) {
                            // firstReceived = true;  // 1 .0.10
                            AIAgentManager& aiam = AIAgentManager::getInstance();
                            // Log the exact string we're searching for
                            std::string actorName = trim(lineParts[0]);
                            logger::info("Looking for actor with exact name: '{}'", actorName);

                            if (IsPlayerStreamActor(actorName)) {
                                firstReceived = true;
                                logger::info("Received streamed Player line '{}'; skipping AI agent lookup", actorName);
                            } else {
                                auto agent = aiam.getAgentByName(actorName);

                                if (agent) {
                                    firstReceived = true;  // 1 .0.10
                                    npc = agent.get()->getActor();
                                    if (npc) {
                                        //npc->AllowPCDialogue(false);
                                        RE::Actor* speaker = agent.get()->getActor();
                                        logger::info("Calling attention {}", speaker->GetDisplayFullName());

                                    }
                                } else {
                                    logger::info("Failed to find agent. Current agents:");
                                    for (const auto& a : aiam.getAgents()) {
                                        logger::info("- '{}'", a->getActorName());
                                    }
                                }
                            }
                        }
                    }

                    if (line.starts_with("X-CUSTOM-CLOSE")) {
                        logger::debug("End message because X-CUSTOM-CLOSE");
                        closeReason = "custom_close";
                        breakloop = true;
                        break;
                    }
                }
            } else if (iResult == 0) {
                logger::info("Connection closed by server");
                closeReason = "connection_closed";
                breakloop = true;
            } else {
                if (WSAGetLastError() == WSAEWOULDBLOCK) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                logger::info("Closed connection {}", WSAGetLastError());
                closeReason = std::format("socket_error_{}", WSAGetLastError());
                breakloop = true;
            }
        }

        if (!firstReceived) {
            logger::info("[HTTPManager] Cancelled server side {}, taskid {}", speaker,tid);
        }

        if (rechatDepth > 0 && !rechatResponseReceived) {
            SpeakManager::getInstance().completeRechatAttempt(speaker, false);
        }

        SPGResponse& spgResponse = SPGResponse::getInstance();
        spgResponse.markUnFinished(false);

        // Cleanup socket
        closesocket(rawSocket);
        WSACleanup();


        // Check for cancellation before starting rechat logic
        if (ThreadPool::getInstance().isCurrentTaskCancelled()) {
            logger::info("Task {} cancelled before rechat logic for speaker: {}", tid,speaker);
            return 0;
        }

        if (PrismaUIBridge::GetDialogueStopGeneration() != dialogueStopGenerationSnapshot) {
            logger::info("[HTTPMANAGER] Skipping rechat logic for stale stream speaker: {}", speaker);
            return 0;
        }

        if (GlobalRechatPolicyAsap==1)
            if (npc) {
                logger::info("[RECHAT OLD] Rechat called, task id {}", tid);
                if (SpeakManager::getInstance().rechat(npc->GetDisplayFullName(), targetFollower, rechatDepth,
                                                       "asap") > 0) {
                    SpeakManager::getInstance().beginRechatAttempt(npc->GetDisplayFullName());
                }
            }

        return 0;
    }

    void log(std::string msg) {
        try {
            ThreadPool::getInstance().enqueue(
                "HTTPLog",
                [msg]() {
                    try {
                        std::string finalMsg(base64_encode(msg.c_str(), std::strlen(msg.c_str())));
                        std::string line = sendMsg(finalMsg.c_str(), false, "");
                        SPGResponse& spgResponse = SPGResponse::getInstance();
                        spgResponse.decodeAndEnqueue(line.c_str());
                    } catch (const std::exception& e) {
                        logger::error("[HTTPManager] Error in log thread: {}", e.what());
                    }
                },
                msg.substr(0, 12),
                std::chrono::seconds(45));  // The server blocks while processing, so we need long timeouts.
        } catch (const std::exception& e) {
            logger::error("[HTTPManager] Failed to queue log task: {}", e.what());
        }
    }

    void log(std::string msg, std::string forcedActor) {
        try {
            ThreadPool::getInstance().enqueue(
                "HTTPLogWithForcedActor",
                [msg, forcedActor]() {
                    try {
                        std::string finalMsg(base64_encode(msg.c_str(), std::strlen(msg.c_str())));
                        std::string line = sendMsg(finalMsg.c_str(), false, forcedActor);
                        SPGResponse& spgResponse = SPGResponse::getInstance();
                        spgResponse.decodeAndEnqueue(line.c_str());
                    } catch (const std::exception& e) {
                        logger::error("[HTTPManager] Error in log thread: {}", e.what());
                    }
                },
                forcedActor,
                std::chrono::seconds(45));  // The server blocks while processing, so we need long timeouts.
        } catch (const std::exception& e) {
            logger::error("[HTTPManager] Failed to queue log task: {}", e.what());
        }
    }

    bool requestPlayerMenuTtsPlay(std::string msg) {
        return requestPlayerMenuTtsPlay(std::move(msg), std::string{});
    }

    std::string requestPlayerMenuTtsPlayResponse(std::string msg, std::string forcedActor) {
        try {
            std::string finalMsg(base64_encode(msg.c_str(), std::strlen(msg.c_str())));
            return sendMsg(finalMsg.c_str(), false, forcedActor);
        } catch (const std::exception& e) {
            logger::error("[HTTPManager] player_menu_tts_play request failed: {}", e.what());
            return {};
        }
    }

    bool requestPlayerMenuTtsPlay(std::string msg, std::string forcedActor) {
        try {
            std::string line = requestPlayerMenuTtsPlayResponse(std::move(msg), std::move(forcedActor));
            if (line.empty()) {
                logger::warn("[HTTPManager] player_menu_tts_play returned an empty response");
                return false;
            }

            SPGResponse& spgResponse = SPGResponse::getInstance();
            spgResponse.decodeAndEnqueue(line.c_str());

            const bool queuedPlayerLine = line.find("Player|ScriptQueue|") != std::string::npos;
            if (!queuedPlayerLine) {
                logger::warn("[HTTPManager] player_menu_tts_play response did not include Player ScriptQueue output");
            }

            return queuedPlayerLine;
        } catch (const std::exception& e) {
            logger::error("[HTTPManager] player_menu_tts_play request failed: {}", e.what());
            return false;
        }
    }

    bool requestPlayerMenuTtsPlay(std::string msg, RE::Actor* actor) {
        if (!actor) {
            return requestPlayerMenuTtsPlay(std::move(msg), std::string{});
        }

        AIAgentManager& aiam = AIAgentManager::getInstance();

        std::shared_ptr<AIAgent> agent = nullptr;
        for (const auto& a : aiam.getAgents()) {
            if (a->getActor() && a->getActor()->GetFormID() == actor->GetFormID()) {
                agent = a;
                break;
            }
        }

        const std::string listener = agent ? agent->getActorName() : actor->GetDisplayFullName();
        return requestPlayerMenuTtsPlay(std::move(msg), listener);
    }

    void log(std::string msg, RE::Actor* actor) {
        if (!actor) {
            logger::error("[HTTPManager] Actor is null for msg: {}", msg);
            return;
        }
        
        AIAgentManager& aiam = AIAgentManager::getInstance();
        
        // First try to find agent by FormID (works for all agents including narrator)
        std::shared_ptr<AIAgent> agent = nullptr;
        for (const auto& a : aiam.getAgents()) {
            if (a->getActor() && a->getActor()->GetFormID() == actor->GetFormID()) {
                agent = a;
                break;
            }
        }
        
        // Use agent's name if found (handles narrator name override), otherwise use actor's display name
        std::string listener = agent ? agent->getActorName() : actor->GetDisplayFullName();
        
        try {
            ThreadPool::getInstance().enqueue(
                "HTTPLogWithActor",
                [msg, listener]() {
                    try {
                        std::string finalMsg(base64_encode(msg.c_str(), std::strlen(msg.c_str())));
                        std::string line = sendMsg(finalMsg.c_str(), false, listener);
                        SPGResponse& spgResponse = SPGResponse::getInstance();
                        spgResponse.decodeAndEnqueue(line.c_str());
                    } catch (const std::exception& e) {
                        logger::error("[HTTPManager] Error in log thread: {}", e.what());
                    }
                },
                listener, std::chrono::seconds(45));  // The server blocks while processing, so we need long timeouts.
        } catch (const std::exception& e) {
            logger::error("[HTTPManager] Failed to queue log task: {}", e.what());
        }
    }

    std::string getServerVersionRaw() {
        constexpr int SOCKET_TIMEOUT_MS = 5000;
        constexpr size_t RECV_BUFFER_SIZE = 1024;
        constexpr size_t MAX_RESPONSE_SIZE = 1024 * 1024;

        try {
            auto server = Conf::getInstance().getServer();
            auto port = Conf::getInstance().getPort();
            auto commPath = Conf::getInstance().getPath();

            if (server.empty() || port.empty() || commPath.empty()) {
                logger::error("[VersionCheck] Missing server configuration");
                return "";
            }

            std::string basePath = commPath;
            size_t lastSlash = basePath.find_last_of('/');
            if (lastSlash != std::string::npos) {
                basePath = basePath.substr(0, lastSlash + 1);
            } else {
                basePath = "/";
            }

            std::string endpointPath = basePath + "ui/tools/server_version.php";
            if (!endpointPath.empty() && endpointPath.front() != '/') {
                endpointPath.insert(endpointPath.begin(), '/');
            }

            WSADATA wsaData;
            int wsaRes = WSAStartup(MAKEWORD(2, 2), &wsaData);
            if (wsaRes != 0) {
                logger::error("[VersionCheck] WSAStartup failed: {}", wsaRes);
                return "";
            }

            struct addrinfo hints {};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_protocol = IPPROTO_TCP;

            struct addrinfo* result = nullptr;
            int addrRes = getaddrinfo(server.c_str(), port.c_str(), &hints, &result);
            if (addrRes != 0 || !result) {
                logger::error("[VersionCheck] getaddrinfo failed for {}:{} ({})", server, port, addrRes);
                WSACleanup();
                return "";
            }

            SOCKET rawSocket = INVALID_SOCKET;
            bool connected = false;
            for (auto ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
                rawSocket = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
                if (rawSocket == INVALID_SOCKET) {
                    continue;
                }

                DWORD timeout = SOCKET_TIMEOUT_MS;
                setsockopt(rawSocket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
                setsockopt(rawSocket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));

                if (connect(rawSocket, ptr->ai_addr, static_cast<int>(ptr->ai_addrlen)) == 0) {
                    connected = true;
                    break;
                }

                closesocket(rawSocket);
                rawSocket = INVALID_SOCKET;
            }

            freeaddrinfo(result);

            if (!connected || rawSocket == INVALID_SOCKET) {
                logger::error("[VersionCheck] Could not connect to {}:{}", server, port);
                WSACleanup();
                return "";
            }

            std::string request = std::format(
                "GET {} HTTP/1.1\r\nHost: {}\r\nConnection: close\r\n\r\n",
                endpointPath,
                server
            );

            int sent = send(rawSocket, request.c_str(), static_cast<int>(request.size()), 0);
            if (sent == SOCKET_ERROR) {
                logger::error("[VersionCheck] Failed to request server_version endpoint: {}", WSAGetLastError());
                closesocket(rawSocket);
                WSACleanup();
                return "";
            }

            std::string response;
            response.reserve(4096);
            char buffer[RECV_BUFFER_SIZE];
            int bytes = 0;
            while ((bytes = recv(rawSocket, buffer, sizeof(buffer), 0)) > 0) {
                response.append(buffer, bytes);
                if (response.size() > MAX_RESPONSE_SIZE) {
                    logger::error("[VersionCheck] Response too large");
                    closesocket(rawSocket);
                    WSACleanup();
                    return "";
                }
            }

            closesocket(rawSocket);
            WSACleanup();

            if (response.empty()) {
                logger::error("[VersionCheck] Empty response from server_version endpoint");
                return "";
            }

            size_t bodyStart = response.find("\r\n\r\n");
            std::string body = (bodyStart == std::string::npos) ? response : response.substr(bodyStart + 4);

            json parsed = json::parse(body, nullptr, false);
            if (parsed.is_discarded() || !parsed.contains("serverVersion") || !parsed["serverVersion"].is_string()) {
                logger::error("[VersionCheck] Invalid JSON payload from server_version endpoint");
                return "";
            }

            return trim(parsed["serverVersion"].get<std::string>());
        } catch (const std::exception& e) {
            logger::error("[VersionCheck] Exception while fetching server version: {}", e.what());
            return "";
        }
    }

    static bool postGameDataInternal(const std::string& endpoint, const nlohmann::json& data)
    {
        std::string actorName = data.contains("actor_name") ? data["actor_name"].get<std::string>() : "Unknown";
        std::string dataType = data.contains("type") ? data["type"].get<std::string>() : "unknown";

        WSADATA wsaData;
        int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (iResult != 0) {
            logger::error("[postGameData] WSAStartup failed: {}", iResult);
            return false;
        }

        SOCKET rawSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (rawSocket == INVALID_SOCKET) {
            logger::error("[postGameData] Failed to create socket: {}", WSAGetLastError());
            WSACleanup();
            return false;
        }

        auto server = Conf::getInstance().getServer();
        auto portStr = Conf::getInstance().getPort();
        int port = std::stoi(portStr);

        std::string configPath = Conf::getInstance().getPath();
        std::string basePath;
        size_t lastSlash = configPath.find_last_of("/");
        if (lastSlash != std::string::npos) {
            basePath = configPath.substr(0, lastSlash + 1);
        }
        std::string fullEndpoint = basePath + endpoint;

        sockaddr_in serverAddr;
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(static_cast<u_short>(port));
        inet_pton(AF_INET, server.c_str(), &serverAddr.sin_addr);

        DWORD timeout = 5000;
        setsockopt(rawSocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
        setsockopt(rawSocket, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));

        iResult = connect(rawSocket, (sockaddr*)&serverAddr, sizeof(serverAddr));
        if (iResult == SOCKET_ERROR) {
            logger::error("[postGameData] Failed to connect: {}", WSAGetLastError());
            closesocket(rawSocket);
            WSACleanup();
            return false;
        }

        std::string jsonBody = data.dump();
        std::string httpRequest = std::format(
            "POST /{} HTTP/1.1\r\n"
            "Host: {}\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: {}\r\n"
            "Connection: close\r\n"
            "\r\n"
            "{}",
            fullEndpoint, server, jsonBody.size(), jsonBody);

        iResult = send(rawSocket, httpRequest.c_str(), static_cast<int>(httpRequest.size()), 0);
        if (iResult == SOCKET_ERROR) {
            logger::error("[postGameData] Failed to send: {}", WSAGetLastError());
            closesocket(rawSocket);
            WSACleanup();
            return false;
        }

        char buffer[1024];
        std::string fullResponse;
        bool success = false;

        iResult = recv(rawSocket, buffer, sizeof(buffer) - 1, 0);
        if (iResult > 0) {
            buffer[iResult] = '\0';
            fullResponse = std::string(buffer);

            if (fullResponse.find("200 OK") != std::string::npos ||
                fullResponse.find("HTTP/1.1 200") != std::string::npos ||
                fullResponse.find("HTTP/1.0 200") != std::string::npos) {
                logger::trace("[postGameData] Successfully sent {} for {}", dataType, actorName);
                success = true;
            } else {
                std::string responsePreview = fullResponse.substr(0, std::min<size_t>(200, fullResponse.length()));
                logger::warn("[postGameData] Unexpected response for {} ({}): {}", actorName, dataType, responsePreview);
            }
        } else if (iResult == 0) {
            logger::debug("[postGameData] Connection closed by server for {} ({})", actorName, dataType);
        } else {
            logger::error("[postGameData] recv failed for {} ({}): {}", actorName, dataType, WSAGetLastError());
        }

        closesocket(rawSocket);
        WSACleanup();
        return success;
    }

    void postGameData(const std::string& endpoint, const nlohmann::json& data) {
        try {
            std::string actorName = data.contains("actor_name") ? data["actor_name"].get<std::string>() : "Unknown";
            ThreadPool::getInstance().enqueue(
                "HTTPGameData",
                [endpoint, data]() {
                    try {
                        postGameDataInternal(endpoint, data);
                    } catch (const std::exception& e) {
                        logger::error("[postGameData] Error: {}", e.what());
                    }
                },
                actorName,
                std::chrono::seconds(10)
            );
        } catch (const std::exception& e) {
            logger::error("[HTTPManager] Failed to queue postGameData task: {}", e.what());
        }
    }

    bool postGameDataSync(const std::string& endpoint, const nlohmann::json& data)
    {
        try {
            return postGameDataInternal(endpoint, data);
        } catch (const std::exception& e) {
            logger::error("[postGameDataSync] Error: {}", e.what());
            return false;
        }
    }

    void stream(std::string msg) { stream(msg, 0); }


    void stream(std::string msg, int rechatDepth) {
        // Determine speaker

        AIAgentManager& aiam = AIAgentManager::getInstance();

        RE::Actor* listenerPtr = nullptr;

        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            logger::warn("[LISTENER-RESOLVE] No player singleton");
            return;
        }

        std::string narratorOverrideReason;
        auto* localPlayerCell = player->GetParentCell();
        const auto playerSpatialSettings = GetPlayerSpeechSpatialSettings(player, HERIKA_MAX_VISION_RANGE);
        const float spatialListenerDistanceLimit =
            (localPlayerCell && localPlayerCell->IsInteriorCell())
                ? playerSpatialSettings.interiorMaxDistance
                : playerSpatialSettings.exteriorMaxDistance;

        const bool playerInputMessage =
            msg.starts_with("inputtext_s|") || msg.starts_with("inputtext|") ||
            msg.starts_with("ginputtext_s|") || msg.starts_with("ginputtext|");
        const auto spatialSnapshot = SpatialSnapshotManager::GetPlayerSnapshot(
            false, playerInputMessage ? "player_input_listener_resolve" : "listener_resolve");
        const auto& audibleActors = spatialSnapshot.audibleActors;
        std::unordered_map<RE::FormID, const AudibleActorDescriptor*> audibleActorsByFormId;
        audibleActorsByFormId.reserve(audibleActors.size());
        for (const auto& audibleActor : audibleActors) {
            audibleActorsByFormId[audibleActor.formId] = &audibleActor;
        }
        float minDistance = (std::numeric_limits<float>::max)();
        bool directedChat = false;
        const bool everyoneTargetOverride = PrismaUIBridge::IsChatboxEveryoneTargetOverrideActive();
        auto rankedTargets = SpatialSnapshotManager::GetPlayerConversationTargets(
            playerInputMessage ? "player_input_listener_resolve" : "listener_resolve", true);

        std::string currentParty = "";
        for (const auto& target : rankedTargets) {
            auto* targetActor = target.actor;
            if (!targetActor || !targetActor->IsPlayerTeammate()) {
                continue;
            }

            auto* vampireForm = RE::BGSKeyword::LookupByID(0x000A82BB);
            auto* isVampireKW = vampireForm ? vampireForm->As<RE::BGSKeyword>() : nullptr;
            bool isVampire = isVampireKW && targetActor->HasKeyword(isVampireKW);
            std::string playerinfo =
                std::format("\"level\":{},\"name\":\"{}\",\"race\":\"{}\",\"gender\":\"{}\",\"isVampire\":\"{}\"",
                            targetActor->GetLevel(), EscapeJson(targetActor->GetDisplayFullName()),
                            EscapeJson(targetActor->GetRace()->GetFullName()),
                            (targetActor->GetActorBase()->GetSex() == RE::SEXES::kFemale) ? "female" : "male",
                            isVampire ? "yes" : "no");
            currentParty.append("{" + playerinfo + "},");
        }

        HTTPManager::log(
            std::format("setconf|{}|{}|CurrentParty@{}", getCurrentTimeMillis(), GetGameTimeStamp(), currentParty));
        logger::debug("setconf|{}|{}|CurrentParty@{}", getCurrentTimeMillis(), GetGameTimeStamp(), currentParty);

        std::shared_ptr<AIAgent> agentPointer = nullptr;
        std::string listener;
        const auto selectTarget = [&](const PlayerSpatialCandidate& target, const char* source) {
            if (!target.agent || !target.actor) {
                return false;
            }

            listenerPtr = target.actor;
            agentPointer = target.agent;
            listener = target.name.empty() ? target.agent->getActorName() : target.name;
            minDistance = target.airDistance;
            directedChat = target.lookTarget;
            logger::info("[LISTENER-RESOLVE] Selected '{}' via {} (source={}, status={}, dist={:.1f}m)",
                         listener, source, target.source, target.status, target.distanceMeters);
            return true;
        };
        auto validatePlayerInputTarget = [&](const PlayerSpatialCandidate& target, const char* source) {
            if (!playerInputMessage || !target.actor) {
                return true;
            }

            // VoiceRecord runs on a worker thread. Do not call full SpatialAwareness::Evaluate here:
            // LOS/navmesh/door/game-ref reads belong on the game-thread snapshot path and can throw SEH
            // if actor refs move or unload while STT is handing off. For final STT routing, only accept
            // targets whose cached/ranked status already came from a stable spatial result.
            const bool stableAudibleReason =
                target.reason == "immediate_proximity" ||
                target.reason == "line_of_sight_clear" ||
                target.reason == "path_fallback_clear" ||
                target.reason == "open_door_muffled";
            const bool actualCrosshairTarget = target.source.starts_with("crosshair_");
            constexpr float kCloseLookTargetMeters = 6.0f;
            const bool closeLookTarget = target.lookTarget && target.reason == "distance_cell_clear" &&
                                         target.distanceMeters <= kCloseLookTargetMeters;
            const bool directCheapTarget = target.targetable && (actualCrosshairTarget || closeLookTarget);

            if (!target.targetable || (!stableAudibleReason && !directCheapTarget)) {
                logger::info(
                    "[LISTENER-RESOLVE] Rejecting '{}' via {}: STT routing requires cached stable spatial "
                    "(source={}, status={}, reason={}, targetable={}, dist={:.1f}m)",
                    target.name.empty() && target.agent ? target.agent->getActorName() : target.name,
                    source, target.source, target.status, target.reason, target.targetable ? 1 : 0,
                    target.distanceMeters);
                return false;
            }

            if (stableAudibleReason) {
                logger::info(
                    "[LISTENER-RESOLVE] Confirmed '{}' via {} using cached stable spatial "
                    "(source={}, status={}, reason={}, dist={:.1f}m)",
                    target.name.empty() && target.agent ? target.agent->getActorName() : target.name,
                    source, target.source, target.status, target.reason, target.distanceMeters);
            } else {
                logger::info(
                    "[LISTENER-RESOLVE] Confirmed '{}' via {} using direct cheap target "
                    "(source={}, status={}, reason={}, dist={:.1f}m)",
                    target.name.empty() && target.agent ? target.agent->getActorName() : target.name,
                    source, target.source, target.status, target.reason, target.distanceMeters);
            }
            return true;
        };
        std::vector<RE::FormID> rejectedTargetFormIds;
        rejectedTargetFormIds.reserve(4);
        const auto wasRejectedTarget = [&](RE::FormID formId) {
            return formId != 0 &&
                   std::find(rejectedTargetFormIds.begin(), rejectedTargetFormIds.end(), formId) !=
                       rejectedTargetFormIds.end();
        };

        uint32_t chatboxOverrideFormId = 0;
        std::string chatboxOverrideName;
        if (PrismaUIBridge::GetChatboxTargetOverride(chatboxOverrideFormId, chatboxOverrideName)) {
            auto overrideIt = std::find_if(rankedTargets.begin(), rankedTargets.end(),
                [&](const PlayerSpatialCandidate& target) {
                    const bool formMatch = chatboxOverrideFormId != 0 && target.formId == chatboxOverrideFormId;
                    const bool nameMatch = !chatboxOverrideName.empty() && target.name == chatboxOverrideName;
                    return formMatch || nameMatch;
            });
            if (overrideIt != rankedTargets.end()) {
                if (selectTarget(*overrideIt, "chatbox_override")) {
                    directedChat = true;
                    logger::info("[LISTENER-RESOLVE] Chatbox override absolute priority: {} (bypassed spatial validate)",
                                 chatboxOverrideName);
                }
            } else if (!chatboxOverrideName.empty()) {
                auto* overrideAgent = aiam.getAgentByName(chatboxOverrideName).get();
                if (overrideAgent && overrideAgent->getActor() && !overrideAgent->getActor()->IsDead()) {
                    PlayerSpatialCandidate fallback{};
                    fallback.agent = aiam.getAgentByName(chatboxOverrideName);
                    fallback.actor = overrideAgent->getActor();
                    fallback.name = chatboxOverrideName;
                    fallback.formId = overrideAgent->getActor()->GetFormID();
                    fallback.source = "chatbox_override_outofrange";
                    fallback.status = "Locked target (out of spatial range)";
                    fallback.reason = "chatbox_override_absolute";
                    fallback.targetable = true;
                    fallback.airDistance = 0.0f;
                    fallback.distanceMeters = 0.0f;
                    if (selectTarget(fallback, "chatbox_override_outofrange")) {
                        directedChat = true;
                        logger::info("[LISTENER-RESOLVE] Chatbox override target out of spatial range; sending anyway: {}",
                                     chatboxOverrideName);
                    }
                } else {
                    logger::warn("[LISTENER-RESOLVE] Chatbox override target not found or dead; clearing: name='{}'",
                                 chatboxOverrideName);
                    PrismaUIBridge::ClearChatboxTargetOverride();
                }
            }
        }

        if (!agentPointer) {
            auto lookIt = std::find_if(rankedTargets.begin(), rankedTargets.end(),
                [](const PlayerSpatialCandidate& target) {
                    return target.lookTarget && target.targetable;
                });
            if (lookIt != rankedTargets.end()) {
                if (!validatePlayerInputTarget(*lookIt, "look_target")) {
                    rejectedTargetFormIds.push_back(lookIt->formId);
                    logger::info("[LISTENER-RESOLVE] Look target failed final spatial check; trying ranked auto target");
                } else {
                    selectTarget(*lookIt, "look_target");
                }
            }
        }

        if (!agentPointer) {
            constexpr int kMaxRankedAutoTargetChecks = 3;
            int rankedAutoTargetsChecked = 0;
            bool rankedAutoTargetRejected = false;
            for (const auto& target : rankedTargets) {
                if (!target.autoEligible || wasRejectedTarget(target.formId)) {
                    continue;
                }

                if (rankedAutoTargetsChecked >= kMaxRankedAutoTargetChecks) {
                    break;
                }
                ++rankedAutoTargetsChecked;

                if (validatePlayerInputTarget(target, "ranked_targets")) {
                    selectTarget(target, "ranked_targets");
                    break;
                }

                rejectedTargetFormIds.push_back(target.formId);
                rankedAutoTargetRejected = true;
            }

            if (!agentPointer && rankedAutoTargetRejected) {
                logger::info(
                    "[LISTENER-RESOLVE] Ranked auto targets failed final spatial check after {} candidate(s); falling back",
                    rankedAutoTargetsChecked);
            }
        }

        if (!agentPointer && playerInputMessage) {
            const auto fallbackIt = std::find_if(rankedTargets.begin(), rankedTargets.end(),
                [](const PlayerSpatialCandidate& target) {
                    return target.agent && target.actor && target.agent->getActorName() != NARRATOR_NAME &&
                           target.targetable;
                });
            if (fallbackIt != rankedTargets.end()) {
                logger::info(
                    "[LISTENER-RESOLVE] Using nearest targetable NPC fallback '{}' before Narrator "
                    "(source={}, status={}, reason={}, dist={:.1f}m)",
                    fallbackIt->name.empty() ? fallbackIt->agent->getActorName() : fallbackIt->name,
                    fallbackIt->source, fallbackIt->status, fallbackIt->reason, fallbackIt->distanceMeters);
                selectTarget(*fallbackIt, "nearest_targetable_fallback");
            }
        }

        if (!agentPointer) {
            auto narrator = aiam.getAgentByName(NARRATOR_NAME);
            if (narrator) {
                logger::info("[LISTENER-RESOLVE] Routing to Narrator: no targetable NPC in ranked spatial targets (audible={}, ranked={})",
                             audibleActors.size(), rankedTargets.size());
                listenerPtr = narrator->getActor();
                agentPointer = narrator;
                listener.assign(NARRATOR_NAME);
                directedChat = true;
                narratorOverrideReason = "no targetable NPCs";
            }
        }

        if (!agentPointer) {
            logger::info("No agent available");
            return;
        }

        try {
            if (agentPointer->getActor() && listener != NARRATOR_NAME) {
                listener = agentPointer->getActor()->GetDisplayFullName();
            }
        } catch (const std::exception& e) {
            logger::error("Error getting actor display name for {}: {}", agentPointer->getActorName(), e.what());
            if (listener != NARRATOR_NAME) {
                listener = agentPointer->getActorName();
            }
        }

        if (startsWithHey(msg)) {
            logger::info("Hey override detected in message: {}", msg);
            for (std::string agentname : aiam.getAgentsNamesFollowing()) {
                if (msg.contains(getFirstWord(agentname))) {
                    auto heyAgent = aiam.getAgentByName(agentname);
                    if (heyAgent && heyAgent->getActor()) {
                        agentPointer = heyAgent;
                        listenerPtr = heyAgent->getActor();
                        listener.assign(heyAgent->getActorName());
                        directedChat = true;
                        logger::info("Override by Hey NPC {}", agentname);
                    }
                }
            }
        }

        if (everyoneTargetOverride) {
            logger::info("[LISTENER-RESOLVE] Using Everyone chatbox target override");
        }

        if (agentPointer->hasConversationCooldown()) {
            logger::info("{} is on conversation cooldown, showing message", agentPointer->getActorName());
            std::string cooldownMsg = std::format("{} does not want to talk right now", agentPointer->getActorName());
            RE::DebugNotification(cooldownMsg.c_str());
            return;
        }

        auto position = player->GetPosition();
        auto forceNarratorListener = [&](const char* reason) {
            auto narrator = aiam.getAgentByName(NARRATOR_NAME);
            if (!narrator) {
                logger::warn("Narrator override requested ({}) but narrator agent was not available", reason);
                return false;
            }

            logger::info("Override by {}", reason);
            listenerPtr = narrator->getActor();
            listener.assign(NARRATOR_NAME);
            agentPointer = narrator;
            directedChat = true;
            narratorOverrideReason = reason ? reason : "unknown";
            return true;
        };


        // Hey Narrator override
        if (startsWithHeyNarrator(msg)) {
            forceNarratorListener("explicit call of narrator");
        }

        // VR: PlayerCamera->currentState->GetRotation returns degenerate quat in action_rework's
        // external camera, force-routing every utterance to Narrator. Read HMD node directly instead.
        auto narrator = aiam.getAgentByName(NARRATOR_NAME);
        if (narrator) {
            float pitchDegrees = 0.0f;
            bool pitchValid = false;

            if (REL::Module::IsVR()) {
                auto* player = RE::PlayerCharacter::GetSingleton();
                auto* nodeData = player ? player->GetVRNodeData() : nullptr;
                auto hmd = (nodeData && nodeData->UprightHmdNode) ?
                               nodeData->UprightHmdNode.get() :
                               nullptr;
                if (hmd) {
                    const RE::NiMatrix3& rot = hmd->world.rotate;
                    const float fz = std::clamp(rot.entry[2][1], -1.0f, 1.0f);
                    const float pitchRadians = -std::asin(fz);
                    pitchDegrees = pitchRadians * (180.0f / 3.141592654f);
                    pitchValid = std::isfinite(pitchDegrees);
                    logger::info("VR HMD pitch: {} degrees (forward.z={})", pitchDegrees, fz);
                } else {
                    logger::info("Skipping pitch-up narrator override: VR UprightHmdNode unavailable");
                }
            } else {
                auto camera = RE::PlayerCamera::GetSingleton();
                auto cameraState = camera ? camera->currentState.get() : nullptr;
                if (cameraState) {
                    RE::NiQuaternion rotation;
                    cameraState->GetRotation(rotation);
                    float pitchRadians = GetPitchFromQuaternionDebug(rotation);
                    pitchRadians = GetPitchFromQuaternion(rotation);
                    pitchDegrees = pitchRadians * (180.0f / 3.141592654f);
                    pitchValid = true;
                    logger::info("Camera pitch: {} degrees", pitchDegrees);
                }
            }

            if (pitchValid && pitchDegrees < -85.0f) {
                logger::info("Override by explicit camera pitch");
                listenerPtr = narrator->getActor();
                listener.assign(NARRATOR_NAME);
                agentPointer = narrator;
                directedChat = true;
            }
        }

        if (PrismaUIBridge::IsNarratorChatModeEnabled()) {
            forceNarratorListener("Narrator mode");
        }

        const bool useEveryoneBroadcast = everyoneTargetOverride && listener != NARRATOR_NAME;

        if (listener == NARRATOR_NAME) {
            // When talking to narrator, use narrator_inputtext to hide from NPCs
            if (msg.starts_with("ginputtext_s|") || msg.starts_with("inputtext_s|") ||
                msg.starts_with("ginputtext|") || msg.starts_with("inputtext|")) {
                size_t separatorPos = msg.find('|');
                if (separatorPos != std::string::npos) {
                    msg = std::string("narrator_inputtext") + msg.substr(separatorPos);
                }
            }
        } else if (useEveryoneBroadcast) {
            if (msg.starts_with("inputtext_s|")) {
                msg.replace(0, 11, "ginputtext_s");
                logger::info("[HTTPStream] Forced broadcast behavior for Everyone override via ginputtext_s (listener {})",
                             listener);
            } else if (msg.starts_with("inputtext|")) {
                msg.replace(0, 9, "ginputtext");
                logger::info("[HTTPStream] Forced broadcast behavior for Everyone override via ginputtext (listener {})",
                             listener);
            } else {
                logger::info("[HTTPStream] Everyone override active for listener {} but request type '{}' is not inputtext",
                             listener, msg.substr(0, msg.find('|')));
            }
        } else if (!directedChat) {
            // Keep default STT scoped to the resolved listener. Implicit broadcast
            // made the server keep continuing stale group context, so one NPC could
            // monopolize player speech even after the player tried to address others.
            logger::info("[HTTPStream] Undirected chat remains scoped to resolved listener {}. Use Everyone override for broadcast.",
                         listener);
        }
        // listenerPtr->GetActorRuntimeData().currentProcess->SetHeadtrackTarget(listenerPtr, position);

        if (listenerPtr) {
            auto callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>();
            auto args = RE::MakeFunctionArguments(std::move(listenerPtr), std::move(player));
            RE::BSScript::Internal::VirtualMachine::GetSingleton()->DispatchStaticCall("AIAgentAIMind", "LookAt", args,
                                                                                       callback);
        }
        if (agentPointer) {
            agentPointer->setClean(false);
            agentPointer->setRestored(false);
        }

        if (listenerPtr && agentPointer && listener != NARRATOR_NAME) {
            RefreshAIAgentInventory(listenerPtr, agentPointer->getActorName(), false, false);
        }

        std::string outboundMsg = msg;
        json speechLogPayload;
        bool shouldLogSpeech = false;
        double audienceSnapshotMs = 0.0;
        std::size_t audibleCompanionCount = 0;
        std::size_t spatialAudibilityCount = 0;
        const bool isPlayerInputRequest =
            msg.starts_with("inputtext") || msg.starts_with("ginputtext") || msg.starts_with("narrator_inputtext");
        const bool isSpatialSnapshotEligibleRequest =
            msg.starts_with("inputtext") || msg.starts_with("ginputtext");

        try {
            if (isPlayerInputRequest) {
                const auto audienceSnapshotStartedAt = std::chrono::steady_clock::now();
                float distance = minDistance;
                bool hasSpatialContext = false;
                AudibleActorDescriptor listenerSpatial{};
                if (listenerPtr) {
                    const auto listenerAudibleIt = audibleActorsByFormId.find(listenerPtr->GetFormID());
                    if (listenerAudibleIt != audibleActorsByFormId.end()) {
                        listenerSpatial = *listenerAudibleIt->second;
                        hasSpatialContext = true;
                        distance = listenerSpatial.airDistance;
                    }
                }

                std::vector<std::string> audibleCompanions;
                json spatialAudibility = json::array();

                for (const auto& candidateAgent : aiam.getAgents()) {
                    if (!candidateAgent) {
                        continue;
                    }

                    const std::string candidateName = candidateAgent->getActorName();
                    if (candidateName == NARRATOR_NAME) {
                        continue;
                    }

                    auto* candidateActor = candidateAgent->getActor();
                    if (!candidateActor || candidateActor->IsDead()) {
                        continue;
                    }

                    const float candidateDistance = player->GetPosition().GetDistance(candidateActor->GetPosition());
                    if (candidateDistance > spatialListenerDistanceLimit) {
                        continue;
                    }

                    const auto candidateAudibleIt = audibleActorsByFormId.find(candidateActor->GetFormID());
                    if (candidateAudibleIt == audibleActorsByFormId.end()) {
                        continue;
                    }
                    const auto& candidateSpatial = *candidateAudibleIt->second;

                    json candidateDebug;
                    candidateDebug["name"] = candidateName;
                    candidateDebug["can_communicate"] = true;
                    candidateDebug["volume"] = candidateSpatial.volume;
                    candidateDebug["reason"] = candidateSpatial.reason;
                    candidateDebug["distance"] = candidateSpatial.airDistance;
                    spatialAudibility.push_back(candidateDebug);

                    if (std::find(audibleCompanions.begin(), audibleCompanions.end(), candidateName) ==
                            audibleCompanions.end()) {
                        audibleCompanions.push_back(candidateName);
                    }
                }

                const std::string playerSpeaker = RE::PlayerCharacter::GetSingleton()->GetName();
                if (!playerSpeaker.empty() &&
                    std::find(audibleCompanions.begin(), audibleCompanions.end(), playerSpeaker) ==
                        audibleCompanions.end()) {
                    audibleCompanions.push_back(playerSpeaker);
                }
                audibleCompanionCount = audibleCompanions.size();
                spatialAudibilityCount = spatialAudibility.size();

                speechLogPayload["speaker"] = playerSpeaker;
                speechLogPayload["location"] = GetPlayerLocation();
                speechLogPayload["speech"] = msg_decode(outboundMsg);
                speechLogPayload["listener"] = listener;
                speechLogPayload["companions"] = audibleCompanions;
                speechLogPayload["distance"] = distance;
                speechLogPayload["spatial_can_communicate"] = hasSpatialContext;
                speechLogPayload["spatial_volume"] = hasSpatialContext ? listenerSpatial.volume : 0.0f;
                speechLogPayload["spatial_reason"] = hasSpatialContext ? listenerSpatial.reason : "no_listener_context";
                speechLogPayload["spatial_audibility"] = spatialAudibility;
                shouldLogSpeech = true;

                if (isSpatialSnapshotEligibleRequest) {
                    json audienceSnapshot;
                    audienceSnapshot["source"] = "plugin_spatial_input_v1";
                    audienceSnapshot["speaker"] = playerSpeaker;
                    audienceSnapshot["listener"] = listener;
                    audienceSnapshot["companions"] = audibleCompanions;
                    const std::string snapshotDump = audienceSnapshot.dump();
                    outboundMsg.append("|");
                    outboundMsg.append(base64_encode(snapshotDump.c_str(), snapshotDump.size()));
                }
                audienceSnapshotMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - audienceSnapshotStartedAt).count();
            }
        } catch (const std::exception& e) {
            logger::error("Exception preparing player audience snapshot: {}", e.what());
            outboundMsg = msg;
            shouldLogSpeech = false;
        }

        if (GodMode && isPlayerInputRequest) {
            // Godmode
            const auto dialogueStopGeneration = PrismaUIBridge::GetDialogueStopGeneration();
            ThreadPool::getInstance().enqueue(
                rechatDepth == 0 ? "HTTPStreamGodMode" : "HTTPStreamRechat",
                [outboundMsg, listener, rechatDepth, dialogueStopGeneration]() {
                    std::string finalMsg(base64_encode(outboundMsg.c_str(), std::strlen(outboundMsg.c_str())));
                    sendMsgStream(finalMsg.c_str(), false, listener, rechatDepth, true, dialogueStopGeneration);
                },
                listener, std::chrono::seconds(90));
        } else {
            // Fire the event
            const auto dialogueStopGeneration = PrismaUIBridge::GetDialogueStopGeneration();
            ThreadPool::getInstance().enqueue(
                rechatDepth == 0 ? "HTTPStream" : "HTTPStreamRechat",
                [outboundMsg, listener, rechatDepth, dialogueStopGeneration]() {
                    std::string finalMsg(base64_encode(outboundMsg.c_str(), std::strlen(outboundMsg.c_str())));
                    sendMsgStream(finalMsg.c_str(), false, listener, rechatDepth, false, dialogueStopGeneration);
                },
                listener, std::chrono::seconds(90));

            if (shouldLogSpeech) {
                try {
                    HTTPManager::log(
                        std::format("_speech|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(), speechLogPayload.dump()));
                } catch (const std::exception& e) {
                    logger::error("Exception in speech logging: {}", e.what());
                }
            }

            QueueInterruptNPC(listenerPtr, agentPointer, listener);
        }

       
    }

    void stream(std::string msg, RE::Actor* actor) { 
        stream(msg, actor, 0); 
    }

    void stream(std::string msg, RE::Actor* actor, int rechatDepth) {
        // Determine speaker
        logger::info("[HTTPStream] Streaming for actor: {} (rechat depth: {})", 
            actor ? actor->GetDisplayFullName() : "null", 
            rechatDepth);

        if (!actor) {
            logger::warn("[HTTPStream] Skipping stream with null actor");
            return;
        }

        const bool isCombatBark = msg.starts_with("combatbark|");
        AIAgentManager& aiam = AIAgentManager::getInstance();
        
        // First try to find agent by FormID (works for all agents including narrator)
        std::shared_ptr<AIAgent> agent = nullptr;
        const auto actorFormID = actor->GetFormID();
        for (const auto& a : aiam.getAgents()) {
            if (a->getActor() && a->getActor()->GetFormID() == actorFormID) {
                agent = a;
                break;
            }
        }
        
        // Use agent's name if found (handles narrator name override), otherwise use actor's display name
        std::string listener = agent ? agent->getActorName() : actor->GetDisplayFullName();
        
        logger::info("[HTTPStream] Setting dialogue busy for actor: {}, isPlayerTeammate: {}, resolved listener: {}", 
            actor->GetDisplayFullName(), 
            actor->IsPlayerTeammate(),
            listener);

        if (actor && agent && listener != NARRATOR_NAME) {
            RefreshAIAgentInventory(actor, agent->getActorName(), false, false);
        }

        // Rechat is launched while the current line may still be playing; do not interrupt it.
        if (rechatDepth > 0) {
            logger::trace("[HTTPStream] Rechat skips dialogue interrupt for {}", listener);
        } else if (!isCombatBark && msg.find("suggestion") == std::string::npos) {
            QueueInterruptNPC(actor, agent, listener);
        } else if (isCombatBark) {
            logger::trace("[HTTPStream] Combat bark skips dialogue interrupt for {}", listener);
        }
        logger::info("Stream Called for {}", listener);

        const auto dialogueStopGeneration = PrismaUIBridge::GetDialogueStopGeneration();
        ThreadPool::getInstance().enqueue(
            rechatDepth == 0 ? "HTTPStream" : "HTTPStreamRechat",
            [msg, listener, rechatDepth, dialogueStopGeneration]() {
                std::string finalMsg(base64_encode(msg.c_str(), std::strlen(msg.c_str())));
                sendMsgStream(finalMsg.c_str(), false, listener, rechatDepth, false, dialogueStopGeneration);
            },
            listener, std::chrono::seconds(90));
    }

}
