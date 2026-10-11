/* BFA_SpeechAPI.h -- cooperative streaming speech interface.
 * Freely usable and redistributable, including in closed-source projects.
 *
 * Prepare copies a complete PCM WAV clip; its handle identifies pending work.
 * Start reports that YOUR audio started; keep your own lipsync until Started.
 * Started is emitted only after generation AND the actor's head are ready.
 * BFA waits for Started callbacks to return before its first facial write.
 * Set the actor's write gate in the callback; queued writes must check that
 * gate at execution time. Do not defer the gate change to another queue.
 * Callbacks may arrive before Prepare returns: correlate utterance + segment.
 * Never hold your own locks while calling BFA. Callbacks run without BFA locks.
 *
 * Stop finishes one segment. EndUtterance ends the reply and cancels remaining
 * prepared segments. Normal sentence gaps retain ownership for up to 10s;
 * EndUtterance releases that hold. Pending never-started handles expire at 30s.
 * Started lines expire after 600s without a control call. World reset cancels
 * all handles and moods. The cache is internal; handles are never reused.
 * Admission limits: 32 MiB per clip, 128 MiB retained audio, 256 open handles.
 * Capacity rejects new work; it does not evict another caller's handle.
 *
 * Ended/Failed/Declined/Expired terminate a handle. OwnerChanged is an actor
 * ownership notification when handle=0 (reason is BFA_SpeechOwner). With a
 * handle it terminates that line; reason is TakenByOtherLine.
 * Use Owner immediately before a queued facial write, in addition to your
 * local gate. Other BFA providers or suppression forbid fallback too.
 * No guarantee is made against uncooperative third-party writers.
 *
 * OK means the command was accepted. Admission can fail later on the game
 * thread; that failure is reported by an event. No polling Start for readiness.
 * PCM WAV 8/16-bit mono/stereo is decoded/resampled internally. Mood is captured
 * when preparation is admitted, not recomputed when Start is called.
 * No fade_seconds argument: mood transitions are not implemented.
 *
 * Discovery: GetProcAddress("BFA_QuerySpeechInterface"), at/after kPostLoad.
 * This is a separate ABI; legacy BFA_QueryInterface remains unchanged.
 * All structs are append-only, default Windows x64 alignment, no packing.
 */
#ifndef BFA_SPEECH_API_H
#define BFA_SPEECH_API_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define BFA_SPEECH_API_VERSION 1u

typedef uint32_t BFA_SpeechResult;
enum {
  BFA_SpeechOK = 0, BFA_SpeechInvalidHandle, BFA_SpeechInvalidArgument,
  BFA_SpeechInvalidState, BFA_SpeechDisabled, BFA_SpeechPlayerDisabled,
  BFA_SpeechRaceDisabled, BFA_SpeechInScene, BFA_SpeechPotatoMode,
  BFA_SpeechNoHead, BFA_SpeechModelUnavailable, BFA_SpeechNoDevice,
  BFA_SpeechAudioFailed, BFA_SpeechSuppressed, BFA_SpeechTakenByOtherLine,
  BFA_SpeechExpired, BFA_SpeechWorldChanged, BFA_SpeechBusy,
  BFA_SpeechUnavailable, BFA_SpeechCapacity
};
enum BFA_SpeechStopReason {
  BFA_SpeechCompleted = 0, BFA_SpeechSkipped = 1, BFA_SpeechInterrupted = 2
};
enum BFA_SpeechEventType {
  BFA_SpeechStarted = 0, BFA_SpeechEnded, BFA_SpeechFailed,
  BFA_SpeechDeclined, BFA_SpeechOwnerChanged, BFA_SpeechHandleExpired
};
enum BFA_SpeechOwner {
  BFA_SpeechOwnerEngine = 0, BFA_SpeechOwnerProvider = 1,
  BFA_SpeechOwnerSuppressed = 2
};
typedef struct BFA_SpeechLine {
  uint32_t version, size;
  uint64_t utterance_id; /* choose process-unique reply IDs, shared by segments */
  uint32_t segment, speaker_form_id;
  const void* audio;    /* copied complete WAV, not a borrowed playback buffer */
  uint32_t audio_bytes;
} BFA_SpeechLine;
typedef struct BFA_SpeechEmotion {
  uint32_t version, size;
  float channels[10]; /* amazement anger cheekiness disgust fear grief joy
                         outofbreath pain sadness; each 0..2 */
} BFA_SpeechEmotion;
typedef struct BFA_SpeechEvent {
  uint32_t version, size;
  uint32_t event, reason; /* result for failures; stop reason for Ended */
  uint64_t handle, utterance_id;
  uint32_t segment, speaker_form_id;
} BFA_SpeechEvent;
typedef void (*BFA_SpeechCallback)(const BFA_SpeechEvent*, void* ctx);
typedef struct BFA_SpeechInterface {
  uint32_t version, size;
  BFA_SpeechResult (*Prepare)(const BFA_SpeechLine*, uint64_t* handle);
  BFA_SpeechResult (*Start)(uint64_t handle, double audio_position);
  BFA_SpeechResult (*Pause)(uint64_t handle);
  BFA_SpeechResult (*Resume)(uint64_t handle, double audio_position);
  BFA_SpeechResult (*Seek)(uint64_t handle, double audio_position);
  BFA_SpeechResult (*Stop)(uint64_t handle, uint32_t reason);
  BFA_SpeechResult (*EndUtterance)(uint32_t actor, uint64_t utterance,
                                  uint32_t reason);
  BFA_SpeechResult (*RegisterListener)(BFA_SpeechCallback, void* ctx,
                                      uint64_t* listener);
  /* Busy means a callback is in flight. New dispatch is disabled immediately.
   * Retry later; only OK permits freeing ctx. Never wait inside a callback. */
  BFA_SpeechResult (*UnregisterListener)(uint64_t listener);
  BFA_SpeechResult (*SetMood)(uint32_t actor, const BFA_SpeechEmotion*);
  uint32_t (*Owner)(uint32_t actor); /* BFA_SpeechOwner, all BFA providers */
} BFA_SpeechInterface;
typedef const BFA_SpeechInterface* (*BFA_QuerySpeechInterfaceFn)(uint32_t);
#ifdef __cplusplus
}
#endif
#endif
