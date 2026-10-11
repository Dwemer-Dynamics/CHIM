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
// IsReady and the decline reasons say so.
//
// Threads: every function may be called from any thread. Engine access
// happens on BFA's side. Pointers passed in are copied before the call
// returns; pointers BFA passes to a listener die when the listener returns.
// Listeners run on a BFA thread, never on the game thread, and may call back
// into the interface.
//
// ABI: append-only. Every struct begins with its version and size; fields
// are never reordered, resized or removed, and new ones go at the end with
// the version incremented. A newer BFA adds functions at the end of
// BFA_Interface and raises BFA_API_VERSION; a caller asking for an older
// version is given the interface, whose size says how much of it exists.

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
  // (else kBadRequest). Null with data.
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

// Why a line or an actor was declined (Prepare's and CanAnimate's reason,
// the kDeclined event, Owner).
enum BFA_Reason {
  BFA_kAccepted = 0,
  BFA_kDisabled = 1,            // BFA's master switch is off
  BFA_kPlayerDisabled = 2,      // player facial animation is off
  BFA_kRaceDisabled = 3,        // the user disabled the actor's race
  BFA_kInScene = 4,             // a SexLab/OStim scene owns the face
  BFA_kNoFaceGenHead = 5,       // no such actor, or one without a FaceGen head
  BFA_kHeadMapFailed = 6,       // the head lacks a morph BFA needs
  BFA_kModelNotLoaded = 7,      // the network is not loaded (yet)
  BFA_kNoDevice = 8,            // neither GPU nor CPU inference is available
  BFA_kSuppressed = 9,          // Suppress() is in effect for the actor
  BFA_kUnsupportedRuntime = 10,  // BFA cannot look actors up on this runtime
  BFA_kBadRequest = 11,         // a null or malformed struct, unknown format
  BFA_kAudioFailed = 12,        // the audio could not be decoded, or is silent
  BFA_kNoHandle = 13,           // no such line handle (ended, or never was)
  BFA_kNotInDialogue = 14,      // Potato mode: only the player and the
                                // actor in dialogue with them are animated
};

// How a line ended (Stop's and StopActor's reason, the kEnded event).
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

// ---------------- events ----------------
enum BFA_Event {
  BFA_kQueued = 0,     // Prepare accepted the line; inference is queued
  BFA_kGenerated = 1,  // its animation is ready (audio_seconds measured)
  BFA_kStarted = 2,    // it is playing on the face
  BFA_kEnded = 3,      // it left the face (reason: BFA_StopReason)
  BFA_kFailed = 4,     // inference failed; the face was never touched
  BFA_kDeclined = 5,   // Start could not take the face (reason: BFA_Reason)
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
  uint32_t version;  // the version BFA implements
  uint32_t size;     // sizeof(BFA_Interface) as BFA built it

  // -- capability --
  // BFA's plugin version, packed as SKSE does: major << 24 | minor << 16 |
  // patch << 4 | build.
  uint32_t (*GetVersion)(void);
  // The network is loaded, the hooks are in place and BFA is enabled.
  bool (*IsReady)(void);
  // 0 no (reason set), 1 yes, 2 probably (the head is not mapped yet:
  // Prepare anyway; a kDeclined event follows if the mapping fails).
  uint32_t (*CanAnimate)(uint32_t actor_form_id, uint32_t* reason);
  // BFA_Owner of the actor's face now.
  uint32_t (*Owner)(uint32_t actor_form_id);

  // -- lines --
  // Queues the line's inference at the front of BFA's queue and returns its
  // handle, or 0 with reason set. The audio is copied; the line's events
  // follow (kQueued now, then kGenerated or kFailed). A provider that has
  // the audio only at playback time may call Start on a handle before its
  // kGenerated: the face catches up over BFA's blend-in once it is ready.
  uint64_t (*Prepare)(const BFA_Line* line, uint32_t* reason);
  // The audio is at offset_seconds now: the line takes the face (or a
  // kDeclined event says why not). False: no such handle.
  bool (*Start)(uint64_t handle, double offset_seconds);
  // The audio paused (a menu); the face holds its pose.
  bool (*Pause)(uint64_t handle);
  // The audio resumed at offset_seconds, or where it paused when negative.
  bool (*Resume)(uint64_t handle, double offset_seconds);
  // The audio is at offset_seconds now (drift correction, a scrub).
  bool (*Seek)(uint64_t handle, double offset_seconds);
  // The line is over (reason: BFA_StopReason): the face is handed back.
  // Also forgets a prepared line that never started.
  bool (*Stop)(uint64_t handle, uint32_t reason);
  // Stops every provider line of the actor (0: of every actor). Returns how
  // many.
  uint32_t (*StopActor)(uint32_t actor_form_id, uint32_t reason);

  // -- events --
  // Registers a listener for every event of every line. Returns its id, or
  // 0. Several listeners may be registered; each is called once per event.
  uint64_t (*RegisterListener)(BFA_Listener listener, void* context);
  bool (*UnregisterListener)(uint64_t listener_id);

  // -- mood and ownership --
  // The actor's mood: used for its lines whose emotion is neutral (gain 0,
  // or a neutral skyrim_type), provider lines and the engine's own alike,
  // until replaced; null clears it. fade_seconds is advisory. Mood values
  // are capped by the user's per-channel maximums like any emotion.
  bool (*SetMood)(uint32_t actor_form_id, const BFA_Emotion* mood,
                  float fade_seconds);
  // BFA leaves the actor's face alone for seconds (0: until Release): its
  // lines end (kAborted) and new ones are declined (kSuppressed). reason is
  // for BFA's log.
  bool (*Suppress)(uint32_t actor_form_id, float seconds, const char* reason);
  bool (*Release)(uint32_t actor_form_id);
} BFA_Interface;

// Exported by BetterFacialAnimations.dll. wanted_version: the
// BFA_API_VERSION the caller was built against. Null when BFA cannot serve
// that version (it is older) or is not running.
typedef const BFA_Interface* (*BFA_QueryInterfaceFn)(uint32_t wanted_version);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // BFA_API_H
