#pragma once
#include "json.hpp"
#include <cstdint>
#include <string>
#include <vector>

// Game-plugin actions for Interact: bounded descriptors owned by a Papyrus handler form.
// Registrations live only for the current game load; handlers re-register after every load.
namespace InteractExtensions
{
constexpr int ApiVersion = 1;
constexpr std::uint32_t TargetLivingActor = 1;
constexpr std::uint32_t TargetDeadActor = 2;
constexpr std::uint32_t TargetObject = 4;
constexpr int ItemOptional = 0;
constexpr int ItemRequired = 1;
constexpr int ItemForbidden = 2;
constexpr const char *ReadyEvent = "CHIM_InteractActionsReady";

struct Descriptor
{
    std::string id;
    std::string description;
    std::uint32_t targets = 0;
    int item = ItemOptional;
    float minValue = 0.0f;
    float maxValue = 0.0f;
    bool wholeNumber = false;
    RE::FormID owner = 0;
};

// Copies the current load's registrations that accept this target and item selection.
std::vector<Descriptor> Eligible(RE::TESObjectREFR *target, bool hasItem);
nlohmann::json Payload(const std::vector<Descriptor> &descriptors);
bool TargetEligible(const Descriptor &descriptor, RE::TESObjectREFR *target);
bool OwnedBy(const std::string &id, RE::FormID owner);
bool Dispatch(const Descriptor &descriptor, const std::string &requestId, int step, RE::TESObjectREFR *target,
              RE::TESForm *itemBase, float value);
// Game thread only. Sends ReadyEvent once per load after that load is allowed, so handlers that registered
// before the load began (such as a new game's OnInit) register again for it.
void NotifyReady();
void Register(RE::BSScript::IVirtualMachine *vm);
} // namespace InteractExtensions
