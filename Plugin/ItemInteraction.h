#pragma once
#include "json.hpp"
struct ScriptLine;
namespace ItemInteraction
{
void Initialize();
bool HoldReaction(const ScriptLine &line, bool playback = false, RE::FormID playbackActor = 0);
void Tick();
void NarrationComplete(const std::string &utteranceId, bool completed);
void GrantPower();
void Open();
void Cancel();
void Command(const std::string &command);
bool CanExecute(const std::string &id, int step);
void Complete(const std::string &id, int step, const std::string &status, const std::string &detail);
// Plugin action steps accept completion only from the handler form captured for that request step, before
// its deadline, while that form still owns the registration and the captured target remains eligible.
bool PluginStepPending(const std::string &id, int step, RE::FormID owner);
// Claims the step's single completion; returns false if it is not pending or was already claimed.
bool CompletePluginStep(const std::string &id, int step, RE::FormID owner, const std::string &status,
                        const std::string &detail);
void Register(RE::BSScript::IVirtualMachine *vm);
} // namespace ItemInteraction
