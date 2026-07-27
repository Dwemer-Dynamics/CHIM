#include <DXGI.h>
#include <Windows.h>
#include <d3d11.h>

#include <chrono>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include "Globals.h"
#include "HTTPUploader.h"
#include "HTTPManager.h"
#include "Misc.h"
#include "RE/Skyrim.h"
#include "SpatialAwareness.h"
#include "ThreadPool.h"
#pragma comment(lib, "d3d11.lib")

namespace logger = SKSE::log;

std::string globalHints;

namespace {
    std::string EncodeVisualQueryValue(const std::string& value) {
        std::ostringstream encoded;
        encoded << std::uppercase << std::hex;
        for (unsigned char character : value) {
            if (std::isalnum(character) || character == '-' || character == '_' || character == '.' ||
                character == '~') {
                encoded << character;
            } else {
                encoded << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(character);
            }
        }
        return encoded.str();
    }

    std::string FormIdText(RE::FormID formId) {
        return std::format("{:08X}", static_cast<std::uint32_t>(formId));
    }

    std::string VisualSubjectType(RE::TESObjectREFR* reference) {
        if (reference->As<RE::Actor>()) {
            return "actor";
        }

        auto* baseObject = reference->GetBaseObject();
        if (baseObject && baseObject->GetFormType() == RE::FormType::Furniture) {
            return "furniture";
        }

        return "object";
    }

    std::string BuildVisualCaptureMetadata() {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return "&visual_type=scene&visual_perspective=first_person";
        }

        const std::string location = GetPlayerLocation();
        std::string subjectType = "scene";
        std::string subjectName;
        std::string subjectKey = "scene:" + location;
        std::string pluginName;
        std::string baseId;
        std::string refId;

        auto* crosshairData = RE::CrosshairPickData::GetSingleton();
        auto crosshairTarget = crosshairData ? crosshairData->target : RE::ObjectRefHandle{};
        if (crosshairTarget) {
            auto reference = crosshairTarget.get();
            if (reference) {
                subjectName = reference->GetDisplayFullName();
                refId = FormIdText(reference->GetFormID());
                subjectType = VisualSubjectType(reference.get());

                if (auto* baseObject = reference->GetBaseObject()) {
                    baseId = FormIdText(baseObject->GetLocalFormID());
                    if (auto* sourceFile = baseObject->GetFile(0)) {
                        pluginName = sourceFile->GetFilename();
                    }
                    subjectKey = subjectType + ":" + pluginName + ":" + baseId;
                } else {
                    subjectKey = subjectType + ":" + refId;
                }
            }
        }

        std::string cellId;
        if (auto* cell = player->GetParentCell()) {
            cellId = FormIdText(cell->GetFormID());
        }

        return "&visual_type=" + EncodeVisualQueryValue(subjectType) +
               "&visual_key=" + EncodeVisualQueryValue(subjectKey) +
               "&visual_name=" + EncodeVisualQueryValue(subjectName) +
               "&visual_plugin=" + EncodeVisualQueryValue(pluginName) +
               "&visual_baseid=" + EncodeVisualQueryValue(baseId) +
               "&visual_refid=" + EncodeVisualQueryValue(refId) +
               "&visual_cell=" + EncodeVisualQueryValue(cellId) +
               "&visual_location=" + EncodeVisualQueryValue(location) +
               "&visual_perspective=first_person";
    }
}

extern int MutexGetScreenShotSendMode();
extern void MutexSetScreenShotSendMode(int newVal);

struct ImageData {
    unsigned char* bmpData;
    int fileSize;
};

std::string ScenarioHints() {
    std::string buffer;

    auto player = RE::PlayerCharacter::GetSingleton();

    auto cell = player->GetParentCell();

    auto camera = RE::PlayerCamera::GetSingleton();

    if (const auto processLists = RE::ProcessLists::GetSingleton(); processLists) {
        for (auto& targetHandle : processLists->highActorHandles) {
            if (auto target = targetHandle.get(); target && target->GetActorRuntimeData().currentProcess) {
                bool hasLos = false;

                if (!target) continue;

                std::string actorLabel(target->GetName());
                auto reference = target.get();

                if (!reference) continue;
                if (reference->IsDisabled()) continue;
                if (!reference->Is3DLoaded()) continue;
                if (!reference->Is3rdPersonVisible()) continue;
                if (reference->IsDeleted()) continue;

                RE::Actor* t = reference;

                auto distance = player->GetPosition().Cross(reference->GetPosition());

                reference->HasLineOfSight(player, hasLos);

                if (hasLos && !actorLabel.empty()) {
                    auto actor = targetHandle.get().get();
                    // float distance = player->GetPosition().GetDistance(actor->GetPosition());

                    if (actor->IsDead()) actorLabel.append("(dead)");

                    if (buffer.empty()) buffer.append("This characters are currently visible: ");  // redundant?
                    logger::info("Scanning actors: {}", actorLabel);
                    buffer.append(actorLabel + ",");

                    // return RE::BSContainer::ForEachResult::kStop;
                }
            }
        }
    }

    return buffer;
}

ImageData* TakeShotToMemory() {
    logger::info("Init DirectX stuff");

    const auto renderer = RE::BSRenderManager::GetSingleton();

    if (!renderer) {
        logger::info("No renderer");
        return nullptr;
    }

    auto rendererData = renderer->GetRuntimeData();

    ID3D11Device* device = rendererData.forwarder;
    IDXGISwapChain* swapChain = rendererData.swapChain;
    ID3D11DeviceContext* ctx = rendererData.context;

    // device->GetImmediateContext(&ctx);

    logger::info("Getting swapchain desc...");
    DXGI_SWAP_CHAIN_DESC sd{};
    if (swapChain->GetDesc(std::addressof(sd)) < 0) {
        logger::error("IDXGISwapChain::GetDesc failed.");
        return nullptr;
    }

    // logger::info("Getting swapchain desc buffer count{}", sd.BufferCount);

    // Get the back buffer
    logger::info("Get the back buffer");
    ID3D11Texture2D* pBackBuffer = nullptr;
    swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);

    // logger::info("Create a staging texture for the back buffer");

    // Create a staging texture for the back buffer
    ID3D11Texture2D* pStagingTexture = nullptr;
    D3D11_TEXTURE2D_DESC desc;
    pBackBuffer->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.BindFlags = 0;

    // logger::info("D3D11_TEXTURE2D_DESC created");

    HRESULT res = device->CreateTexture2D(&desc, NULL, &pStagingTexture);
    if (FAILED(res)) {
        logger::info("Unable to CreateTexture2D");
        return nullptr;
    }

    if (pStagingTexture == nullptr) {
        logger::info("pStagingTexture = nullptr");
        return nullptr;
    }

    // Copy the back buffer to the staging texture

    ctx->CopyResource(pStagingTexture, pBackBuffer);

    D3D11_MAPPED_SUBRESOURCE mappedResource;
    ZeroMemory(&mappedResource, sizeof(D3D11_SUBRESOURCE_DATA));

    if (pStagingTexture) {
        res = ctx->Map(pStagingTexture, 0, D3D11_MAP_READ, 0, &mappedResource);
        if (FAILED(res)) {
            logger::info("FAILED Map the staging texture to access pixel data");
            return nullptr;
        }
    } else {
        logger::info("pStagingTexture is invalid");
        return nullptr;
    }

    logger::info("Backbuffer format: {} " , desc.Format);
    /*
    // BMP header
    const int bpp = 4;
    const int width = desc.Width;
    const int height = desc.Height;
    const int imageSize = width * height * bpp;  // Assuming 32-bit RGBA format
    const int fileSize = 54 + imageSize;

    unsigned char* bmpData = new unsigned char[fileSize];
    memset(bmpData, 0, fileSize);

    bmpData[0] = 'B';
    bmpData[1] = 'M';
    *((int*)(bmpData + 2)) = fileSize;
    *((int*)(bmpData + 10)) = 54;
    *((int*)(bmpData + 14)) = 40;
    *((int*)(bmpData + 18)) = width;
    *((int*)(bmpData + 22)) = height;
    *((int*)(bmpData + 26)) = 1;
    *((int*)(bmpData + 28)) = 32;
    *((int*)(bmpData + 34)) = imageSize;

    for (int i = 0; i < imageSize; i += bpp) {
        bmpData[54 + i] = ((unsigned char*)mappedResource.pData)[i + 2];
        bmpData[55 + i] = ((unsigned char*)mappedResource.pData)[i + 1];
        bmpData[56 + i] = ((unsigned char*)mappedResource.pData)[i];
        bmpData[57 + i] = 0xFF;  // Alpha channel, set to 255 (fully opaque)
    }
    */


    // ChatGPT code
    const int bpp = 4;
    const int width = desc.Width;
    const int height = desc.Height;
    const int pitch = width * bpp;         // Calculate the pitch (bytes per row)
    const int imageSize = pitch * height;  // Assuming 32-bit RGBA format
    const int fileSize = 54 + imageSize;

    unsigned char* bmpData = new unsigned char[fileSize];
    memset(bmpData, 0, fileSize);

    bmpData[0] = 'B';
    bmpData[1] = 'M';
    *((int*)(bmpData + 2)) = fileSize;
    *((int*)(bmpData + 10)) = 54;
    *((int*)(bmpData + 14)) = 40;
    *((int*)(bmpData + 18)) = width;
    *((int*)(bmpData + 22)) = height;
    *((int*)(bmpData + 26)) = 1;
    *((int*)(bmpData + 28)) = 32;
    *((int*)(bmpData + 34)) = imageSize;

    unsigned char* pSrc = (unsigned char*)mappedResource.pData;
    unsigned char* pDst = bmpData + 54;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int srcIndex = y * mappedResource.RowPitch + x * bpp;
            int dstIndex = y * pitch + x * bpp;

            pDst[dstIndex + 0] = pSrc[srcIndex + 2];  // B
            pDst[dstIndex + 1] = pSrc[srcIndex + 1];  // G
            pDst[dstIndex + 2] = pSrc[srcIndex + 0];  // R
            pDst[dstIndex + 3] = 0xFF;                // Alpha channel, set to 255 (fully opaque)
        }
    }


    ctx->Unmap(pStagingTexture, 0);
    // delete pMappedResource;

    // Release resources

    pBackBuffer->Release();
    pStagingTexture->Release();
    // logger::info("End of DirectX stuff");
    //  Release your DirectX resources and clean up

    ImageData* result = new ImageData;
    result->bmpData = bmpData;
    result->fileSize = fileSize;

    return result;
}

void FreeImageData(ImageData* imageData) {
    if (imageData) {
        if (imageData->bmpData) {
            delete[] imageData->bmpData;
            imageData->bmpData = nullptr;  // Set to nullptr to avoid double deletion
        }
        delete imageData;  // Free the ImageData structure itself
    }
}

void ProcedureTakeShot() {
    logger::info("Take a shot");

    ImageData* id = TakeShotToMemory();

    if (id == nullptr)
        logger::info("Take a shot failed");

    else {
        ThreadPool::getInstance().enqueue("ProcessScreenshot", [id]() {
            /* std::ofstream outputFile("eyeShot.bmp", std::ios::out | std::ios::binary);
            if (outputFile.is_open()) {
                outputFile.write(reinterpret_cast<const char*>(id->bmpData), id->fileSize);
                outputFile.close();
            }*/
            logger::info(" HTTPUploader::getInstance()");
            auto player = RE::PlayerCharacter::GetSingleton();
            auto result = InspectSurroundings(player->AsReference(), true, HERIKA_MAX_VISION_RANGE, ",",
                                              DISTANCE_ACTIVATING_NPC_OUT);
            HTTPManager::log(std::format("infonpc|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                         "(beings in range:" + result + ")"));

            HTTPUploader& uploader = HTTPUploader::getInstance();

            std::string rawdata = reinterpret_cast<const char*>(id->bmpData);
            logger::info("Uploading...");

            std::string hints;

            hints.append("&vc=" + globalHints);
            hints.append(BuildVisualCaptureMetadata());

            // hints.append(ScenarioHints());

            auto cameraObject = RE::CrosshairPickData::GetSingleton()->target;
            if (cameraObject) {
                hints.append("&fg=");
                hints.append(cameraObject.get()->GetDisplayFullName());
            }

            std::string buffer = uploader.UploadImage(reinterpret_cast<const char*>(id->bmpData), id->fileSize, hints);
            logger::info("Done");
            FreeImageData(id);

             if (MutexGetScreenShotSendMode() == 0)
                HTTPManager::stream(std::format("vision|{}|{}|{} {}", getCurrentTimeMillis(), GetGameTimeStamp(),
                              "(Context location: " + std::string(GetPlayerLocation()) + ")", buffer));
             
             MutexSetScreenShotSendMode(0);
        });
    }
    return;
}

void ProcedureSendShot(char const* a_path) {
    logger::info("Send  a shot");

    ThreadPool::getInstance().enqueue("UploadScreenshot", [a_path]() {
        logger::info(" HTTPUploader::getInstance()");

        HTTPUploader& uploader = HTTPUploader::getInstance();

        auto player = RE::PlayerCharacter::GetSingleton();
        auto result =
            InspectSurroundings(player->AsReference(), true, HERIKA_MAX_VISION_RANGE, ",", DISTANCE_ACTIVATING_NPC_OUT);
        HTTPManager::log(std::format("infonpc|{}|{}|{}", getCurrentTimeMillis(), GetGameTimeStamp(),
                                     "(beings in range:" + result + ")"));

        std::ifstream binaryFile(a_path, std::ios::binary);

        if (!binaryFile.is_open()) {
            logger::info("Failed to open the binary file. {}", a_path);
            return ;
        }

        // Get the size of the file
        binaryFile.seekg(0, std::ios::end);
        std::streampos fileSize = binaryFile.tellg();
        binaryFile.seekg(0, std::ios::beg);

        // Allocate a buffer to store the binary data
        char* prebuffer = new char[fileSize];

        // Read the binary data into the buffer
        binaryFile.read(prebuffer, fileSize);

        // Close the binary file
        binaryFile.close();

        logger::info("Uploading...");

        std::string hints;

        hints.append("&vc=" + globalHints);
        hints.append(BuildVisualCaptureMetadata());

        // hints.append(ScenarioHints());

        auto cameraObject = RE::CrosshairPickData::GetSingleton()->target;
        if (cameraObject) {
            hints.append("&fg=");
            hints.append(cameraObject.get()->GetDisplayFullName());
        }

        std::string buffer = uploader.UploadImagePng(reinterpret_cast<const char*>(prebuffer), fileSize, hints);
        logger::info("Done");

        delete[] prebuffer;

        if (MutexGetScreenShotSendMode() == 0)
                   HTTPManager::stream(std::format("vision|{}|{}|{} {}", getCurrentTimeMillis(), GetGameTimeStamp(),
                          "(Context location: " + std::string(GetPlayerLocation()) + ")", buffer));

        MutexSetScreenShotSendMode(0);
    });

    return;
}

// ---------------------------------------------------------------------------------------------------
// SHARMAT gaze / staring detection. This file is CHIM's "what is being looked at" module (it already
// reads CrosshairPickData for screenshots and scenario hints), so player-gaze detection lives here too.
// The player resting the crosshair on an NPC for a sustained moment emits a gaze event down the same
// physics_raw pipe as VR body contact (plain name per upstream review: DLL events are not ext_*;
// core logs it as a fast command, the SHARMAT server extension renames + turns it into an
// in-character reaction). The DLL only detects "player stared at <region> of <actor>" - the server owns
// every reaction, prompt, and relationship/scene/child gate. Inert without that extension. VR gaze is
// always eligible; flatscreen gaze is eligible only while the camera is actually first-person.
// ---------------------------------------------------------------------------------------------------
namespace {
    constexpr float       kGazeSeconds        = 6.0f;    // continuous dwell before a gaze fires
    constexpr float       kGazeDwellGrace     = 0.75f;   // VR: brief target loss (head jitter) that does NOT reset the dwell
    constexpr float       kGazeDistance       = 350.0f;  // max player<->target distance (game units)
    constexpr float       kGazeCooldown       = 25.0f;   // matches SHARMAT's default server-side gaze cooldown
    constexpr float       kGazeNodeMaxDist    = 45.0f;   // hit must be within this of a mapped node, else "person"
    constexpr const char* kGazePhysicsEvent   = "physics_raw"; // same pipe as touch/grab/spank (plain name, see header note)

    std::mutex                                                            g_gazeMutex;
    RE::FormID                                                            g_gazeDwellActor = 0;
    std::chrono::steady_clock::time_point                                 g_gazeDwellStart{};
    std::chrono::steady_clock::time_point                                 g_gazeLastSeen{};
    bool                                                                  g_gazeFired = false; // fired for current dwell
    std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> g_lastGazeEmit;

    bool IsGazeCameraEligible() {
        // Never track gaze during a scripted conversation: in dialogue the player stares at the
        // NPC's face far past the dwell threshold, and the resulting reaction is a full model
        // turn that stomps the vanilla dialogue state (the NPC wedges "busy" and quest dialogue
        // can never resume). The PollPlayerGaze ineligible path also resets the dwell, so
        // closing the menu never fires a stare accumulated while it was open.
        if (auto* ui = RE::UI::GetSingleton(); ui && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME)) {
            return false;
        }

        if (REL::Module::IsVR()) { return true; }

        auto* camera = RE::PlayerCamera::GetSingleton();
        return camera && camera->IsInFirstPerson();
    }

    std::string CleanGazeField(std::string s) {
        for (auto& c : s) {
            if (c == '^' || c == '|' || c == '\n' || c == '\r' || c == '\t') { c = ' '; }
        }
        return s;
    }

    struct GazeNodeCand { const char* node; const char* region; };
    constexpr GazeNodeCand kGazeNodeCands[] = {
        { "NPC Head [Head]",     "eyes"   },
        { "NPC L Breast",        "tits"   },
        { "NPC R Breast",        "tits"   },
        { "NPC Spine2 [Spn2]",   "tits"   }, // chest fallback when breast nodes are absent
        { "NPC L Butt",          "ass"    },
        { "NPC R Butt",          "ass"    },
        { "NPC GenitalsScrotum", "crotch" },
        { "NPC Pelvis [Pelv]",   "crotch" }, // lower fallback
    };

    // Classify the gazed region by the body node nearest the crosshair hit point.
    // Returns "eyes" | "tits" | "ass" | "crotch" | "person".
    std::string ClassifyGazeRegion(RE::Actor* actor, const RE::NiPoint3& hit) {
        auto* root = actor->Get3D();
        if (!root) { return "person"; }
        float       best       = 1.0e30f;
        const char* bestRegion = "person";
        for (const auto& c : kGazeNodeCands) {
            auto* n = root->GetObjectByName(c.node);
            if (!n) { continue; }
            const RE::NiPoint3 d    = n->world.translate - hit;
            const float        dist = d.Length();
            if (dist < best) {
                best       = dist;
                bestRegion = c.region;
            }
        }
        if (best > kGazeNodeMaxDist) { return "person"; } // hit far from any mapped node -> general staring
        return bestRegion;
    }

    // ---- VR path: CrosshairPickData is the wrong signal there (targetActor tracks the ACTIVATION/hand
    // ray, and collisionPoint reads through the SSE struct layout are not valid on the VR runtime), so
    // VR gaze is driven by the HMD ray instead: dwell target = the actor nearest the view axis, region =
    // the body node nearest the ray. ----

    // Perpendicular distance from ray (origin, unit dir) to a point; points behind the origin never match.
    float RayPointDistance(const RE::NiPoint3& origin, const RE::NiPoint3& dir, const RE::NiPoint3& p) {
        const RE::NiPoint3 v = p - origin;
        const float        t = v.Dot(dir);
        if (t <= 0.0f) { return 1.0e30f; }
        const RE::NiPoint3 closest{ origin.x + dir.x * t, origin.y + dir.y * t, origin.z + dir.z * t };
        return (p - closest).Length();
    }

    std::string ClassifyGazeRegionByRay(RE::Actor* actor, const RE::NiPoint3& origin, const RE::NiPoint3& dir) {
        auto* root = actor->Get3D();
        if (!root) { return "person"; }
        float       best       = 1.0e30f;
        const char* bestRegion = "person";
        for (const auto& c : kGazeNodeCands) {
            auto* n = root->GetObjectByName(c.node);
            if (!n) { continue; }
            const float dist = RayPointDistance(origin, dir, n->world.translate);
            if (dist < best) {
                best       = dist;
                bestRegion = c.region;
            }
        }
        if (best > kGazeNodeMaxDist) { return "person"; } // ray passes far from every mapped node
        return bestRegion;
    }

    // Which actor is the HMD ray resting on? Nearest to the view axis within the cone and range.
    // Safe off the game thread: uses only the paced camera snapshot + actor ref positions.
    RE::FormID VrPickGazeActor(const RE::NiPoint3& origin, const RE::NiPoint3& dir) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* lists  = RE::ProcessLists::GetSingleton();
        if (!player || !lists) { return 0; }
        constexpr float kGazeConeMinCos = 0.94f;  // ~20 degree half-angle
        float      bestCos = kGazeConeMinCos;
        RE::FormID best    = 0;
        for (auto& handle : lists->highActorHandles) {
            auto actorPtr = handle.get();
            RE::Actor* actor = actorPtr.get();
            if (!actor || actor == player) { continue; }
            if (actor->IsDead() || actor->IsDisabled() || !actor->Is3DLoaded()) { continue; }
            RE::NiPoint3 to = actor->GetPosition();
            to.z += 96.0f;  // aim at the torso/head band, not the feet
            to -= origin;
            const float dist = to.Length();
            if (dist < 1.0f || dist > kGazeDistance) { continue; }
            const float cosAngle = to.Dot(dir) / dist;
            if (cosAngle > bestCos) {
                bestCos = cosAngle;
                best    = actor->GetFormID();
            }
        }
        return best;
    }

    // Runs on the GAME THREAD (scene-graph node reads are unsafe off-thread). Re-verifies the target,
    // gates, and emits the gaze event on a worker thread.
    void FireGazeOnGameThread(RE::FormID expectActor, float seconds) {
        // PollPlayerGaze runs on CHIM's manager thread. Re-check the camera here so a flatscreen
        // player who changed to third-person while the task was queued cannot emit a stale gaze.
        if (!IsGazeCameraEligible()) { return; }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) { return; }

        RE::Actor*  actor = nullptr;
        std::string region;
        if (REL::Module::IsVR()) {
            // Game thread: the live camera-root read is tear-free here, and the ray re-verify
            // catches a head that moved off the target while the task was queued.
            RE::NiPoint3 origin{}, dir{};
            if (!SpatialAwareness::GetPlayerCameraGaze(origin, dir)) { return; }
            if (VrPickGazeActor(origin, dir) != expectActor) { return; }  // looked away
            auto* form = RE::TESForm::LookupByID(expectActor);
            actor = form ? form->As<RE::Actor>() : nullptr;
            if (!actor || actor == player || actor->IsDead()) { return; }
            region = ClassifyGazeRegionByRay(actor, origin, dir);
        } else {
            auto* pick = RE::CrosshairPickData::GetSingleton();
            if (!pick) { return; }
            auto actorPtr = pick->targetActor.get();  // ObjectRefHandle -> NiPointer<TESObjectREFR>
            actor = actorPtr ? actorPtr->As<RE::Actor>() : nullptr;
            if (!actor || actor->GetFormID() != expectActor) { return; }  // player looked away / not an actor
            if (actor == player || actor->IsDead()) { return; }
            region = ClassifyGazeRegion(actor, pick->collisionPoint);
        }

        const RE::NiPoint3 delta = actor->GetPosition() - player->GetPosition();
        if (delta.Length() > kGazeDistance) { return; }

        // Lewd regions (tits/ass/crotch) only fire for a MALE player staring at a FEMALE NPC; otherwise
        // the gaze degrades to general "person" staring. Eyes-gaze fires for any pairing.
        const bool playerMale = player->GetActorBase() && player->GetActorBase()->GetSex() == RE::SEXES::kMale;
        const bool npcFemale  = actor->GetActorBase() && actor->GetActorBase()->GetSex() == RE::SEXES::kFemale;
        if ((region == "tits" || region == "ass" || region == "crotch") && !(playerMale && npcFemale)) {
            region = "person";
        }

        std::string actorName = CleanGazeField(std::string(actor->GetDisplayFullName()));
        if (actorName.empty()) { return; }
        const int playerSex = playerMale ? 0 : 1;

        // rawData mirrors the physics touch layout so it flows through the same server pipe:
        //   actor ^ region ^ gaze ^ blocked(false) ^ blockedby() ^ seconds ^ playerSex
        const std::string rawData =
            std::format("{}^{}^gaze^false^^{:.0f}^{}", actorName, region, seconds, playerSex);

        ThreadPool::getInstance().enqueue(
            "SharmatGaze",
            [rawData]() {
                HTTPManager::log(std::format("{}|{}|{}|{}", kGazePhysicsEvent, getCurrentTimeMillis(),
                                             GetGameTimeStamp(), rawData));
            },
            "gaze:" + rawData);
    }
}

// Called once per ManagerMainQueue pass (alongside the other awareness ticks). Tracks how long the
// crosshair has dwelled on the same actor and, past a threshold, marshals a game-thread read to
// classify the gazed region and emit. Cheap; does its own dwell/cooldown bookkeeping.
void PollPlayerGaze() {
    if (!IsGazeCameraEligible()) {
        // Do not carry a partial first-person dwell through time spent in third-person. Starting a
        // new first-person view must earn the complete dwell interval before it can emit a gaze.
        std::lock_guard<std::mutex> lk(g_gazeMutex);
        g_gazeDwellActor = 0;
        g_gazeFired      = false;
        return;
    }

    if (!RE::PlayerCharacter::GetSingleton()) { return; }

    RE::FormID cur = 0;
    if (REL::Module::IsVR()) {
        // Manager thread: the paced snapshot is the only safe camera read here (a live
        // camRoot read off the game thread returns torn/static transforms in VR).
        RE::NiPoint3 origin{}, dir{};
        if (!SpatialAwareness::GetPlayerCameraGaze(origin, dir)) { return; }
        cur = VrPickGazeActor(origin, dir);
    } else {
        auto* pick = RE::CrosshairPickData::GetSingleton();
        if (!pick) { return; }
        auto actorPtr = pick->targetActor.get();
        if (actorPtr) { cur = actorPtr->GetFormID(); }
    }

    const auto                  now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(g_gazeMutex);

    if (cur == 0) {  // not looking at any actor
        // VR head jitter flicks the cone off the target for a frame or two; a brief loss keeps
        // the dwell alive so a natural stare can actually accumulate the full interval.
        if (g_gazeDwellActor != 0 &&
            std::chrono::duration<float>(now - g_gazeLastSeen).count() < kGazeDwellGrace) {
            return;
        }
        g_gazeDwellActor = 0;
        g_gazeFired      = false;
        return;
    }
    if (cur != g_gazeDwellActor) {  // switched target -> restart dwell
        g_gazeDwellActor = cur;
        g_gazeDwellStart = now;
        g_gazeLastSeen   = now;
        g_gazeFired      = false;
        return;
    }
    g_gazeLastSeen = now;
    if (g_gazeFired) { return; }  // already emitted for this continuous dwell

    const float elapsed = std::chrono::duration<float>(now - g_gazeDwellStart).count();
    if (elapsed < kGazeSeconds) { return; }

    auto it = g_lastGazeEmit.find(cur);
    if (it != g_lastGazeEmit.end() &&
        std::chrono::duration<float>(now - it->second).count() < kGazeCooldown) {
        g_gazeFired = true;  // on cooldown; don't recheck until target changes
        return;
    }
    g_lastGazeEmit[cur] = now;
    g_gazeFired         = true;

    SKSE::GetTaskInterface()->AddTask([cur, elapsed]() { FireGazeOnGameThread(cur, elapsed); });
}
