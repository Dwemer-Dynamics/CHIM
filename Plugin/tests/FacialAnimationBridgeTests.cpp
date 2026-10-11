#include "FacialAnimationBridge.h"
#include <cstdlib>
#include <iostream>

namespace {
#define Check(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (0)
std::uint32_t owner = BFA_kOwnerEngine;
int stops = 0, releases = 0, moods = 0, starts = 0, seeks = 0;
BFA_Line prepared{};
BFA_Emotion mood{};
BFA_LineStatus status{};
bool reject = false, immediateFailure = false, asynchronousFailure = false;
BFA_Result R(std::uint32_t value = BFA_ResultOK) { return value; }
BFA_Interface MakeApi()
{
    BFA_Interface api{};
    api.version = BFA_API_VERSION;
    api.size = sizeof(api);
    api.Owner = [](std::uint32_t) { return owner; };
    api.Prepare = [](const BFA_Line* line, std::uint64_t* handle) -> BFA_Result {
        *handle = 0;
        if (reject) return BFA_ResultDisabled;
        prepared = *line;
        *handle = 42;
        status = {};
        status.version = BFA_API_VERSION; status.size = sizeof(status);
        status.handle = 42; status.actor_form_id = line->speaker_form_id;
        status.generation = BFA_GenerationQueued;
        status.playback = BFA_PlaybackPrepared;
        status.result = R(BFA_ResultPending);
        if (immediateFailure) {
            status.generation = BFA_GenerationFailed;
            status.playback = BFA_PlaybackFailed;
            status.result = BFA_ResultAudioFailed;
        }
        return status.result;
    };
    api.Start = [](std::uint64_t h, double offset) -> BFA_Result {
        Check(h == 42); ++starts;
        status.position_seconds = offset; status.playback = BFA_PlaybackPlaying;
        owner = BFA_kOwnerProvider; status.face_owned = 1;
        return R(BFA_ResultPending);
    };
    api.Pause = [](std::uint64_t) -> BFA_Result {
        status.playback = BFA_PlaybackPaused; return R();
    };
    api.Resume = [](std::uint64_t, double offset) -> BFA_Result {
        status.playback = BFA_PlaybackPlaying; status.position_seconds = offset; return R();
    };
    api.Seek = [](std::uint64_t, double offset) -> BFA_Result {
        ++seeks; status.position_seconds = offset;
        if (asynchronousFailure) {
            status.generation = BFA_GenerationFailed; status.playback = BFA_PlaybackFailed;
            status.result = BFA_ResultInferenceFailed;
            status.face_owned = 0; owner = BFA_kOwnerEngine;
        }
        return R(); // caller must also inspect asynchronous status
    };
    api.GetStatus = [](std::uint64_t h, BFA_LineStatus* out) -> BFA_Result {
        Check(h == 42 && out->version == BFA_API_VERSION && out->size == sizeof(*out));
        *out = status; return R();
    };
    api.Stop = [](std::uint64_t h, std::uint32_t reason) -> BFA_Result {
        Check(h == 42); ++stops;
        if (status.playback >= BFA_PlaybackEnded) return R(BFA_ResultAlreadyStopped);
        status.playback = BFA_PlaybackEnded; status.stop_reason = reason;
        status.face_owned = 0; owner = BFA_kOwnerEngine; status.result = R();
        return R();
    };
    api.ReleaseHandle = [](std::uint64_t h) -> BFA_Result { Check(h == 42); ++releases; return R(); };
    api.SetMood = [](std::uint32_t, const BFA_Emotion* value, float fade) -> BFA_Result {
        Check(fade == 0); ++moods; mood = *value; return R();
    };
    return api;
}
}
int main()
{
    using namespace FacialAnimationBridge;
    auto api = MakeApi();
    const char wav[] = "test audio";
    {
        Line line(&api, 0x123);
        Check(line.Prepare(wav, sizeof(wav), "hello", "DialogueHappy") == BFA_ResultPending);
        Check(prepared.flags == BFA_kLastSegment && prepared.audio.bytes == sizeof(wav));
        Check(prepared.audio.format == BFA_kContainer && mood.skyrim_type == 5);
        Check(mood.skyrim_value == 90 && line.HasStatus());
        auto queued = line.GetToken();
        Check(queued->CanWrite());
        Check(line.Start(.25) == BFA_ResultPending && !queued->CanWrite());
        Check(line.Pause() == BFA_ResultOK);
        Check(line.Snapshot().playback == BFA_PlaybackPaused);
        Check(line.Resume(.5) == BFA_ResultOK);
        Check(line.Seek(.75) == BFA_ResultOK);
        Check(line.Snapshot().position_seconds == .75);
        status.generation = BFA_GenerationReady; status.audio_seconds = 1;
        Check(line.Seek(.8) == BFA_ResultOK);
        Check(line.Snapshot().generation == BFA_GenerationReady);
        Check(line.Stop(BFA_kCompleted) == BFA_ResultOK);
        Check(line.Snapshot().playback == BFA_PlaybackEnded && !queued->CanWrite());
        Check(stops == 1 && releases == 1);
    }
    Check(stops == 1); // RAII cleanup does not double-stop
    {
        Line absent(nullptr, 0x123);
        Check(absent.Prepare(wav, sizeof(wav), "", "") == BFA_ResultUnavailable);
        Check(absent.Start(0) == BFA_ResultUnavailable);
        Check(absent.GetToken()->CanWrite());
    }
    auto old = api; old.size = 16;
    Check(!IsCompatible(&old));
    old = api; old.version = 0;
    Check(!IsCompatible(&old));
    reject = true;
    {
        Line denied(&api, 0x123);
        Check(denied.Prepare(wav, sizeof(wav), "", "") == BFA_ResultDisabled);
        Check(denied.Start(0) == BFA_ResultDisabled);
        Check(denied.GetToken()->CanWrite());
    }
    Check(stops == 1 && moods == 1);
    reject = false; immediateFailure = true;
    {
        Line failed(&api, 0x123);
        Check(failed.Prepare(wav, sizeof(wav), "", "") == BFA_ResultAudioFailed);
        Check(failed.Snapshot().result == BFA_ResultAudioFailed);
        const auto before = starts;
        Check(failed.Start(0) == BFA_ResultAudioFailed && starts == before);
        Check(failed.GetToken()->CanWrite());
    }
    Check(stops == 2 && releases == 2);
    immediateFailure = false;
    {
        Line late(&api, 0x123);
        Check(late.Prepare(wav, sizeof(wav), "", "MoodNeutral") == BFA_ResultPending);
        Check(mood.gain == 0);
        late.Start(0);
        asynchronousFailure = true;
        Check(late.Seek(.1) == BFA_ResultInferenceFailed);
        Check(late.LastResult() == BFA_ResultInferenceFailed && late.GetToken()->CanWrite());
        const auto before = seeks;
        Check(late.Seek(.2) == BFA_ResultInferenceFailed && seeks == before);
        owner = BFA_kOwnerSuppressed;
        Check(!late.GetToken()->CanWrite());
        owner = BFA_kOwnerEngine;
    }
    Check(stops == 3 && releases == 3);
    std::cout << "Provider result/status bridge passed: pending, terminal failure, clock, fallback and cleanup\n";
}
