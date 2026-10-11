#include "FacialAnimationBridge.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {
void Check(bool condition)
{
    if (!condition) {
        std::cerr << "FacialAnimationBridge check failed" << std::endl;
        std::exit(1);
    }
}

std::uint32_t owner = BFA_kOwnerEngine;
bool ready = true;
bool decline = false;
int stops = 0;
int moods = 0;
int pauses = 0;
double position = 0;
BFA_Line prepared{};
BFA_Emotion mood{};
BFA_Interface MakeApi()
{
    BFA_Interface api{};
    api.version = BFA_API_VERSION;
    api.size = sizeof(api);
    api.IsReady = [] { return ready; };
    api.Owner = [](std::uint32_t) { return owner; };
    api.Prepare = [](const BFA_Line* line, std::uint32_t* reason) -> std::uint64_t {
        prepared = *line;
        *reason = decline ? BFA_kRaceDisabled : BFA_kAccepted;
        return decline ? 0 : 42;
    };
    api.Start = [](std::uint64_t, double offset) { position = offset; owner = BFA_kOwnerProvider; return true; };
    api.Pause = [](std::uint64_t) { ++pauses; return true; };
    api.Resume = [](std::uint64_t, double offset) { position = offset; return true; };
    api.Seek = api.Resume;
    api.Stop = [](std::uint64_t handle, std::uint32_t reason) {
        Check(handle == 42 && reason <= BFA_kAborted);
        ++stops; owner = BFA_kOwnerEngine; return true;
    };
    api.SetMood = [](std::uint32_t actor, const BFA_Emotion* value, float) {
        Check(actor == 0x123); ++moods; mood = *value; return true;
    };
    return api;
}
}

int main()
{
    using namespace FacialAnimationBridge;
    const char* expressions[] = {"MoodNeutral", "MoodAnger", "MoodDisgusted", "MoodFear", "MoodSad",
                                 "MoodHappy", "MoodSurprise", "MoodPuzzled"};
    for (int i = 0; i < 8; ++i) {
        Check(MakeMood(expressions[i]).skyrim_type == i);
    }
    Check(MakeMood("DialogueHappy").skyrim_type == 5);
    Check(MakeMood("CombatAnger").skyrim_type == 1);
    Check(MakeMood("unknown").gain == 0);
    auto api = MakeApi();
    Check(IsCompatible(&api));
    auto shortApi = api;
    shortApi.size = offsetof(BFA_Interface, SetMood);
    Check(!IsCompatible(&shortApi));
    shortApi = api; shortApi.Seek = nullptr;
    Check(!IsCompatible(&shortApi));
    const char wav[] = "RIFF audio fixture";
    std::shared_ptr<FaceToken> queued;
    {
        Line line(&api, 0x123);
        queued = line.GetToken();
        Check(queued->CanWrite());
        Check(line.Prepare(wav, sizeof(wav), "hello", "MoodHappy") == BFA_kAccepted);
        Check(prepared.audio.data == wav && prepared.audio.bytes == sizeof(wav));
        Check(prepared.audio.format == BFA_kContainer && prepared.audio.path == nullptr);
        Check(prepared.speaker_form_id == 0x123 && prepared.flags == BFA_kLastSegment);
        Check(prepared.emotion.skyrim_type == 5 && mood.skyrim_type == 5 && moods == 1);
        line.Start(0.02);
        Check(position == 0.02 && !queued->CanWrite());
        line.Pause(); Check(pauses == 1);
        line.Resume(0.5); Check(position == 0.5);
        line.Seek(0.7); Check(position == 0.7);
        // Async inference failure/decline releases ownership; fallback recovers.
        owner = BFA_kOwnerEngine; Check(queued->CanWrite());
        owner = BFA_kOwnerSuppressed; Check(!queued->CanWrite());
        line.Stop(BFA_kSkipped);
        Check(!queued->CanWrite() && stops == 1);
    }
    Check(stops == 1 && !queued->CanWrite());
    {
        Line absent(nullptr, 0x123);
        Check(absent.Prepare(wav, sizeof(wav), "hello", "") == BFA_kDisabled);
        Check(absent.GetToken()->CanWrite());
        absent.Start(0); absent.Pause(); absent.Resume(0); absent.Seek(1);
    }
    decline = true;
    {
        Line declined(&api, 0x123);
        Check(declined.Prepare(wav, sizeof(wav), "hello", "MoodSad") == BFA_kRaceDisabled);
        Check(declined.GetToken()->CanWrite() && moods == 1);
    }
    decline = false;
    {
        Line aborted(&api, 0x123);
        aborted.Prepare(wav, sizeof(wav), "hello", "");
        Check(moods == 1);
    }
    Check(stops == 2);
    ready = false;
    Line disabled(&api, 0x123);
    Check(disabled.Prepare(wav, sizeof(wav), "hello", "MoodSad") == BFA_kDisabled);
    Check(disabled.GetToken()->CanWrite());
    std::cout << "FacialAnimationBridge tests passed\n";
}
