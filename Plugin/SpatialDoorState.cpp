#include "SpatialAwareness.h"
#include "SpatialDoorStatePolicy.h"

#include <mutex>

namespace SpatialAwareness
{
    namespace
    {
        std::mutex g_doorStateMutex;
        SpatialDoorStatePolicy::ObservedDoors g_observedDoors;
    }

    void SetDoorStateCell(RE::FormID cellId)
    {
        std::lock_guard lock(g_doorStateMutex);
        g_observedDoors.SetCell(cellId);
    }

    void ResetDoorStates()
    {
        std::lock_guard lock(g_doorStateMutex);
        g_observedDoors.Reset();
    }

    void ForgetDoorState(RE::FormID doorId)
    {
        std::lock_guard lock(g_doorStateMutex);
        g_observedDoors.Forget(doorId);
    }

    void RecordDoorState(RE::TESObjectREFR* door, bool opened)
    {
        auto* cell = door ? door->GetParentCell() : nullptr;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!cell || !cell->IsInteriorCell() || !player || cell != player->GetParentCell()) {
            return;
        }
        std::lock_guard lock(g_doorStateMutex);
        g_observedDoors.SetCell(cell->GetFormID());
        g_observedDoors.Record(door->GetFormID(), opened);
    }

    // Open/close events survive visual culling. Without an event or usable 3D,
    // the state is unknown, not evidence of a closed-door hearing barrier.
    RE::BGSOpenCloseForm::OPEN_STATE GetDoorState(RE::TESObjectREFR* door)
    {
        using State = RE::BGSOpenCloseForm::OPEN_STATE;
        auto* cell = door ? door->GetParentCell() : nullptr;
        if (!cell || door->IsDisabled() || door->IsDeleted()) {
            return State::kNone;
        }
        {
            std::lock_guard lock(g_doorStateMutex);
            if (const auto opened = g_observedDoors.Get(cell->GetFormID(), door->GetFormID())) {
                return *opened ? State::kOpen : State::kClosed;
            }
        }
        auto* node = door->Get3D();
        if (!node || node->GetAppCulled()) {
            return State::kNone;
        }
        return RE::BGSOpenCloseForm::GetOpenState(door);
    }
}
