#pragma once

// Sharmat-specific plugin additions, kept isolated from CHIM core so upstream merges stay clean.
// (2026-07-05) Gaze tracking: the player staring at an NPC's body/eyes for a while emits a gaze event
// down the existing NSFW physics pipe; the SHARMAT server turns it into an in-character reaction.
namespace Sharmat {
    // Called once per ManagerMainQueue tick. Tracks how long the player's crosshair has dwelled on the
    // same actor and, past a threshold, marshals a game-thread read to classify the gazed body region and
    // emit a gaze event. Cheap + safe to call every tick; does its own dwell/cooldown bookkeeping.
    void PollGaze();
}
