#include <DXGI.h>
#include <Windows.h>
#include <d3d11.h>

#include <fstream>
#include <iostream>

#include "Globals.h"
#include "HTTPUploader.h"
#include "HTTPManager.h"
#include "Misc.h"
#include "RE/Skyrim.h"
#include "ThreadPool.h"
#pragma comment(lib, "d3d11.lib")

namespace logger = SKSE::log;

std::string globalHints;

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

            // hints.append(ScenarioHints());

            if (MutexGetScreenShotSendMode() != 3) {
                auto cameraObject = RE::CrosshairPickData::GetSingleton()->target;
                if (cameraObject) {
                    hints.append("&fg=");
                    hints.append(cameraObject.get()->GetDisplayFullName());
                }
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

        // hints.append(ScenarioHints());

        if (MutexGetScreenShotSendMode() != 3) {
            auto cameraObject = RE::CrosshairPickData::GetSingleton()->target;
            if (cameraObject) {
                hints.append("&fg=");
                hints.append(cameraObject.get()->GetDisplayFullName());
            }
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
