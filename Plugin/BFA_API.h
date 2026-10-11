// Better Facial Animations provider API. Freely redistributable, plain C.
// Unreleased: changed in place, no compatibility shim or version bump.
// Rebuild every caller against this header. Discover at/after SKSE PostLoad.
//
// Threads: every function may be called from any thread, including from
// inside an event callback. Callbacks run on BFA's event thread without BFA
// locks held: copy the event and return. Unregister does not wait for an
// in-flight callback; keep its context alive. Every listener receives every
// provider's events: act only on handles you prepared.
//
// Lifecycle: NewUtteranceId names a reply. Prepare copies the WAV bytes and queues engine work on the SKSE
// task interface. Start/Pause/Seek report the audio clock; they do not
// mean BFA is animating. kStarted means animation has begun: keep fallback
// lipsync until then, and stop fallback writes until a terminal event
// (kEnded/kFailed/kDeclined/kOwnerChanged/kExpired). Terminal events retire
// the handle. No status polling or explicit release.
//
// Limits: open handles expire after 120 seconds without a playback call,
// a paused one included. Utterance holds expire after 120 seconds without
// activity; call EndUtterance. At most 256 open handles and 256 open
// utterances; audio is at most BFA_MAX_AUDIO_BYTES per line.
#ifndef BFA_API_H
#define BFA_API_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define BFA_API_VERSION 1u
#define BFA_PLUGIN_NAME "BetterFacialAnimations"
#define BFA_MAX_AUDIO_BYTES (32u * 1024u * 1024u)


enum BFA_MessageType {
  BFA_kRequestInterface = 0x42464101,
  BFA_kInterface = 0x42464102,
};

// A single fixed-width result. Compare named values, not truthiness (OK == 0).
typedef uint32_t BFA_Result;
enum BFA_ResultCode {
  BFA_kResultOK = 0,
  BFA_kResultPending = 1,  // accepted; generation or face cleanup pending
  BFA_kResultNoChange = 2,  // already in the requested state
  BFA_kResultInvalidArgument = 3,
  BFA_kResultInvalidHandle = 4,  // unknown or finished handle
  BFA_kResultInvalidState = 5,  // operation not legal in the current state
  BFA_kResultNotFound = 6,  // unknown listener or reply
  BFA_kResultUnavailable = 7,  // plugin hooks not ready
  BFA_kResultDisabled = 8,  // master switch off
  BFA_kResultPlayerDisabled = 9,
  BFA_kResultRaceDisabled = 10,
  BFA_kResultInScene = 11,  // a scene owns the face
  BFA_kResultNoFaceGenHead = 12,  // missing actor or FaceGen head
  BFA_kResultHeadMapFailed = 13,  // required morphs unavailable
  BFA_kResultModelNotLoaded = 14,  // network not loaded yet
  BFA_kResultNoDevice = 15,  // no inference device available
  BFA_kResultUnsupportedRuntime = 16,  // actor lookup unsupported (VR)
  BFA_kResultAudioFailed = 17,  // decode failed or audio silent
  BFA_kResultNotInDialogue = 18,  // excluded by Potato mode
  BFA_kResultInternalError = 19,
  BFA_kResultInferenceFailed = 20,  // animation generation failed
  BFA_kResultCancelled = 21,
  BFA_kResultTakenByOtherLine = 22,
  BFA_kResultExpired = 23,
  BFA_kResultCapacity = 24,
};

enum BFA_StopReason {
  BFA_kCompleted = 0,
  BFA_kSkipped = 1,
  BFA_kSuperseded = 2,
  BFA_kAborted = 3,
};

enum BFA_EmotionChannel {
  BFA_kAmazement, BFA_kAnger, BFA_kCheekiness, BFA_kDisgust, BFA_kFear,
  BFA_kGrief, BFA_kJoy, BFA_kOutOfBreath, BFA_kPain, BFA_kSadness,
  BFA_kNumEmotionChannels
};

// The emotion a line is generated with: each channel finite, 0..2.
typedef struct BFA_Emotion {
  uint32_t version, size;
  float channels[BFA_kNumEmotionChannels];
} BFA_Emotion;

typedef struct BFA_Line {
  uint32_t version, size;
  // From NewUtteranceId, shared by the reply's segments; one speaker.
  uint64_t utterance_id;
  uint32_t segment;
  uint32_t speaker_form_id;  // loaded actor, including player 0x14
  const void* audio;  // RIFF WAVE bytes, copied before returning
  uint32_t audio_bytes;  // 12..BFA_MAX_AUDIO_BYTES
  const BFA_Emotion* emotion;  // copied before returning; null = neutral
} BFA_Line;

// reason carries: kStarted none (0); kEnded a BFA_StopReason; kFailed,
// kDeclined, kOwnerChanged and kExpired a BFA_ResultCode.
enum BFA_EventType {
  BFA_kStarted = 0,
  BFA_kEnded = 1,
  BFA_kFailed = 2,
  BFA_kDeclined = 3,
  BFA_kOwnerChanged = 4,
  BFA_kExpired = 5,
};

typedef struct BFA_Event {
  uint32_t version, size;
  uint32_t event;  // BFA_EventType
  uint32_t reason;  // see BFA_EventType
  uint64_t handle, utterance_id;
  uint32_t segment, speaker_form_id;
} BFA_Event;

typedef void (*BFA_EventCallback)(const BFA_Event* event, void* context);

typedef struct BFA_Interface {
  uint32_t version, size;
  // A reply id no other caller can get. Costs nothing until Prepare uses it.
  uint64_t (*NewUtteranceId)(void);
  // 0 on rejected intake; result_out required. Pending means queued, not
  // animated. Later admission/inference failures arrive as terminal events.
  // An utterance_id not from NewUtteranceId is NotFound.
  uint64_t (*Prepare)(const BFA_Line* line, BFA_Result* result_out);
  // Audio playback began at offset_seconds. A second Start moves the clock
  // like Seek and returns OK.
  BFA_Result (*Start)(uint64_t handle, double offset_seconds);
  BFA_Result (*Pause)(uint64_t handle);  // the clock stops where it is
  // The clock runs from offset_seconds (finite, >= 0): also the unpause.
  BFA_Result (*Seek)(uint64_t handle, double offset_seconds);
  BFA_Result (*Stop)(uint64_t handle, uint32_t reason);  // valid before Start
  // Closes the reply: stops its segments, frees their queued/cached audio and
  // animations, cancels running inference and releases the expression hold.
  BFA_Result (*EndUtterance)(uint64_t utterance_id, uint32_t reason);
  uint64_t (*RegisterListener)(BFA_EventCallback callback, void* context,
                               BFA_Result* result_out);
  BFA_Result (*UnregisterListener)(uint64_t listener_id);
} BFA_Interface;

typedef const BFA_Interface* (*BFA_QueryInterfaceFn)(uint32_t wanted_version);

#ifdef __cplusplus
}
#endif
#endif
