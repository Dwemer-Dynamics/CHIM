#include "ItemIdentifierUtils.h"

#include <cstdlib>
#include <iostream>

namespace
{
    void Check(bool condition, const char* message)
    {
        if (!condition) {
            std::cerr << message << std::endl;
            std::exit(1);
        }
    }
}

int main()
{
    using namespace ItemIdentifierUtils;

    const auto identified = ParseInventoryItemIdentifier("`0x00012EB7:Iron Sword`");
    Check(identified.baseId.has_value(), "BaseID prefix was not parsed");
    Check(identified.baseId.value() == 0x00012EB7, "Parsed BaseID was incorrect");
    Check(identified.name == "Iron Sword", "Parsed inventory name was incorrect");
    Check(MatchesRequestedBaseId(identified, 0x00012EB7), "BaseID action matching rejected the requested item");
    Check(!MatchesRequestedBaseId(identified, 0x00012EB8), "BaseID action matching accepted a different item");

    const auto legacy = ParseInventoryItemIdentifier("Iron Sword");
    Check(!legacy.baseId.has_value(), "Legacy name unexpectedly produced a BaseID");
    Check(legacy.name == "Iron Sword", "Legacy item name changed");
    Check(!MatchesRequestedBaseId(legacy, 0x00012EB7), "Name-only fallback incorrectly forced a BaseID match");

    const auto invalid = ParseInventoryItemIdentifier("not-an-id:Iron Sword");
    Check(!invalid.baseId.has_value(), "Malformed BaseID was accepted");
    Check(invalid.name == "not-an-id:Iron Sword", "Malformed identifier did not preserve its name fallback");

    HeldItemTracker tracker;
    tracker.Set("left", 0xFF001234, "Iron Sword");
    tracker.Set("right", 0xFF004321, "Tankard");
    Check(tracker.Get("left").refId == 0xFF001234, "Left-hand RefID was not retained");
    Check(tracker.Get("right").name == "Tankard", "Right-hand item name was not retained");

    const auto leftPickup = BuildHeldItemEvent("Iron Sword", "pickup", "left", tracker.Get("left").refId);
    const auto leftDrop = BuildHeldItemEvent("Iron Sword", "drop", "left", tracker.Get("left").refId);
    Check(leftPickup == "Iron Sword^pickup^left^0xFF001234", "Left pickup protocol was incorrect");
    Check(leftDrop == "Iron Sword^drop^left^0xFF001234", "Left drop protocol was incorrect");

    // Stash and consume callbacks reuse state captured by the preceding grab.
    const auto stashed = tracker.Get("left");
    Check(BuildHeldItemEvent(stashed.name, "drop", "left", stashed.refId) ==
              "Iron Sword^drop^left^0xFF001234",
          "Stash/consume did not retain the grabbed RefID");
    tracker.Clear("left");
    Check(tracker.Get("left").refId == 0, "Cleared hand retained stale state");

    // Flat mode uses the same wire format with the synthetic both-hands slot.
    Check(BuildHeldItemEvent("Goblet", "pickup", "both", 0xFF00ABCD) ==
              "Goblet^pickup^both^0xFF00ABCD",
          "Flat-mode pickup protocol was incorrect");

    // Identically named objects retain distinct wire identities.
    const auto first = BuildHeldItemEvent("Tankard", "pickup", "right", 0xFF000001);
    const auto second = BuildHeldItemEvent("Tankard", "pickup", "right", 0xFF000002);
    Check(first != second, "Distinct RefIDs were debounced into the same identity");

    // Older clients remain representable when no RefID exists.
    Check(BuildHeldItemEvent("Tankard", "pickup", "right", 0) == "Tankard^pickup^right",
          "Legacy held-item protocol was incorrect");
    return 0;
}
