#pragma once
#include "json.hpp"
namespace ItemInteraction
{
void Initialize();
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
