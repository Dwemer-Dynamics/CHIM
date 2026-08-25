#include "ActorIdentityUtils.h"
#include "ActorTargetIdentifierUtils.h"

#include <cassert>

int main()
{
    using ActorTargetIdentifierUtils::Parse;

    {
        const auto target = Parse("Alvor [RefID: 00013475]");
        assert(target.hasRefId);
        assert(target.refId == 0x00013475);
        assert(target.fallbackName == "Alvor");
    }

    {
        const auto target = Parse("[RefID: 0xFF001234] Bandit");
        assert(target.hasRefId);
        assert(target.refId == 0xFF001234);
        assert(target.fallbackName == "Bandit");
    }

    {
        const auto target = Parse("Camilla Valerius");
        assert(!target.hasRefId);
        assert(target.refId == 0);
        assert(target.fallbackName == "Camilla Valerius");
    }

    {
        const auto target = Parse("Invalid [RefID: NOTHEX]");
        assert(!target.hasRefId);
        assert(target.fallbackName == "Invalid [RefID: NOTHEX]");
    }

    {
        const auto target = Parse("Alvor [RefID: 00000000]");
        assert(!target.hasRefId);
        assert(target.fallbackName == "Alvor");
    }

    assert(ActorIdentityUtils::BuildPromptIdentifier("Bandit", 0xFF001234) ==
           "Bandit [RefID: FF001234]");
    assert(ActorIdentityUtils::BuildPlacedActorKey("Example Bandits.esp", 0x1234) ==
           "skyrim-ref-v1:4578616d706c652042616e646974732e657370:00001234");
    assert(ActorIdentityUtils::BuildRuntimeActorKey("ABC-123") == "skyrim-runtime-v1:abc123");
    assert(ActorIdentityUtils::NormalizeRuntimeInstanceId("ABC-123") == "abc123");

    return 0;
}
