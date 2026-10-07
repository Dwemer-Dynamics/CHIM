#include "InteractExtensions.h"
#include "ItemInteraction.h"
#include "PlaythroughSession.h"
#include <cmath>
#include <map>
#include <mutex>

namespace InteractExtensions
{
namespace
{
struct Entry
{
    Descriptor descriptor;
    std::uint64_t generation = 0;
};

constexpr std::size_t maxRegistrations = 32;
constexpr std::size_t maxPerHandler = 16;
constexpr std::size_t maxEligible = 16;
constexpr std::size_t maxDescriptionBytes = 240;
constexpr std::size_t maxDetailBytes = 200;
constexpr float valueLimit = 1000000.0f;

// RegisterAction results; positive values succeed. Keep in sync with CHIMInteractExtensions.psc.
constexpr int registered = 1;
constexpr int updated = 2;
constexpr int invalidHandler = -1;
constexpr int invalidId = -2;
constexpr int invalidDescription = -3;
constexpr int invalidTargets = -4;
constexpr int invalidItemRequirement = -5;
constexpr int invalidBounds = -6;
constexpr int ownedByAnotherHandler = -7;
constexpr int registryFull = -8;

std::mutex registryMutex;
std::map<std::string, Entry> registry;

// Entries from an earlier load are never dispatched or revived; they are dropped on the next change.
void PurgeStale()
{
    const auto generation = PlaythroughSession::Generation();
    std::erase_if(registry, [generation](const auto &entry) { return entry.second.generation != generation; });
}

bool ValidSegment(std::string_view segment)
{
    if (segment.empty() || segment.size() > 31 || segment[0] < 'a' || segment[0] > 'z') return false;
    return std::all_of(segment.begin(), segment.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

// Papyrus may return an interned string with different casing; IDs are canonical lowercase.
std::string CanonicalId(std::string id)
{
    for (auto &c : id)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    const auto colon = id.find(':');
    if (colon == std::string::npos || id.find(':', colon + 1) != std::string::npos) return {};
    const auto ns = std::string_view(id).substr(0, colon);
    if (ns == "chim" || !ValidSegment(ns) || !ValidSegment(std::string_view(id).substr(colon + 1))) return {};
    return id;
}

// Keep complete UTF-8 sequences, replace control or malformed bytes and truncate on a boundary.
std::string BoundedText(const std::string &text, std::size_t limit, bool &changed)
{
    std::string result;
    changed = false;
    for (std::size_t i = 0; i < text.size();)
    {
        const auto c = static_cast<unsigned char>(text[i]);
        std::size_t length = 0;
        if (c < 0x80)
            length = 1;
        else if (c >= 0xC2 && c <= 0xDF)
            length = 2;
        else if (c >= 0xE0 && c <= 0xEF)
            length = 3;
        else if (c >= 0xF0 && c <= 0xF4)
            length = 4;
        bool valid = length > 0 && i + length <= text.size();
        for (std::size_t k = 1; valid && k < length; ++k)
            valid = (static_cast<unsigned char>(text[i + k]) & 0xC0) == 0x80;
        if (valid && length == 3)
        {
            const auto next = static_cast<unsigned char>(text[i + 1]);
            valid = !(c == 0xE0 && next < 0xA0) && !(c == 0xED && next > 0x9F);
        }
        if (valid && length == 4)
        {
            const auto next = static_cast<unsigned char>(text[i + 1]);
            valid = !(c == 0xF0 && next < 0x90) && !(c == 0xF4 && next > 0x8F);
        }
        if (valid && length == 1 && (c < 0x20 || c == 0x7F)) valid = false;
        const std::string piece = valid ? text.substr(i, length) : std::string("?");
        if (!valid) changed = true;
        if (result.size() + piece.size() > limit)
        {
            changed = true;
            break;
        }
        result += piece;
        i += valid ? length : 1;
    }
    return result;
}

// Only persistent forms can own actions; temporary references may be reused by the engine.
bool UsableHandler(RE::TESForm *handler)
{
    if (!handler || handler->GetFormID() == 0 || (handler->GetFormID() >> 24) == 0xFF) return false;
    auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
    auto policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
    return policy && policy->GetHandleForObject(handler->GetFormType(), handler) != policy->EmptyHandle();
}

std::int32_t RegisterAction(RE::StaticFunctionTag *, RE::TESForm *handler, std::string actionId, std::string description,
                            std::int32_t targets, std::int32_t item, float minValue, float maxValue, bool wholeNumber)
{
    if (!UsableHandler(handler)) return invalidHandler;
    const auto id = CanonicalId(actionId);
    if (id.empty()) return invalidId;
    bool changed = false;
    auto text = BoundedText(description, maxDescriptionBytes, changed);
    while (!text.empty() && text.back() == ' ') text.pop_back();
    if (changed || text.empty() || text.front() == ' ') return invalidDescription;
    if (targets < 1 || targets > 7) return invalidTargets;
    if (item < ItemOptional || item > ItemForbidden) return invalidItemRequirement;
    if (!std::isfinite(minValue) || !std::isfinite(maxValue) || minValue > maxValue || std::abs(minValue) > valueLimit ||
        std::abs(maxValue) > valueLimit || (wholeNumber && std::ceil(minValue) > std::floor(maxValue)))
        return invalidBounds;
    Descriptor descriptor{id, text, static_cast<std::uint32_t>(targets), item, minValue, maxValue, wholeNumber, handler->GetFormID()};
    std::lock_guard lock(registryMutex);
    PurgeStale();
    const auto existing = registry.find(id);
    if (existing != registry.end())
    {
        if (existing->second.descriptor.owner != descriptor.owner) return ownedByAnotherHandler;
        existing->second.descriptor = descriptor;
        SKSE::log::info("[INTERACT] Plugin action {} updated by {:08X}", id, descriptor.owner);
        return updated;
    }
    const auto owned = std::count_if(registry.begin(), registry.end(),
        [&](const auto &entry) { return entry.second.descriptor.owner == descriptor.owner; });
    if (registry.size() >= maxRegistrations || static_cast<std::size_t>(owned) >= maxPerHandler) return registryFull;
    registry[id] = {descriptor, PlaythroughSession::Generation()};
    SKSE::log::info("[INTERACT] Plugin action {} registered by {:08X}", id, descriptor.owner);
    return registered;
}

bool UnregisterAction(RE::StaticFunctionTag *, RE::TESForm *handler, std::string actionId)
{
    const auto id = CanonicalId(actionId);
    if (!handler || id.empty()) return false;
    std::lock_guard lock(registryMutex);
    PurgeStale();
    const auto existing = registry.find(id);
    if (existing == registry.end() || existing->second.descriptor.owner != handler->GetFormID()) return false;
    registry.erase(existing);
    SKSE::log::info("[INTERACT] Plugin action {} unregistered by {:08X}", id, handler->GetFormID());
    return true;
}

bool IsActionPending(RE::StaticFunctionTag *, RE::TESForm *handler, std::string requestId, std::int32_t step)
{
    return handler && ItemInteraction::PluginStepPending(requestId, step, handler->GetFormID());
}

// Only the call that claims the pending step returns true, even when several arrive in the same frame.
bool CompleteAction(RE::StaticFunctionTag *, RE::TESForm *handler, std::string requestId, std::int32_t step,
                    std::string status, std::string detail)
{
    bool changed = false;
    return handler && ItemInteraction::CompletePluginStep(requestId, step, handler->GetFormID(), status,
                                                          BoundedText(detail, maxDetailBytes, changed));
}
} // namespace

bool TargetEligible(const Descriptor &descriptor, RE::TESObjectREFR *target)
{
    if (!target || target->IsDeleted() || target->IsDisabled() || !target->Is3DLoaded()) return false;
    auto actor = target->As<RE::Actor>();
    const auto kind = !actor ? TargetObject : (actor->IsDead() ? TargetDeadActor : TargetLivingActor);
    return (descriptor.targets & kind) != 0;
}

std::vector<Descriptor> Eligible(RE::TESObjectREFR *target, bool hasItem)
{
    std::vector<Descriptor> result;
    std::lock_guard lock(registryMutex);
    const auto generation = PlaythroughSession::Generation();
    for (const auto &[id, entry] : registry)
    {
        const auto &descriptor = entry.descriptor;
        if (entry.generation != generation || !TargetEligible(descriptor, target) ||
            (descriptor.item == ItemRequired && !hasItem) || (descriptor.item == ItemForbidden && hasItem))
            continue;
        result.push_back(descriptor);
        if (result.size() == maxEligible) break;
    }
    return result;
}

nlohmann::json Payload(const std::vector<Descriptor> &descriptors)
{
    static const char *items[] = {"optional", "required", "forbidden"};
    auto result = nlohmann::json::array();
    for (const auto &descriptor : descriptors)
    {
        auto targets = nlohmann::json::array();
        if (descriptor.targets & TargetLivingActor) targets.push_back("living_actor");
        if (descriptor.targets & TargetDeadActor) targets.push_back("dead_actor");
        if (descriptor.targets & TargetObject) targets.push_back("object");
        result.push_back({{"version", ApiVersion}, {"id", descriptor.id}, {"description", descriptor.description},
                          {"targets", targets}, {"item", items[descriptor.item]}, {"min", descriptor.minValue},
                          {"max", descriptor.maxValue}, {"whole", descriptor.wholeNumber}});
    }
    return result;
}

bool OwnedBy(const std::string &id, RE::FormID owner)
{
    std::lock_guard lock(registryMutex);
    const auto existing = registry.find(id);
    return existing != registry.end() && existing->second.generation == PlaythroughSession::Generation() &&
           existing->second.descriptor.owner == owner;
}

// Deliver one event to the captured owner only; SendEvent reports no delivery, so Tick owns the timeout.
bool Dispatch(const Descriptor &descriptor, const std::string &requestId, int step, RE::TESObjectREFR *target,
              RE::TESForm *itemBase, float value)
{
    auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
    auto policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
    auto handler = RE::TESForm::LookupByID(descriptor.owner);
    if (!policy || !handler) return false;
    const auto handle = policy->GetHandleForObject(handler->GetFormType(), handler);
    if (handle == policy->EmptyHandle()) return false;
    SKSE::log::info("[INTERACT] Plugin action dispatch id={} step={} action={} handler={:08X}", requestId, step,
                    descriptor.id, descriptor.owner);
    auto args = RE::MakeFunctionArguments(std::string(requestId), static_cast<std::int32_t>(step), std::string(descriptor.id),
                                          std::move(target), std::move(itemBase), std::move(value));
    vm->SendEvent(handle, RE::BSFixedString("OnCHIMInteractAction"), args);
    return true;
}

void NotifyReady()
{
    static std::uint64_t notified = 0;
    const auto generation = PlaythroughSession::Generation();
    if (generation == notified || !PlaythroughSession::Allowed(generation)) return;
    auto source = SKSE::GetModCallbackEventSource();
    if (!source) return;
    notified = generation;
    SKSE::ModCallbackEvent event{RE::BSFixedString(ReadyEvent), RE::BSFixedString(""), static_cast<float>(ApiVersion), nullptr};
    source->SendEvent(&event);
    SKSE::log::info("[INTERACT] Plugin actions ready for load {}", generation);
}

void Register(RE::BSScript::IVirtualMachine *vm)
{
    vm->RegisterFunction("GetApiVersion", "CHIMInteractExtensions",
                         +[](RE::StaticFunctionTag *) { return static_cast<std::int32_t>(ApiVersion); }, false);
    vm->RegisterFunction("RegisterAction", "CHIMInteractExtensions", RegisterAction, false);
    vm->RegisterFunction("UnregisterAction", "CHIMInteractExtensions", UnregisterAction, false);
    vm->RegisterFunction("IsActionPending", "CHIMInteractExtensions", IsActionPending, false);
    vm->RegisterFunction("CompleteAction", "CHIMInteractExtensions", CompleteAction, false);
}
} // namespace InteractExtensions
