#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <unordered_map>

#include "FacialAnimationBridge.h"

namespace {
#define Check(x)                                   \
    do {                                           \
        if (!(x)) {                                \
            std::cerr << __LINE__ << ": " #x "\n"; \
            std::exit(1);                          \
        }                                          \
    } while (0)
    BFA_EventCallback callback = nullptr;
    BFA_Line prepared{};
    BFA_Emotion mood{};
    uint64_t handles = 41;
    int starts = 0, clocks = 0, stops = 0, ends = 0;
    bool reject = false, failInline = false;
    std::unordered_map<uint64_t, BFA_Line> submitted;
    void Event(uint32_t event, uint32_t reason, uint64_t handle) {
        const auto& line = submitted.at(handle);
        BFA_Event ev{BFA_API_VERSION, sizeof(BFA_Event), event,        reason,
                     handle,          line.utterance_id, line.segment, line.speaker_form_id};
        callback(&ev, nullptr);
    }
    BFA_Interface MakeApi() {
        BFA_Interface api{};
        api.version = BFA_API_VERSION;
        api.size = sizeof api;
        api.RegisterListener = [](BFA_EventCallback cb, void*, BFA_Result* result) -> uint64_t {
            callback = cb;
            *result = BFA_kResultOK;
            return 1;
        };
        api.UnregisterListener = [](uint64_t) -> BFA_Result { return BFA_kResultOK; };
        api.NewUtteranceId = []() -> uint64_t {
            static uint64_t next = 0;
            return ++next;
        };
        api.Prepare = [](const BFA_Line* line, BFA_Result* result) -> uint64_t {
            Check(line->utterance_id);
            prepared = *line;
            mood = line->emotion ? *line->emotion : BFA_Emotion{};
            prepared.emotion = line->emotion ? &mood : nullptr;
            *result = reject ? BFA_kResultInvalidArgument : BFA_kResultPending;
            if (reject) return 0;
            submitted[++handles] = *line;
            if (failInline) Event(BFA_kDeclined, BFA_kResultInScene, handles);
            return handles;
        };
        api.Start = [](uint64_t h, double) -> BFA_Result {
            Check(h == handles);
            ++starts;
            return BFA_kResultPending;
        };
        api.Pause = [](uint64_t) -> BFA_Result {
            ++clocks;
            return BFA_kResultOK;
        };
        api.Seek = [](uint64_t, double) -> BFA_Result {
            ++clocks;
            return BFA_kResultOK;
        };
        api.Stop = [](uint64_t, uint32_t) -> BFA_Result {
            ++stops;
            return BFA_kResultOK;
        };
        api.EndUtterance = [](uint64_t id, uint32_t) -> BFA_Result {
            Check(id);
            ++ends;
            return BFA_kResultPending;
        };
        return api;
    }
}
int main() {
    using namespace FacialAnimationBridge;
    auto api = MakeApi();
    const char wav[] = "RIFF0000WAVE";
    // A reply: its sentences share one utterance, and the face stays BFA's between them.
    std::shared_ptr<FaceToken> reply;
    {
        Line first(&api, 0x14);
        reply = first.GetToken();
        Check(first.Prepare(wav, sizeof wav, "hello", "DialogueHappy") == BFA_kResultPending && prepared.segment == 0);
        Check(prepared.emotion && mood.channels[BFA_kJoy] == 1 && !reply->CanWrite());
        Check(first.Start(0) == BFA_kResultPending && !reply->CanWrite());  // pending also blocks writes
        Event(BFA_kStarted, 0, 42);
        Check(!reply->CanWrite() && !CanWriteActor(0x14));
        Check(first.Pause() == BFA_kResultOK && first.Seek(.2) == BFA_kResultOK);
        Check(first.Stop(BFA_kCompleted) == BFA_kResultOK && stops == 1 && ends == 0 &&
              !CanWriteActor(0x14));                            // the reply goes on
        Check(first.Seek(.3) == BFA_kResultOK && clocks == 2);  // stopped: no more clock calls
    }
    Check(stops == 1 && !CanWriteActor(0x14));
    const auto utterance = prepared.utterance_id;
    {
        Line second(&api, 0x14);
        Check(second.GetToken() == reply);
        Check(second.Prepare(wav, sizeof wav, "again", "") == BFA_kResultPending &&
              prepared.utterance_id == utterance && prepared.segment == 1);
        Check(second.Start(0) == BFA_kResultPending);
        Event(BFA_kStarted, 0, 43);
        Check(!CanWriteActor(0x14));
        Event(BFA_kFailed, BFA_kResultInferenceFailed, 43);
        Check(!CanWriteActor(0x14) && second.LastResult() == BFA_kResultInferenceFailed);
        Event(BFA_kEnded, BFA_kCompleted, 42);  // older sentence must also hand back
        Check(CanWriteActor(0x14));
        int before = starts;
        Check(second.Start(0) == BFA_kResultInferenceFailed && starts == before);
        Event(BFA_kStarted, 0, 43);
        Check(CanWriteActor(0x14));  // late callback ignored
        Line third(&api, 0x14);
        Check(third.GetToken() == reply);
        Check(third.Prepare(wav, sizeof wav, "", "MoodNeutral") == BFA_kResultPending && prepared.segment == 2 &&
              mood.channels[BFA_kJoy] == 0);
        Check(third.Start(0) == BFA_kResultPending);
        Event(BFA_kStarted, 0, 44);
        Check(!CanWriteActor(0x14));
    }  // the sentences are stopped, the reply still open
    Check(stops == 2 && ends == 0 && !CanWriteActor(0x14));
    EndReply(0x14, BFA_kCompleted);
    Check(ends == 1 && !CanWriteActor(0x14));  // BFA fades the face back first
    Event(BFA_kEnded, BFA_kCompleted, 42);
    Check(!CanWriteActor(0x14));  // duplicate old event
    Event(BFA_kEnded, BFA_kCompleted, 44);
    Check(CanWriteActor(0x14));
    EndReply(0x14, BFA_kCompleted);
    Check(ends == 1);  // nothing open
    // The next sentence opens a new reply.
    {
        Line line(&api, 0x14);
        Check(line.GetToken() != reply);
        Check(line.Prepare(wav, sizeof wav, "", "") == BFA_kResultPending && prepared.utterance_id != utterance &&
              prepared.segment == 0);
    }
    EndReply(0x14, BFA_kAborted);
    Check(ends == 2);
    Event(BFA_kEnded, BFA_kAborted, 45);
    // A reply with no new sentence for StaleSeconds is over when the next one starts.
    {
        Line a(&api, 0x14);
        Check(a.Prepare(wav, sizeof wav, "", "") == BFA_kResultPending);
    }
    StaleSeconds() = 0;
    {
        Line b(&api, 0x14);
        Check(ends == 3);
        Check(b.Prepare(wav, sizeof wav, "", "") == BFA_kResultPending && prepared.segment == 0);
    }
    StaleSeconds() = 10;
    EndReply(0x14, BFA_kCompleted);
    Check(ends == 4);
    Check(!CanWriteActor(0x14));
    Event(BFA_kEnded, BFA_kCompleted, 46);
    Check(!CanWriteActor(0x14));
    Event(BFA_kEnded, BFA_kCompleted, 47);
    Check(CanWriteActor(0x14));
    {
        Line line(nullptr, 0x14);
        Check(line.Prepare(wav, sizeof wav, "", "") == BFA_kResultUnavailable);
        Check(line.Start(0) == BFA_kResultUnavailable && line.GetToken()->CanWrite());
    }
    reject = true;
    {
        Line line(&api, 0x14);
        Check(line.Prepare(wav, sizeof wav, "", "") == BFA_kResultInvalidArgument);
        Check(line.Start(0) == BFA_kResultInvalidArgument && line.GetToken()->CanWrite());
    }
    reject = false;
    EndReply(0x14, BFA_kAborted);  // rejected intake leaves no callback to wait for
    Check(State().tokens.empty());
    // A later segment's pending/started state survives terminal callbacks for
    // older segments. Each Line keeps its own result even after another ends.
    {
        Line first(&api, 0x14), second(&api, 0x14);
        first.Prepare(wav, sizeof wav, "", "");
        const auto a = first.Handle();
        second.Prepare(wav, sizeof wav, "", "");
        const auto b = second.Handle();
        Check(!CanWriteActor(0x14) && CanWriteActor(0x15));
        Event(BFA_kStarted, 0, b);
        Event(BFA_kDeclined, BFA_kResultHeadMapFailed, a);
        Check(!CanWriteActor(0x14) && first.LastResult() == BFA_kResultHeadMapFailed);
        Event(BFA_kStarted, 0, a);  // late start cannot resurrect a retired segment
        Check(!CanWriteActor(0x14));
        EndReply(0x14, BFA_kCompleted);
        Check(!CanWriteActor(0x14));
        Event(BFA_kEnded, BFA_kCompleted, b);
        Check(CanWriteActor(0x14) && first.LastResult() == BFA_kResultHeadMapFailed);
    }
    Check(State().tokens.empty());
    // A rejected intake may be corrected and retried on the same Line.
    {
        Line line(&api, 0x14);
        reject = true;
        Check(line.Prepare(wav, sizeof wav, "", "") == BFA_kResultInvalidArgument);
        reject = false;
        Check(line.Prepare(wav, sizeof wav, "", "") == BFA_kResultPending);
        Check(!CanWriteActor(0x14));
        EndReply(0x14, BFA_kCompleted);
        Event(BFA_kEnded, BFA_kCompleted, line.Handle());
        Check(CanWriteActor(0x14));
    }
    // Audio can fail to start after intake. Stop before Start must wait for
    // the terminal callback, then release fallback even though Ended means OK.
    {
        Line line(&api, 0x14);
        const int startsBefore = starts, stopsBefore = stops;
        Check(line.Prepare(wav, sizeof wav, "", "DialogueHappy") == BFA_kResultPending);
        Check(line.Stop(BFA_kAborted) == BFA_kResultOK);
        Check(starts == startsBefore && stops == stopsBefore + 1 && !CanWriteActor(0x14));
        Event(BFA_kEnded, BFA_kAborted, line.Handle());
        Check(line.LastResult() == BFA_kResultOK && CanWriteActor(0x14));
        line.Seek(.5);
        line.Stop(BFA_kAborted);
        Check(stops == stopsBefore + 1);
        EndReply(0x14, BFA_kAborted);
    }
    Check(State().tokens.empty());
    // Failure can arrive during Prepare itself; returning Pending must not
    // close the gate again or lose the terminal result.
    failInline = true;
    {
        Line line(&api, 0x14);
        Check(line.Prepare(wav, sizeof wav, "", "") == BFA_kResultInScene);
        Check(CanWriteActor(0x14));
        EndReply(0x14, BFA_kAborted);
    }
    Check(State().tokens.empty());
    std::cout << "Event bridge passed: one reply per response, pending suppression, per-segment ownership, terminal "
                 "errors, reply cleanup\n";
}
