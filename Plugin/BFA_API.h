// BFA_API.h - the provider interface of Better Facial Animations (BFA).
//
// This header may be freely used, modified and redistributed, including in
// closed-source projects. It declares a plain C interface: no BFA code, no
// linking against BetterFacialAnimations.dll, nothing beyond <stdint.h>.
//
// A speech provider (an AI dialogue mod, a TTS mod, anything that plays a
// voice line the engine never sees) hands BFA the line's audio and tells it
// when playback starts and ends; BFA runs Audio2Face on the audio and
// animates the speaker's face, lips, jaw, tongue, brows and eyes, with the
// line's emotion. When BFA declines a line, nothing on that face changes and
// the provider keeps its own lip sync.
//
// Discovery, at or after SKSE's kPostLoad (plugins load in an unknown order):
//
//   HMODULE bfa = GetModuleHandleA("BetterFacialAnimations.dll");
//   auto query = bfa ? (BFA_QueryInterfaceFn)GetProcAddress(bfa, "BFA_QueryInterface") : nullptr;
//   const BFA_Interface* api = query ? query(BFA_API_VERSION) : nullptr;
//
// or through SKSE messaging: register a listener for the sender
// "BetterFacialAnimations". At kPostPostLoad BFA sends every listener
// BFA_kInterface, whose data is the same const BFA_Interface*; a plugin that
// registers later dispatches BFA_kRequestInterface (no data) to the receiver
// "BetterFacialAnimations" and is answered the same way. A null interface
// means BFA is absent or older than the version asked for: keep the lips you
// have. The interface is handed out even while BFA is disabled by the user;
// GetReadiness and the decline reasons say so.
//
// Threads: every function may be called from any thread. Engine access
// happens on BFA's side. Pointers passed in are copied before the call
// returns; pointers BFA passes to a listener die when the listener returns.
// Listeners run on a BFA thread, never on the game thread, and may call back
// into the interface.
//
// Unreleased ABI: changed in place. Rebuild all callers against this header.
// Query exactly BFA_API_VERSION; the version value has not been bumped.
// Functions return a single BFA_Result (uint32_t), never bool.
// Pending is not proof of visible animation.
// GetStatus/events report subsequent progress. Struct versions must equal
// BFA_API_VERSION and size must cover the known fields.
// UnregisterListener prevents future dispatch snapshots; callbacks already copied
// may still run. Keep callback context alive for the plugin lifetime.

#ifndef BFA_API_H
#define BFA_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BFA_API_VERSION 1u

// SKSE messaging: sender / receiver name and the two message types.
#define BFA_PLUGIN_NAME "BetterFacialAnimations"
enum BFA_MessageType {
  BFA_kRequestInterface = 0x42464101,  // provider -> BFA, no data
  BFA_kInterface = 0x42464102,  // BFA -> provider, data = const BFA_Interface*
};

// ---------------- audio ----------------
enum BFA_AudioFormat {
  BFA_kContainer = 0,  // .wav, .xwm or .fuz bytes, as the game ships them
  BFA_kPcm16 = 1,      // interleaved signed 16-bit samples
  BFA_kPcmF32 = 2,     // interleaved 32-bit float samples, -1..1
};

typedef struct BFA_Audio {
  uint32_t version;  // BFA_API_VERSION
  uint32_t size;     // sizeof(BFA_Audio)
  uint32_t format;   // BFA_AudioFormat
  uint32_t sample_rate;  // PCM only; any rate, BFA resamples to 16 kHz
  uint32_t channels;     // PCM only, 1 or 2 (mixed down)
  uint32_t bytes;        // size of data
  const void* data;      // copied; may be null when path is given
  // Alternative to data: a file relative to the game's Data directory,
  // UTF-8, read through the engine's resource system (BSAs and loose files),
  // e.g. "Sound\\Voice\\MyMod.esp\\line.wav"; it must lie under Data\\Sound
  // (else BFA_ResultInvalidArgument). Null with data.
  const char* path;
} BFA_Audio;

// ---------------- emotion ----------------
// Either the game's own dialogue emotion (skyrim_type 0..7: Neutral, Anger,
// Disgust, Fear, Sad, Happy, Surprise, Puzzled, at skyrim_value 0..100), or,
// with skyrim_type -1, the ten Audio2Face emotion channels directly. gain
// scales either; 0 means neutral (and lets a mood set with SetMood apply).
enum BFA_EmotionChannel {
  BFA_kAmazement,
  BFA_kAnger,
  BFA_kCheekiness,
  BFA_kDisgust,
  BFA_kFear,
  BFA_kGrief,
  BFA_kJoy,
  BFA_kOutOfBreath,
  BFA_kPain,
  BFA_kSadness,
  BFA_kNumEmotionChannels
};

typedef struct BFA_Emotion {
  uint32_t version;
  uint32_t size;
  int32_t skyrim_type;   // 0..7, or -1 to use channels
  int32_t skyrim_value;  // 0..100
  float channels[BFA_kNumEmotionChannels];  // each 0..2
  float gain;  // 0 neutral; 1 as the user's emotion strength setting
} BFA_Emotion;

// ---------------- lines ----------------
enum BFA_LineFlags {
  // The last segment of its utterance: the face is handed back normally
  // afterwards. Without it, BFA holds the face for the next segment of the
  // same utterance_id (no expression fade between sentences).
  BFA_kLastSegment = 1u << 0,
  // Keep the provider's expression (SetExpressionOverride and the like) in
  // place while the line plays. By default BFA fades the engine's
  // expression keyframes out and carries the emotion through the network.
  BFA_kKeepProviderExpression = 1u << 1,
  // Hold the face for the whole utterance: an engine line (idle chatter, a
  // combat bark) does not take it over between segments.
  BFA_kClaimUtterance = 1u << 2,
};

typedef struct BFA_Line {
  uint32_t version;
  uint32_t size;
  uint64_t utterance_id;    // the provider's id for the whole reply; any value
  uint32_t segment;         // 0-based within the utterance
  uint32_t flags;           // BFA_LineFlags
  uint32_t speaker_form_id;  // the actor's form id; 0x14 is the player
  uint32_t reserved;
  BFA_Audio audio;
  BFA_Emotion emotion;
  const char* text;  // optional, UTF-8, for BFA's log only
} BFA_Line;


// How a line ended (Stop/StopActor input, status.stop_reason, kEnded event).
enum BFA_StopReason {
  BFA_kCompleted = 0,   // played to its end
  BFA_kSkipped = 1,     // the user skipped it
  BFA_kSuperseded = 2,  // replaced by another line
  BFA_kAborted = 3,     // cancelled (the provider, a load, BFA)
};

// What a face is doing (Owner, the kOwnerChanged event).
enum BFA_Owner {
  BFA_kOwnerEngine = 0,    // the engine's own speech and expressions
  BFA_kOwnerProvider = 1,  // a provider's line is playing through BFA
  BFA_kOwnerSuppressed = 2,  // Suppress() is in effect
};

// ---------------- operation results and retained status ----------------
// One fixed-width result per call; compare against these values, not truthiness.
// Specific failures (e.g. Disabled or AudioFailed) are returned directly.
typedef uint32_t BFA_Result;
enum BFA_ResultCode {
  BFA_ResultOK = 0,
  BFA_ResultPending = 1, // accepted; generation or face cleanup pending
  BFA_ResultNoChange = 2, // already in the requested state
  BFA_ResultAlreadyStopped = 3, // retained terminal handle; GetStatus has its outcome
  BFA_ResultInvalidArgument = 4,
  BFA_ResultInvalidHandle = 5, // unknown, released or expired handle
  BFA_ResultInvalidState = 6, // operation not legal in the current state
  BFA_ResultNotFound = 7, // unknown listener
  BFA_ResultUnavailable = 8, // plugin hooks not ready
  BFA_ResultDisabled = 9, // master switch off
  BFA_ResultPlayerDisabled = 10,
  BFA_ResultRaceDisabled = 11,
  BFA_ResultInScene = 12, // a scene owns the face
  BFA_ResultNoFaceGenHead = 13, // missing actor or FaceGen head
  BFA_ResultHeadMapFailed = 14, // required morphs unavailable
  BFA_ResultModelNotLoaded = 15, // network not loaded yet
  BFA_ResultNoDevice = 16, // no inference device available
  BFA_ResultSuppressed = 17,
  BFA_ResultUnsupportedRuntime = 18, // actor lookup unsupported on this runtime
  BFA_ResultAudioFailed = 19, // decode failed or audio silent
  BFA_ResultNotInDialogue = 20, // excluded by Potato mode
  BFA_ResultInternalError = 21,
  BFA_ResultInferenceFailed = 22, // animation generation failed
  BFA_ResultCancelled = 23,
};

// Generation and playback are independent: audio may start/pause while inference
// is queued. Playing means the provider clock runs, NOT that a rendered face
// frame has been applied. face_owned reports BFA's current ownership.
enum BFA_GenerationState {
  BFA_GenerationQueued = 0,
  BFA_GenerationRunning = 1,
  BFA_GenerationReady = 2,
  BFA_GenerationFailed = 3,
};
enum BFA_PlaybackState {
  BFA_PlaybackPrepared = 0,
  BFA_PlaybackPlaying = 1,
  BFA_PlaybackPaused = 2,
  BFA_PlaybackStopping = 3,
  BFA_PlaybackEnded = 4,
  BFA_PlaybackFailed = 5,
  BFA_PlaybackDeclined = 6,
};
typedef struct BFA_LineStatus {
  uint32_t version; // caller sets BFA_API_VERSION
  uint32_t size;    // caller sets sizeof(BFA_LineStatus)
  uint64_t handle;
  uint64_t utterance_id;
  uint32_t actor_form_id;
  uint32_t segment;
  uint32_t generation; // BFA_GenerationState
  uint32_t playback;   // BFA_PlaybackState
  BFA_Result result;   // latest lifecycle result, not a history of API misuse
  uint32_t stop_reason; // BFA_StopReason; meaningful for Stopping/Ended
  uint32_t face_owned;  // 0/1
  double audio_seconds;    // 0 until decoded
  double position_seconds; // frozen at terminal transition
} BFA_LineStatus;

// Terminal records retain no audio/actor pointer. They expire after 120 seconds,
// capped at 1024 (oldest evicted first). Copy before ReleaseHandle if needed later.
// Active handles cannot be explicitly released.
#define BFA_TERMINAL_RETENTION_SECONDS 120u
#define BFA_TERMINAL_CAPACITY 1024u

// ---------------- events ----------------
enum BFA_Event {
  BFA_kQueued = 0,     // Prepare accepted the line; inference is queued
  BFA_kGenerated = 1,  // its animation is ready (audio_seconds measured)
  BFA_kStarted = 2,    // clock started and ownership accepted; generation may be pending
  BFA_kEnded = 3,      // it left the face (reason: BFA_StopReason)
  BFA_kFailed = 4,     // inference failed; the face was never touched
  BFA_kDeclined = 5,   // Start could not take the face (reason: BFA_ResultCode)
  BFA_kOwnerChanged = 6,  // reason: the actor's new BFA_Owner
};

typedef struct BFA_EventData {
  uint32_t version;
  uint32_t size;
  uint32_t event;  // BFA_Event
  uint32_t reason;
  uint64_t handle;        // 0 for kOwnerChanged
  uint64_t utterance_id;  // the line's
  uint32_t segment;
  uint32_t actor_form_id;
  float audio_seconds;  // kGenerated: the clip's length as decoded
  uint32_t reserved;
} BFA_EventData;

typedef void (*BFA_Listener)(const BFA_EventData* event, void* context);

// ---------------- the interface ----------------
typedef struct BFA_Interface {
  uint32_t version;
  uint32_t size;
  uint32_t (*GetVersion)(void);
  BFA_Result (*GetReadiness)(void);
  BFA_Result (*CanAnimate)(uint32_t actor_form_id); // Pending: head not mapped
  uint32_t (*Owner)(uint32_t actor_form_id); // snapshot, not a write lease

  // handle_out is zero on invalid/declined intake. Once allocated, even immediate
  // failure returns a nonzero inspectable terminal handle. Inputs are copied.
  BFA_Result (*Prepare)(const BFA_Line* line, uint64_t* handle_out);
  // Finite offsets >=0. Legal while generation is pending. Repeated Start seeks.
  BFA_Result (*Start)(uint64_t handle, double offset_seconds);
  BFA_Result (*Pause)(uint64_t handle); // InvalidState before Start
  // -1 resumes from current position; other negative/nonfinite offsets invalid.
  BFA_Result (*Resume)(uint64_t handle, double offset_seconds);
  BFA_Result (*Seek)(uint64_t handle, double offset_seconds);
  BFA_Result (*Stop)(uint64_t handle, uint32_t stop_reason);
  // actor 0 stops all. stopped_out counts newly stopped handles.
  BFA_Result (*StopActor)(uint32_t actor_form_id, uint32_t stop_reason,
                          uint32_t* stopped_out);
  BFA_Result (*GetStatus)(uint64_t handle, BFA_LineStatus* status_out);
  BFA_Result (*ReleaseHandle)(uint64_t handle);
  BFA_Result (*RegisterListener)(BFA_Listener listener, void* context,
                                 uint64_t* listener_out);
  BFA_Result (*UnregisterListener)(uint64_t listener_id);
  // nullptr clears mood. Only immediate changes supported: nonzero fade rejected.
  BFA_Result (*SetMood)(uint32_t actor_form_id, const BFA_Emotion* mood,
                        float fade_seconds);
  // 0 seconds: until Release. Negative/nonfinite durations rejected.
  BFA_Result (*Suppress)(uint32_t actor_form_id, float seconds, const char* reason);
  BFA_Result (*Release)(uint32_t actor_form_id);
} BFA_Interface;

// Exported by BetterFacialAnimations.dll. wanted_version: the
// exact BFA_API_VERSION the caller was built against. Null for other versions.
typedef const BFA_Interface* (*BFA_QueryInterfaceFn)(uint32_t wanted_version);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // BFA_API_H
