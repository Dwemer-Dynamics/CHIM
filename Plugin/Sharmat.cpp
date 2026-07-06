#include "Sharmat.h"

#include "Globals.h"      // RE/Skyrim.h umbrella + logger
#include "HTTPManager.h"  // HTTPManager::log
#include "Misc.h"         // getCurrentTimeMillis(), GetGameTimeStamp()
#include "ThreadPool.h"   // ThreadPool::getInstance().enqueue

#include <chrono>
#include <cmath>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {
    // -----------------------------------------------------------------------------------------------
    // Tunables (v1 defaults). Deliberately conservative. The SERVER owns every reaction, prompt, and the
    // relationship / scene / child gating - the DLL only detects "player stared at <region> of <actor>".
    // -----------------------------------------------------------------------------------------------
    constexpr float       kGazeSeconds        = 6.0f;    // continuous dwell before a gaze fires
    constexpr float       kGazeDistance       = 350.0f;  // max player<->target distance (game units)
    constexpr float       kGazeCooldown       = 20.0f;   // seconds between gaze events for the same actor
    constexpr float       kNodeMaxDist        = 45.0f;   // hit must be within this of a mapped node, else "person"
    constexpr const char* kNsfwPhysicsEvent   = "ext_nsfw_physics_raw"; // same pipe as touch/grab/spank

    std::mutex                                                        g_gazeMutex;
    RE::FormID                                                        g_dwellActor = 0;
    std::chrono::steady_clock::time_point                            g_dwellStart{};
    bool                                                              g_fired = false; // fired for current dwell
    std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> g_lastEmit;

    std::string CleanField(std::string s) {
        for (auto& c : s) {
            if (c == '^' || c == '|' || c == '\n' || c == '\r') { c = ' '; }
        }
        return s;
    }

    // Classify the gazed region by the body node nearest the crosshair hit point.
    // Returns "eyes" | "tits" | "ass" | "crotch" | "person".
    std::string ClassifyRegion(RE::Actor* actor, const RE::NiPoint3& hit) {
        auto* root = actor->Get3D();
        if (!root) { return "person"; }
        struct Cand { const char* node; const char* region; };
        static const Cand cands[] = {
            { "NPC Head [Head]",     "eyes"   },
            { "NPC L Breast",        "tits"   },
            { "NPC R Breast",        "tits"   },
            { "NPC Spine2 [Spn2]",   "tits"   }, // chest fallback when breast nodes are absent
            { "NPC L Butt",          "ass"    },
            { "NPC R Butt",          "ass"    },
            { "NPC GenitalsScrotum", "crotch" },
            { "NPC Pelvis [Pelv]",   "crotch" }, // lower fallback
        };
        float       best       = 1.0e30f;
        const char* bestRegion = "person";
        for (const auto& c : cands) {
            auto* n = root->GetObjectByName(c.node);
            if (!n) { continue; }
            const RE::NiPoint3 d    = n->world.translate - hit;
            const float        dist = d.Length();
            if (dist < best) {
                best       = dist;
                bestRegion = c.region;
            }
        }
        if (best > kNodeMaxDist) { return "person"; } // hit far from any mapped node -> general staring
        return bestRegion;
    }

    // Runs on the GAME THREAD (scene-graph node reads are unsafe off-thread). Re-verifies the target, gates,
    // and emits the gaze event on a worker thread.
    void FireGazeOnGameThread(RE::FormID expectActor, float seconds) {
        auto* pick = RE::CrosshairPickData::GetSingleton();
        if (!pick) { return; }
        auto        actorPtr = pick->targetActor.get();       // ObjectRefHandle -> NiPointer<TESObjectREFR>
        RE::Actor*  actor    = actorPtr ? actorPtr->As<RE::Actor>() : nullptr;
        if (!actor || actor->GetFormID() != expectActor) { return; } // player looked away / not an actor
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || actor == player || actor->IsDead()) { return; }

        const RE::NiPoint3 delta = actor->GetPosition() - player->GetPosition();
        if (delta.Length() > kGazeDistance) { return; }

        const RE::NiPoint3 hit    = pick->collisionPoint;
        std::string        region = ClassifyRegion(actor, hit);

        // Lewd regions (tits/ass/crotch) only fire for a MALE player staring at a FEMALE NPC; otherwise the
        // gaze degrades to general "person" staring. Eyes-gaze fires for any pairing.
        const bool playerMale = player->GetActorBase() && player->GetActorBase()->GetSex() == RE::SEXES::kMale;
        const bool npcFemale  = actor->GetActorBase() && actor->GetActorBase()->GetSex() == RE::SEXES::kFemale;
        if ((region == "tits" || region == "ass" || region == "crotch") && !(playerMale && npcFemale)) {
            region = "person";
        }

        std::string actorName = CleanField(std::string(actor->GetDisplayFullName()));
        if (actorName.empty()) { return; }
        const int playerSex = playerMale ? 0 : 1;

        // rawData mirrors the physics touch layout so it flows through the same server pipe:
        //   actor ^ region ^ gaze ^ blocked(false) ^ blockedby() ^ seconds ^ playerSex
        const std::string rawData =
            std::format("{}^{}^gaze^false^^{:.0f}^{}", actorName, region, seconds, playerSex);

        ThreadPool::getInstance().enqueue(
            "SharmatGaze",
            [rawData]() {
                HTTPManager::log(std::format("{}|{}|{}|{}", kNsfwPhysicsEvent, getCurrentTimeMillis(),
                                             GetGameTimeStamp(), rawData));
            },
            "gaze:" + rawData);
    }
}

namespace Sharmat {
    void PollGaze() {
        auto* pick = RE::CrosshairPickData::GetSingleton();
        if (!pick) { return; }
        if (!RE::PlayerCharacter::GetSingleton()) { return; }

        RE::FormID cur = 0;
        {
            auto actorPtr = pick->targetActor.get();
            if (actorPtr) { cur = actorPtr->GetFormID(); }
        }

        const auto                   now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex>  lk(g_gazeMutex);

        if (cur == 0) { // not looking at any actor
            g_dwellActor = 0;
            g_fired      = false;
            return;
        }
        if (cur != g_dwellActor) { // switched target -> restart dwell
            g_dwellActor = cur;
            g_dwellStart = now;
            g_fired      = false;
            return;
        }
        if (g_fired) { return; } // already emitted for this continuous dwell

        const float elapsed = std::chrono::duration<float>(now - g_dwellStart).count();
        if (elapsed < kGazeSeconds) { return; }

        auto it = g_lastEmit.find(cur);
        if (it != g_lastEmit.end() &&
            std::chrono::duration<float>(now - it->second).count() < kGazeCooldown) {
            g_fired = true; // on cooldown; don't recheck until target changes
            return;
        }
        g_lastEmit[cur] = now;
        g_fired         = true;

        SKSE::GetTaskInterface()->AddTask([cur, elapsed]() { FireGazeOnGameThread(cur, elapsed); });
    }
}
