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
void Register(RE::BSScript::IVirtualMachine *vm);
} // namespace ItemInteraction
