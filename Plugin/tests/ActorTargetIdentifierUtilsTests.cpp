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
    assert(ActorIdentityUtils::BuildReferenceSource("Follower.esp", 0x05001234) ==
           ActorIdentityUtils::BuildReferenceSource("Follower.esp", 0x07001234));
    assert(ActorIdentityUtils::BuildReferenceSource("Light.esp", 0xFE012ABC, true) == "Light.esp/00000ABC");
    assert(ActorIdentityUtils::BuildReferenceSource("Light.esp", 0xFE034ABC, true) == "Light.esp/00000ABC");
    assert(ActorIdentityUtils::BuildReferenceSource("VR.esp", 0xFE012ABC) == "VR.esp/00012ABC");
    assert(ActorIdentityUtils::BuildReferenceSource("Skyrim.esm", 0xFF001234).empty());

    assert(ActorIdentityUtils::BuildProfileKey("Follower.esp/00001234", 0x05001234) ==
           ActorIdentityUtils::BuildProfileKey("FOLLOWER.ESP/00001234", 0x07001234));
    assert(ActorIdentityUtils::BuildProfileKey("Follower.esp/00001234", 0x05001234) ==
           "ref:follower.esp|00001234");
    assert(ActorIdentityUtils::BuildProfileKey("", 0xFF001234) == "runtime:FF001234");

    {
        using namespace ActorIdentityUtils;
        const std::string uuid = FormatDynamicUuid({});
        assert(uuid == "00000000-0000-4000-8000-000000000000");
        assert(IsDynamicUuid(uuid));
        assert(!IsDynamicUuid("00000000-0000-0000-0000-000000000000"));
        assert(!IsDynamicUuid("00000000-0000-4000-8000-00000000000A"));
        assert(!IsDynamicUuid("000000000000-4000-8000-000000000000-"));

        assert(BuildActorKey("Follower.esp/00001234") == "ref:follower.esp|00001234");
        assert(BuildActorKey("Follower.esp/00001234", uuid) == "ref:follower.esp|00001234");
        assert(BuildActorKey("", uuid) == "dyn:" + uuid);
        assert(BuildActorKey("", "").empty());
        assert(BuildActorKey("Bandit", "not-a-uuid").empty());

        assert(IsActorKey("player") && IsActorKey("narrator"));
        assert(IsActorKey("ref:skyrim.esm|0001BDE8"));
        assert(!IsActorKey("ref:odd|name.esp|00000ABC"));
        assert(IsActorKey("dyn:" + uuid));
        assert(!IsActorKey("ref:Skyrim.esm|0001BDE8"));
        assert(!IsActorKey("ref:skyrim.esm|0001bde8"));
        assert(!IsActorKey("ref:skyrim.esm|FF001BDE"));
        assert(!IsActorKey("runtime:FF001234"));
        assert(!IsActorKey("Lydia"));
        assert(!IsActorKey("dyn:00000000-0000-0000-0000-000000000000"));

        const std::vector<DynamicIdentityEntry> entries{
            {0xFF000800, 0x00013BAE, uuid},
            {0xFF000801, 0xFF000900, "12345678-9abc-4def-8012-3456789abcde"},
        };
        const auto data = SerializeDynamicIdentities(entries);
        assert(data.size() == 2 * DynamicIdentityEntrySize);
        const auto parsed = ParseDynamicIdentities(data);
        assert(parsed && parsed->size() == 2);
        assert((*parsed)[1].formId == 0xFF000801 && (*parsed)[1].baseId == 0xFF000900);
        assert((*parsed)[1].uuid == entries[1].uuid);
        assert(ParseDynamicIdentities("") && ParseDynamicIdentities("")->empty());
        assert(!ParseDynamicIdentities(data.substr(1)));
        assert(!ParseDynamicIdentities(data.substr(0, DynamicIdentityEntrySize) +
                                       data.substr(0, DynamicIdentityEntrySize)));
        // Placed references and invalid UUIDs never enter the dynamic registry.
        assert(SerializeDynamicIdentities({{0x05001234, 0, uuid}, {0xFF000802, 0, "bad"}}).empty());
    }
    return 0;
}
