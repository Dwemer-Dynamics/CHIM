#include "FacialAnimationBridge.h"

#include <cstdlib>
#include <iostream>

namespace {
void Check(bool condition)
{
    if (!condition) {
        std::cerr << "FacialAnimationBridge check failed" << std::endl;
        std::exit(1);
    }
}
std::uint32_t owner = BFA_SpeechOwnerEngine;
int stops = 0, endings = 0, moods = 0;
double position = 0;
BFA_SpeechLine prepared{};
BFA_SpeechEmotion mood{};
BFA_SpeechCallback callback = nullptr;
void* context = nullptr;
void Event(std::uint32_t type)
{
    BFA_SpeechEvent event{1, sizeof(BFA_SpeechEvent), type, 0, 42,
                         prepared.utterance_id, prepared.segment, prepared.speaker_form_id};
    callback(&event, context);
}
BFA_SpeechInterface MakeApi()
{
    BFA_SpeechInterface api{};
    api.version = 1;
    api.size = sizeof(api);
    api.Owner = [](std::uint32_t) { return owner; };
    api.RegisterListener = [](BFA_SpeechCallback cb, void* ctx, std::uint64_t* id) -> BFA_SpeechResult {
        callback = cb; context = ctx; *id = 1; return BFA_SpeechOK;
    };
    api.Prepare = [](const BFA_SpeechLine* line, std::uint64_t* handle) -> BFA_SpeechResult {
        prepared = *line; *handle = 42; return BFA_SpeechOK;
    };
    api.Start = [](std::uint64_t, double offset) -> BFA_SpeechResult {
        position = offset; return BFA_SpeechOK; // audio starts, face still fallback
    };
    api.Pause = [](std::uint64_t) -> BFA_SpeechResult { return BFA_SpeechOK; };
    api.Resume = [](std::uint64_t, double offset) -> BFA_SpeechResult {
        position = offset; return BFA_SpeechOK;
    };
    api.Seek = api.Resume;
    api.Stop = [](std::uint64_t handle, std::uint32_t) -> BFA_SpeechResult {
        Check(handle == 42); ++stops; return BFA_SpeechOK;
    };
    api.EndUtterance = [](std::uint32_t actor, std::uint64_t reply, std::uint32_t) -> BFA_SpeechResult {
        Check(actor == prepared.speaker_form_id && reply == prepared.utterance_id);
        ++endings; return BFA_SpeechOK;
    };
    api.SetMood = [](std::uint32_t, const BFA_SpeechEmotion* value) -> BFA_SpeechResult {
        ++moods; mood = *value; return BFA_SpeechOK;
    };
    return api;
}
}

int main()
{
    using namespace FacialAnimationBridge;
    auto api = MakeApi();
    Check(IsCompatible(&api));
    auto shortApi = api; shortApi.size = offsetof(BFA_SpeechInterface, Owner);
    Check(!IsCompatible(&shortApi));
    const char wav[] = "RIFF copied fixture";
    std::shared_ptr<FaceToken> queued;
    {
        Line line(&api, 0x123);
        queued = line.GetToken();
        Check(line.Prepare(wav, sizeof(wav), "hello", "DialogueHappy") == BFA_SpeechOK);
        Check(prepared.audio == wav && prepared.audio_bytes == sizeof(wav));
        Check(moods == 1 && mood.channels[6] == 0.9f);
        Check(line.Start(0.02) == BFA_SpeechOK && position == 0.02);
        Check(queued->CanWrite()); // unlike old ABI, Start does not claim face
        Event(BFA_SpeechStarted);
        Check(!queued->CanWrite()); // synchronous callback gates queued tasks
        owner = BFA_SpeechOwnerProvider;
        line.Pause(); line.Resume(0.5); line.Seek(0.7);
        Check(position == 0.7);
        Event(BFA_SpeechEnded);
        Check(!queued->CanWrite()); // another segment/owner still holds the actor
        owner = BFA_SpeechOwnerEngine;
        Check(queued->CanWrite());
        Event(BFA_SpeechDeclined);
        Check(queued->CanWrite());
        owner = BFA_SpeechOwnerSuppressed;
        Check(!queued->CanWrite());
        line.Stop(BFA_SpeechCompleted);
        Check(stops == 1 && endings == 1 && !queued->CanWrite());
        Event(BFA_SpeechStarted); // stale callback must not revive an old token
        Check(!queued->CanWrite());
    }
    Check(stops == 1 && endings == 1);
    owner = BFA_SpeechOwnerEngine;
    {
        Line absent(nullptr, 0x123);
        Check(absent.Prepare(wav, sizeof(wav), "", "") == BFA_SpeechUnavailable);
        Check(absent.GetToken()->CanWrite());
        Check(absent.Start(0) == BFA_SpeechInvalidHandle);
    }
    {
        Line cancelled(&api, 0x123);
        cancelled.Prepare(wav, sizeof(wav), "", "");
        Check(moods == 1); // omitted mood is preserved
    }
    Check(stops == 2 && endings == 2);
    std::cout << "Streaming bridge tests passed: deferred ownership, callbacks, clock, cleanup and fallback\n";
}
