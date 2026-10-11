// Better Facial Animations provider API. Freely redistributable, plain C.
// Unreleased: changed in place, no compatibility shim or version bump.
// Rebuild every caller against this header. Discover at/after SKSE PostLoad.
// Prepare copies WAV bytes and queues engine work on the SKSE task interface.
// Start/Pause/Resume/Seek track the audio clock; they do not mean BFA is animating.
// kStarted means animation has begun. Keep fallback lipsync until that event;
// stop fallback writes until kEnded/kFailed/kDeclined/kOwnerChanged/kExpired.
// Terminal events retire the handle. No status polling or explicit release.
// Callbacks run on BFA's event thread without BFA locks held: copy and return.
// Unregister does not wait for an in-flight callback; keep its context alive.
// Open handles expire after 120 seconds without a playback call. Maximum 256.
// Utterance holds expire after 120 seconds without activity; call EndUtterance.
#ifndef BFA_API_H
#define BFA_API_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define BFA_API_VERSION 1u
#define BFA_PLUGIN_NAME "BetterFacialAnimations"
enum BFA_MessageType {
  BFA_kRequestInterface = 0x42464101,
  BFA_kInterface = 0x42464102,
};

// A single fixed-width result. Compare named values, not truthiness (OK == 0).
typedef uint32_t BFA_Result;
enum BFA_ResultCode {
  BFA_ResultOK = 0,
  BFA_ResultPending = 1, // accepted; generation or face cleanup pending
  BFA_ResultNoChange = 2, // already in the requested state
  BFA_ResultInvalidArgument = 4,
  BFA_ResultInvalidHandle = 5, // unknown or finished handle
  BFA_ResultInvalidState = 6, // operation not legal in the current state
  BFA_ResultNotFound = 7, // unknown listener or reply
  BFA_ResultUnavailable = 8, // plugin hooks not ready
  BFA_ResultDisabled = 9, // master switch off
  BFA_ResultPlayerDisabled = 10,
  BFA_ResultRaceDisabled = 11,
  BFA_ResultInScene = 12, // a scene owns the face
  BFA_ResultNoFaceGenHead = 13, // missing actor or FaceGen head
  BFA_ResultHeadMapFailed = 14, // required morphs unavailable
  BFA_ResultModelNotLoaded = 15, // network not loaded yet
  BFA_ResultNoDevice = 16, // no inference device available
  BFA_ResultUnsupportedRuntime = 18, // actor lookup unsupported on this runtime
  BFA_ResultAudioFailed = 19, // decode failed or audio silent
  BFA_ResultNotInDialogue = 20, // excluded by Potato mode
  BFA_ResultInternalError = 21,
  BFA_ResultInferenceFailed = 22, // animation generation failed
  BFA_ResultCancelled = 23,
  BFA_ResultTakenByOtherLine = 24,
  BFA_ResultExpired = 25,
  BFA_ResultCapacity = 26,
};


enum BFA_StopReason {
  BFA_kCompleted = 0, BFA_kSkipped = 1,
  BFA_kSuperseded = 2, BFA_kAborted = 3,
};
enum BFA_EmotionChannel {
  BFA_kAmazement, BFA_kAnger, BFA_kCheekiness, BFA_kDisgust, BFA_kFear,
  BFA_kGrief, BFA_kJoy, BFA_kOutOfBreath, BFA_kPain, BFA_kSadness,
  BFA_kNumEmotionChannels
};
typedef struct BFA_Emotion {
  uint32_t version, size;
  float channels[BFA_kNumEmotionChannels]; // each finite, 0..2; zero = neutral
} BFA_Emotion;
typedef struct BFA_Line {
  uint32_t version, size;
  uint64_t utterance_id; // nonzero, unique to this reply; shared across segments
  uint32_t segment;
  uint32_t speaker_form_id; // loaded actor, including player 0x14
  const void* audio; // RIFF WAVE bytes, copied before returning
  uint32_t audio_bytes;
} BFA_Line;
enum BFA_EventType {
  BFA_kStarted = 0, BFA_kEnded, BFA_kFailed, BFA_kDeclined,
  BFA_kOwnerChanged, BFA_kExpired
};
typedef struct BFA_Event {
  uint32_t version, size;
  uint32_t event; // BFA_EventType
  uint32_t reason; // BFA_StopReason for Ended; BFA_ResultCode otherwise
  uint64_t handle, utterance_id;
  uint32_t segment, speaker_form_id;
} BFA_Event;
typedef void (*BFA_EventCallback)(const BFA_Event* event, void* context);
typedef struct BFA_Interface {
  uint32_t version, size;
  // 0 on rejected intake; result_out required. Pending means queued, not animated.
  // Later admission/inference failures arrive as terminal events for this handle.
  uint64_t (*Prepare)(const BFA_Line* line, BFA_Result* result_out);
  BFA_Result (*Start)(uint64_t handle, double offset_seconds);
  BFA_Result (*Pause)(uint64_t handle);
  // -1 resumes at current position; all other offsets must be finite and >= 0.
  BFA_Result (*Resume)(uint64_t handle, double offset_seconds);
  BFA_Result (*Seek)(uint64_t handle, double offset_seconds);
  BFA_Result (*Stop)(uint64_t handle, uint32_t reason); // valid before Start
  // Closes the reply: stops segments, frees queued/cached audio and animations,
  // cancels running inference and releases the expression hold. Running work
  // drops its temporary buffers when it observes cancellation.
  BFA_Result (*EndUtterance)(uint64_t utterance_id, uint32_t reason);
  uint64_t (*RegisterListener)(BFA_EventCallback callback, void* context,
                               BFA_Result* result_out);
  BFA_Result (*UnregisterListener)(uint64_t listener_id);
  // Per-actor mood for subsequent Prepare calls; null clears it. fade must be 0.
  BFA_Result (*SetMood)(uint32_t actor_form_id, const BFA_Emotion* mood,
                        float fade_seconds);
} BFA_Interface;
typedef const BFA_Interface* (*BFA_QueryInterfaceFn)(uint32_t wanted_version);
#ifdef __cplusplus
}
#endif
#endif
