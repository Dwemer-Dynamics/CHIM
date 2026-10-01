#include "ActorIdentityUtils.h"
#include "ActorTargetIdentifierUtils.h"
#include "DiaryBookIdentityUtils.h"
#include "EventIdentityUtils.h"
#include "GameDataDeliveryCache.h"

#include <algorithm>
#include <cassert>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

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
    {
        using EventIdentityUtils::EventIdentity;
        const std::string placed = "ref:skyrim.esm|0001BDE8";
        const std::string placedTwin = "ref:skyrim.esm|0001BDE9";
        const std::string dynamic = "dyn:0f8b2c1e-4a5d-4e6f-9a7b-1c2d3e4f5a6b";
        auto identity = EventIdentity::Capture();
        assert(identity.AddParticipant("Astrid", placed));
        assert(identity.AddParticipant("Bandit", dynamic));
        assert(identity.AddParticipant("Bandit", placedTwin));  // same name, different physical actor stays
        assert(identity.AddParticipant("Bandit", ""));           // unidentified namesake stays keyless
        assert(identity.AddParticipant("Bandit", ""));           // same unkeyed name collapses
        assert(identity.AddParticipant("Astrid", placed));       // same key collapses
        identity.SetSpeaker(std::string(ActorIdentityUtils::PlayerActorKey));
        identity.AddListener(placed);
        identity.AddListener(placed);
        identity.AddListener("");
        identity.SetTarget("");
        assert(identity.Sendable());
        assert(identity.participants.size() == 4);
        assert(identity.participants[3].id.empty());

        // Routing fields already in the snapshot are preserved; roles never add audience members.
        nlohmann::json snapshot = {{"source", "plugin_player_routing_v2"}, {"listener", "Astrid"}};
        EventIdentityUtils::Append(snapshot, identity);
        assert(snapshot["source"] == "plugin_player_routing_v2");
        assert(snapshot["identity_version"] == 2);
        assert(snapshot["participants"].size() == 4);
        assert(snapshot["participants"][0]["id"] == placed);
        assert(snapshot["participants"][1]["id"] == dynamic);
        assert(snapshot["participants"][2]["id"] == placedTwin);
        assert(!snapshot["participants"][3].contains("id"));
        assert(snapshot["speaker_key"] == "player");
        assert(snapshot["listener_keys"] == nlohmann::json::array({placed}));
        assert(!snapshot.contains("target_key"));

        // The encoded request is a value copy: later mutation of the capture cannot reach a queued request.
        const auto queued = EventIdentityUtils::Serialize(identity);
        auto queuedRequest = std::string("_speech|1|2|{}");
        assert(EventIdentityUtils::AttachToRequest(queuedRequest, identity));
        identity.AddParticipant("Late", "ref:skyrim.esm|00013475");
        assert(nlohmann::json::parse(queued)["participants"].size() == 4);
        assert(queuedRequest.ends_with("|" + EventIdentityUtils::Base64(queued)));

        // A computed key that is not canonical invalidates the capture; it never degrades to unresolved.
        auto badKey = EventIdentity::Capture();
        assert(!badKey.AddParticipant("Ghost", "runtime:FF000812"));
        assert(!badKey.Valid() && !badKey.Sendable() && EventIdentityUtils::Serialize(badKey).empty());
        auto badRole = EventIdentity::Capture();
        badRole.SetTarget("Astrid");  // a name never becomes a role key
        assert(badRole.invalidReason == "role_key_invalid:target_key");
        auto badName = EventIdentity::Capture();
        assert(!badName.AddParticipant("   ", placed) && !badName.Valid());
        assert(!EventIdentity::Capture().AddParticipant("Bad\x01Name", ""));
        assert(!EventIdentity::Capture().AddParticipant(std::string("\xC3", 1), ""));  // truncated UTF-8
        assert(EventIdentityUtils::IsParticipantName("Ri'saad | Caravan"));
        assert(EventIdentityUtils::IsParticipantName(std::string(256, 'a')));
        assert(!EventIdentityUtils::IsParticipantName(std::string(257, 'a')));

        // Captured but empty: still version 2 with no participants, never the legacy nearby fallback.
        auto empty = EventIdentity::Capture();
        empty.AddListener(std::string(ActorIdentityUtils::NarratorActorKey));
        const auto emptyJson = nlohmann::json::parse(EventIdentityUtils::Serialize(empty));
        assert(emptyJson["identity_version"] == 2);
        assert(emptyJson["participants"] == nlohmann::json::array());
        assert(emptyJson["listener_keys"] == nlohmann::json::array({"narrator"}));
        // Uncaptured stays legacy, even with roles.
        assert(EventIdentityUtils::Serialize(EventIdentity{}).empty());
        EventIdentity rolesOnly;
        rolesOnly.SetSpeaker(placed);
        assert(EventIdentityUtils::Serialize(rolesOnly).empty());
        auto legacyRequest = std::string("chat|1|2|hello");
        assert(!EventIdentityUtils::AttachToRequest(legacyRequest, rolesOnly) && legacyRequest == "chat|1|2|hello");

        // Transport: exactly five pipe fields; JSON data keeps its meaning, text uses the '/' convention.
        auto text = std::string("chat|1|2|Lydia: yes | no");
        assert(EventIdentityUtils::AttachToRequest(text, empty));
        assert(std::count(text.begin(), text.end(), '|') == 4);
        assert(text.starts_with("chat|1|2|Lydia: yes / no|"));
        auto jsonRequest = std::string("_speech|1|2|{\"speech\":\"a|b\",\"companions\":[\"X|Y\"]}");
        assert(EventIdentityUtils::AttachToRequest(jsonRequest, empty));
        assert(std::count(jsonRequest.begin(), jsonRequest.end(), '|') == 4);
        const auto dataStart = std::string("_speech|1|2|").size();
        const auto data = jsonRequest.substr(dataStart, jsonRequest.rfind('|') - dataStart);
        assert(nlohmann::json::parse(data)["speech"] == "a|b");
        assert(nlohmann::json::parse(data)["companions"][0] == "X|Y");
        auto noPrefix = std::string("chat|1");
        assert(!EventIdentityUtils::AttachToRequest(noPrefix, empty) && noPrefix == "chat|1");
        assert(EventIdentityUtils::Base64("Man") == "TWFu" && EventIdentityUtils::Base64("Ma") == "TWE=" &&
               EventIdentityUtils::Base64("M") == "TQ==");

        assert(EventIdentityUtils::IsObservableEventType("infoaction"));
        assert(EventIdentityUtils::IsObservableEventType(EventIdentityUtils::RequestType("death|1|2|x")));
        assert(!EventIdentityUtils::IsObservableEventType("bored"));  // field 4 already carries the seed actor
        // bored: identity is field 5; the location and seed fields stay byte-for-byte, never escaped.
        auto boredIdentity = EventIdentity::Capture();
        assert(boredIdentity.AddParticipant("Bandit", placed) && boredIdentity.AddParticipant("Bandit", placedTwin));
        boredIdentity.SetTarget(placedTwin);
        const auto boredJson = EventIdentityUtils::Serialize(boredIdentity);
        auto bored = std::string("bored|1|2|Whiterun/Plains District|Bandit");
        assert(EventIdentityUtils::AppendField5(bored, boredJson));
        assert(bored == "bored|1|2|Whiterun/Plains District|Bandit|" + EventIdentityUtils::Base64(boredJson));
        const auto boredParsed = nlohmann::json::parse(boredJson);
        assert(boredParsed["participants"].size() == 2 && boredParsed["target_key"] == placedTwin);
        auto boredTwice = bored;
        assert(!EventIdentityUtils::AppendField5(boredTwice, boredJson) && boredTwice == bored);  // no double append
        auto boredPiped = std::string("bored|1|2|Inn|Bandit|Chief");
        assert(!EventIdentityUtils::AppendField5(boredPiped, boredJson) && boredPiped == "bored|1|2|Inn|Bandit|Chief");
        assert(!EventIdentityUtils::IsObservableEventType("addnpc"));
        assert(!EventIdentityUtils::IsObservableEventType("_speech"));
        // rechat/memory are captured from their speaker's scope, never the player's generic event scope.
        assert(EventIdentityUtils::IsSpeakerScopedEventType("rechat"));
        assert(EventIdentityUtils::IsSpeakerScopedEventType(EventIdentityUtils::RequestType("memory|1|2|x")));
        assert(!EventIdentityUtils::IsObservableEventType("rechat") && !EventIdentityUtils::IsObservableEventType("memory"));
        assert(!EventIdentityUtils::IsSpeakerScopedEventType("quest") && !EventIdentityUtils::IsSpeakerScopedEventType("bored"));
        {
            // A rechat payload is JSON: its '|' is escaped, so field 4 stays the identity and its meaning is exact.
            std::string rechat = "rechat|1|2|{\"speaker\":\"A|B\"}";
            auto speaker = EventIdentityUtils::EventIdentity::Capture();
            assert(speaker.AddParticipant("Astrid", "ref:skyrim.esm|0001BDE8"));
            speaker.SetSpeaker("ref:skyrim.esm|0001BDE8");
            assert(EventIdentityUtils::AttachToRequest(rechat, speaker));
            assert(std::count(rechat.begin(), rechat.end(), '|') == 4);
            assert(rechat.find(R"(A\u007cB)") != std::string::npos);
        }
    }

    {
        // Golden cases mirroring chimIsActorKey().
        using ActorIdentityUtils::IsActorKey;
        for (const char* key : {"player", "narrator", "ref:skyrim.esm|0001BDE8", "ref:my mod.esp|00000D62",
                                "ref:x.esl|00FFFFFF", "ref:..esm|00000001", "ref:caf\xC3\xA9.esp|00000001",
                                "dyn:0f8b2c1e-4a5d-4e6f-9a7b-1c2d3e4f5a6b"}) {
            assert(IsActorKey(key));
        }
        for (const char* key : {"", "Player", "ref:Skyrim.esm|0001BDE8", "ref:skyrim.esm|0001bde8",
                                "ref:skyrim.esm|0101BDE8", "ref:skyrim.esm|001BDE8", "ref:skyrim.esx|0001BDE8",
                                "ref:skyrim.ESM|0001BDE8", "ref:skyrim|0001BDE8", "ref:.esm|0001BDE8",
                                "ref: skyrim.esm|0001BDE8", "ref:a:b.esm|0001BDE8", "ref:a/b.esm|0001BDE8",
                                "ref:a\\b.esm|0001BDE8", "ref:a@b.esm|0001BDE8", "ref:a#b.esm|0001BDE8",
                                "ref:a|b.esm|0001BDE8", "ref:a\tb.esm|0001BDE8", "ref:a\x7F" "b.esm|0001BDE8",
                                "ref:skyrim.esm|0001BDE8 ", "runtime:FF000812",
                                "dyn:00000000-0000-0000-0000-000000000000",
                                "dyn:0F8B2C1E-4A5D-4E6F-9A7B-1C2D3E4F5A6B"}) {
            assert(!IsActorKey(key));
        }
        assert(!IsActorKey(std::string_view("ref:a\0b.esm|0001BDE8", 20)));
        // The canonical builder only emits keys the grammar accepts.
        assert(ActorIdentityUtils::BuildActorKey("Skyrim.esm/0001BDE8") == "ref:skyrim.esm|0001BDE8");
        assert(ActorIdentityUtils::BuildActorKey("Mod.esx/00000D62").empty());
        assert(ActorIdentityUtils::BuildActorKey("a:b.esp/00000D62").empty());
    }

    {
        // A dynamic profile selector is the dyn: key itself, distinct from the recyclable runtime form.
        const auto key = ActorIdentityUtils::BuildActorKey({}, "0f8b2c1e-4a5d-4e6f-9a7b-1c2d3e4f5a6b");
        assert(key == "dyn:0f8b2c1e-4a5d-4e6f-9a7b-1c2d3e4f5a6b");
        assert(ActorIdentityUtils::BuildProfileKey({}, 0xFF000812) == "runtime:FF000812");
        assert(key != ActorIdentityUtils::BuildProfileKey({}, 0xFF000812));
        assert(ActorIdentityUtils::BuildActorKey("", "").empty());
    }

    {
        // Physical routing: a decorated identifier selects exactly its RefID, so same-name actors stay distinct,
        // the player's explicit 00000014 is the player, and a narrator-named label decides nothing by itself.
        using ActorTargetIdentifierUtils::IdentifiesActor;
        using ActorTargetIdentifierUtils::Parse;
        assert(IdentifiesActor("Astrid [RefID: 0001BDE8]", "Astrid", 0x0001BDE8));
        assert(!IdentifiesActor("Astrid [RefID: 0004D6D1]", "Astrid", 0x0001BDE8));
        assert(IdentifiesActor("Astrid", "Astrid", 0x0001BDE8));
        assert(IdentifiesActor("  Astrid ", "Astrid", 0x0004D6D1));
        assert(!IdentifiesActor("Astrid", "Lydia", 0x0001BDE8));
        // A label never overrides the reference it carries.
        assert(IdentifiesActor("Lydia [RefID: 0001BDE8]", "Astrid", 0x0001BDE8));
        assert(IdentifiesActor("Dragonborn [RefID: 00000014]", "Player", 0x00000014));
        assert(!IdentifiesActor("The Narrator [RefID: FF000812]", "The Narrator", 0xFF000813));

        const auto player = Parse("Dragonborn [RefID: 00000014]");
        assert(player.hasRefId && player.refId == 0x14 && player.fallbackName == "Dragonborn");
        const auto recycled = Parse("Bandit [RefID: FF000812]");
        assert(recycled.hasRefId && recycled.refId == 0xFF000812 && recycled.fallbackName == "Bandit");
        // A malformed or zero reference stays a bare, unresolved label.
        assert(!Parse("Bandit [RefID: 00000000]").hasRefId);
        assert(!Parse("Bandit [RefID: ]").hasRefId && Parse("Bandit [RefID: ]").fallbackName == "Bandit [RefID: ]");
        assert(!Parse("Bandit [RefID: 1234567890]").hasRefId);
    }

    {
        // Save snapshots keep a dynamic UUID for a reference that is not resident; only positive evidence drops it.
        using ActorIdentityUtils::RetainDynamicIdentity;
        assert(RetainDynamicIdentity(false, false, 0, 0x00013BAE));
        assert(RetainDynamicIdentity(true, false, 0x00013BAE, 0x00013BAE));
        assert(!RetainDynamicIdentity(true, true, 0x00013BAE, 0x00013BAE));
        assert(!RetainDynamicIdentity(true, false, 0x00013BAF, 0x00013BAE));
    }

    {
        // C12: rechat bookkeeping is keyed by canonical role + load generation, never the decorated label.
        using ActorIdentityUtils::CapturedSpeakerId;
        using ActorIdentityUtils::CapturedSpeakerKind;
        using ActorIdentityUtils::RechatBookkeepingKey;
        const CapturedSpeakerId oldActor{CapturedSpeakerKind::Physical, 0xFF000812, "dyn:old", 7};
        const CapturedSpeakerId newActor{CapturedSpeakerKind::Physical, 0xFF000812, "dyn:new", 7};
        const auto oldKey = RechatBookkeepingKey(oldActor, "dyn:old");
        const auto newKey = RechatBookkeepingKey(newActor, "dyn:new");
        // Same FF slot and "Name [RefID: FF000812]" label: the replacement is independent of the old actor's
        // last-rechatter, and the old actor's stale completion never matches (clears) the new in-flight key.
        assert(!oldKey.empty() && !newKey.empty() && oldKey != newKey);
        std::string inFlight = newKey;
        if (oldKey == inFlight) inFlight.clear();
        assert(inFlight == newKey);
        const std::string lastRechatter = oldKey;
        assert(lastRechatter != newKey);
        assert(RechatBookkeepingKey({CapturedSpeakerKind::Physical, 0xFF000812, "dyn:old", 8}, "dyn:old") != oldKey);
        assert(RechatBookkeepingKey(oldActor, "dyn:old") == oldKey);  // Begin and completion share one key.
        // Typed reserved roles; a physical actor with no bound canonical key (or a reserved one) never begins.
        assert(RechatBookkeepingKey({CapturedSpeakerKind::Narrator, 0, "", 7}, "narrator") == "narrator#7");
        assert(RechatBookkeepingKey({CapturedSpeakerKind::Player, 0, "", 7}, "player") == "player#7");
        assert(RechatBookkeepingKey(oldActor, "").empty());
        assert(RechatBookkeepingKey(oldActor, "narrator").empty());
        assert(RechatBookkeepingKey({}, "dyn:old").empty());
    }

    {
        // C11: a speaker captured before TTS or a deferred rechat is rechecked against the values of that moment,
        // never against the same agent entry's own (mutable) fields.
        using ActorIdentityUtils::CapturedSpeakerId;
        using ActorIdentityUtils::CapturedSpeakerKind;
        using ActorIdentityUtils::CapturedSpeakerObservation;
        using ActorIdentityUtils::StillSameCapturedSpeaker;
        const CapturedSpeakerId physical{CapturedSpeakerKind::Physical, 0xFF000812, "dyn:old", 7};
        const CapturedSpeakerObservation same{7, true, false, 0xFF000812, "dyn:old", true};
        assert(StillSameCapturedSpeaker(physical, same));
        auto rekeyed = same;  // The agent entry at that FormID now carries another profile key.
        rekeyed.agentProfileKey = "dyn:new";
        assert(!StillSameCapturedSpeaker(physical, rekeyed));
        auto recycled = same;  // Same key on the entry, but the bound handle/registry key no longer match.
        recycled.boundActorStill = false;
        assert(!StillSameCapturedSpeaker(physical, recycled));
        auto reloaded = same;
        reloaded.loadEpoch = 8;
        assert(!StillSameCapturedSpeaker(physical, reloaded));
        auto gone = CapturedSpeakerObservation{7};
        assert(!StillSameCapturedSpeaker(physical, gone));
        auto becameNarrator = same;
        becameNarrator.agentIsNarrator = true;
        assert(!StillSameCapturedSpeaker(physical, becameNarrator));
        // The narrator is typed: found only as the narrator agent, never as a physical namesake.
        const CapturedSpeakerId narrator{CapturedSpeakerKind::Narrator, 0, "", 7};
        assert(StillSameCapturedSpeaker(narrator, CapturedSpeakerObservation{7, true, true}));
        assert(!StillSameCapturedSpeaker(narrator, CapturedSpeakerObservation{7, true, false, 0xFF000812, "dyn:old", true}));
        assert(!StillSameCapturedSpeaker(narrator, CapturedSpeakerObservation{8, true, true}));
        // The player is typed and needs no agent; the load still has to match.
        const CapturedSpeakerId player{CapturedSpeakerKind::Player, 0, "", 7};
        assert(StillSameCapturedSpeaker(player, CapturedSpeakerObservation{7}));
        assert(!StillSameCapturedSpeaker(player, CapturedSpeakerObservation{8}));
        assert(!StillSameCapturedSpeaker(CapturedSpeakerId{}, same));
    }

    {
        // C7: a queued action is bound to the physical actor, not the agent's cached profile key. A deleted FF
        // actor's stale agent entry keeps its old dyn: key while the slot is recycled for a new actor: refused.
        using ActorIdentityUtils::BoundActor;
        using ActorIdentityUtils::BoundActorObservation;
        using ActorIdentityUtils::StillSameBoundActor;
        const std::string oldKey = ActorIdentityUtils::BuildActorKey({}, "4f1c2a3b-5d6e-4f70-8a9b-0c1d2e3f4a5b");
        const std::string newKey = ActorIdentityUtils::BuildActorKey({}, "9a8b7c6d-5e4f-4a3b-9c2d-1e0f9a8b7c6d");
        assert(ActorIdentityUtils::IsActorKey(oldKey) && ActorIdentityUtils::IsActorKey(newKey) && oldKey != newKey);
        const BoundActor bound{0xFF000812, oldKey};

        const BoundActorObservation same{true, 0xFF000812, true, false, oldKey};
        assert(StillSameBoundActor(bound, same));
        // Same FF slot, same cached agent, new UUID in the registry: refused.
        auto recycled = same;
        recycled.currentActorKey = newKey;
        assert(!StillSameBoundActor(bound, recycled));
        // Deleted and forgotten: the registry no longer holds a key.
        auto forgotten = same;
        forgotten.currentActorKey.clear();
        assert(!StillSameBoundActor(bound, forgotten));
        // The bound handle was released, now names another reference, or the reference is deleted.
        auto released = same;
        released.handleLive = false;
        assert(!StillSameBoundActor(bound, released));
        auto moved = same;
        moved.sameReference = false;
        assert(!StillSameBoundActor(bound, moved));
        auto deleted = same;
        deleted.deleted = true;
        assert(!StillSameBoundActor(bound, deleted));
        auto otherForm = same;
        otherForm.handleFormId = 0xFF000813;
        assert(!StillSameBoundActor(bound, otherForm));
        // An unbound action, or a dynamic binding without a key, never passes.
        assert(!StillSameBoundActor(BoundActor{}, same));
        assert(!StillSameBoundActor(BoundActor{0xFF000812, ""}, same));
        // A static reference is identified by its handle and FormID; the registry is not consulted.
        const BoundActor lydia{0x000A2C94, "static"};
        assert(StillSameBoundActor(lydia, BoundActorObservation{true, 0x000A2C94, true, false, ""}));
        assert(!StillSameBoundActor(lydia, BoundActorObservation{false, 0, false, false, ""}));
        assert(StillSameBoundActor(BoundActor{0x14, "player"}, BoundActorObservation{true, 0x14, true, false, ""}));
    }

    {
        // C7: a bare name selects an actor only when one distinct actor carries it; never the nearest namesake.
        using ActorTargetIdentifierUtils::NameCandidate;
        using ActorTargetIdentifierUtils::SelectUniqueNameMatch;
        bool ambiguous = false;
        const std::vector<NameCandidate> twoGuards{{0x0001A001, "Whiterun Guard", true},
                                                   {0x0001A002, "Whiterun Guard", true}};
        assert(SelectUniqueNameMatch(twoGuards, "Whiterun Guard", &ambiguous) == -1 && ambiguous);
        // An out-of-sight namesake still makes the name ambiguous.
        const std::vector<NameCandidate> oneHidden{{0x0001A001, "Astrid", true}, {0xFF000812, "Astrid", false}};
        assert(SelectUniqueNameMatch(oneHidden, "astrid", &ambiguous) == -1 && ambiguous);
        // One actor seen twice (cell scan and high-actor list) is one candidate; a visible sighting wins.
        const std::vector<NameCandidate> seenTwice{{0x0001BDE8, "Astrid", false}, {0x0001BDE8, "Astrid", true}};
        assert(SelectUniqueNameMatch(seenTwice, "Astrid", &ambiguous) == 1 && !ambiguous);
        // An exact label is preferred over labels that merely contain the name.
        const std::vector<NameCandidate> exactAndPartial{{0x00013475, "Alvor", true},
                                                         {0x00013476, "Alvor Apprentice", true}};
        assert(SelectUniqueNameMatch(exactAndPartial, " Alvor ", &ambiguous) == 0 && !ambiguous);
        // Several partial matches without an exact one are ambiguous.
        const std::vector<NameCandidate> partials{{0x1, "Bandit Chief", true}, {0x2, "Bandit Outlaw", true}};
        assert(SelectUniqueNameMatch(partials, "Bandit", &ambiguous) == -1 && ambiguous);
        // A sole match that fails the visibility rule selects nobody without being ambiguous.
        const std::vector<NameCandidate> unseen{{0x1, "Delphine", false}};
        assert(SelectUniqueNameMatch(unseen, "Delphine", &ambiguous) == -1 && !ambiguous);
        assert(SelectUniqueNameMatch(twoGuards, "", &ambiguous) == -1 && !ambiguous);
    }

    {
        // C7: role commands bind the existing actors their arguments name, never base forms.
        using ActorTargetIdentifierUtils::RoleActorArgKind;
        using ActorTargetIdentifierUtils::RoleCommandActorArgs;
        auto only = [](const std::string& command, std::size_t index, RoleActorArgKind kind) {
            const auto args = RoleCommandActorArgs(command);
            return args.size() == 1 && args[0].index == index && args[0].kind == kind;
        };
        assert(only("moveToPlayer", 0, RoleActorArgKind::AgentName));
        assert(only("stayAtPlace", 0, RoleActorArgKind::AgentName));
        assert(only("TravelTo", 0, RoleActorArgKind::AgentName));
        assert(only("TeleportNPCRaw", 0, RoleActorArgKind::RoleTarget));
        assert(only("TeleportNPC", 0, RoleActorArgKind::RoleTarget));
        assert(only("KillTargetRaw", 0, RoleActorArgKind::RoleTarget));
        assert(only("SpawnItemRaw", 0, RoleActorArgKind::RoleTarget));
        assert(only("SpawnGoldRaw", 0, RoleActorArgKind::RoleTarget));
        assert(only("Despawn", 0, RoleActorArgKind::AgentName));
        assert(only("Instruction", 0, RoleActorArgKind::AgentName));
        assert(only("Suggestion", 0, RoleActorArgKind::AgentName));
        assert(only("Sandbox", 0, RoleActorArgKind::AgentName));
        // Same-name actors: the AgentName argument may be "Name [RefID: X]", which names only that reference.
        assert(Parse("Lydia [RefID: ff000abc]").hasRefId && Parse("Lydia [RefID: ff000abc]").refId == 0xFF000ABC);
        assert(ActorTargetIdentifierUtils::IsUnresolvableExplicit(Parse("Lydia [RefID: 00000000]")));
        assert(only("ShowTrainingMenu", 0, RoleActorArgKind::TrainerName));
        assert(only("BackgroundCmd", 0, RoleActorArgKind::HexRef));
        assert(only("RenameNPC", 0, RoleActorArgKind::HexRef));
        // The book recipient and item location may be an existing actor. The spawned NPC, outfit, weapon and
        // source are base forms, so only the spawn place is considered.
        assert(only("spawnBook", 2, RoleActorArgKind::DecimalRef));
        assert(only("spawnItem", 2, RoleActorArgKind::DecimalRef));
        assert(only("spawnCharacter", 4, RoleActorArgKind::DecimalRef));
        // Commands without an existing actor argument bind nothing, and are not skipped wholesale.
        assert(RoleCommandActorArgs("SpawnNPCRaw").empty());
        assert(RoleCommandActorArgs("DirectorScene").empty());
        assert(RoleCommandActorArgs("StartQuest").empty());
        assert(RoleCommandActorArgs("ScriptProxy").empty());
    }

    {
        // C10: ordinary command actor arguments mirror parseCommand's branch order; items, spells, locations and
        // amounts are never actor arguments.
        using ActorTargetIdentifierUtils::CommandActorArgKind;
        using ActorTargetIdentifierUtils::IsCastSpellNonActorTarget;
        using ActorTargetIdentifierUtils::OrdinaryCommandActorArgs;
        auto only = [](const std::string& command, CommandActorArgKind kind) {
            const auto args = OrdinaryCommandActorArgs(command);
            return args.size() == 1 && args[0].index == 0 && args[0].kind == kind;
        };
        for (const auto* command : {"Attack", "Brawl", "MoveTo", "Inspect", "Follow", "TradeItems"}) {
            assert(only(command, CommandActorArgKind::Name));
        }
        assert(only("GiveGoldTo", CommandActorArgKind::JsonTargetOrName));
        assert(only("GiveItemTo", CommandActorArgKind::JsonTargetOrName));
        const auto cast = OrdinaryCommandActorArgs("CastSpell");
        assert(cast.size() == 2 && cast[0].index == 0 && cast[0].kind == CommandActorArgKind::JsonTarget &&
               cast[1].index == 1 && cast[1].kind == CommandActorArgKind::Name);
        for (const auto* command : {"FollowPlayer", "MakeFollower", "InspectSurroundings", "LookAround", "TravelTo",
                                    "TravelToRaw", "TakeHeldItem", "PickupItem", "Consume", "Halt", "AddBounty",
                                    "TakeGoldFromPlayer", "CommandAnimation", "OpenInventory"}) {
            assert(OrdinaryCommandActorArgs(command).empty());
        }
        assert(IsCastSpellNonActorTarget("CastSpell", " Self "));
        assert(IsCastSpellNonActorTarget("CastSpell", "Target Location"));
        assert(!IsCastSpellNonActorTarget("CastSpell", "Lydia"));
        assert(!IsCastSpellNonActorTarget("Attack", "self"));
    }

    {
        // C7: commandEndedForActor receives "Name [RefID: XXXXXXXX]" built with DecToHex. A malformed reference
        // is still explicit and never falls back to its label.
        using ActorTargetIdentifierUtils::IsUnresolvableExplicit;
        using ActorTargetIdentifierUtils::Parse;
        const auto ended = Parse("Astrid [RefID: FF000812]");
        assert(ended.hasRefId && ended.refId == 0xFF000812 && ended.fallbackName == "Astrid");
        assert(!IsUnresolvableExplicit(ended));
        assert(IsUnresolvableExplicit(Parse("Astrid [RefID: ]")));
        assert(IsUnresolvableExplicit(Parse("Astrid [RefID: 00000000]")));
        assert(!IsUnresolvableExplicit(Parse("Astrid")));
    }

    {
        // C7: death carries the victim as target_key; that role does not add the victim as a witness or speaker.
        const auto victimKey = ActorIdentityUtils::BuildActorKey({}, "4f1c2a3b-5d6e-4f70-8a9b-0c1d2e3f4a5b");
        auto identity = EventIdentityUtils::EventIdentity::Capture();
        identity.AddParticipant("Prisoner", "player");
        identity.SetTarget(victimKey);
        const auto json = nlohmann::json::parse(EventIdentityUtils::Serialize(identity));
        assert(json.at("target_key") == victimKey);
        assert(json.at("participants").size() == 1);
        assert(!json.contains("speaker_key"));
    }

    {
        // C8: response identity v1 (fourth pipe field, base64 JSON).
        using namespace EventIdentityUtils;
        auto line = [](std::string_view label, std::string_view queue, std::string_view payload,
                       const nlohmann::json& identity) {
            return std::string(label) + "|" + std::string(queue) + "|" + std::string(payload) + "|" +
                   Base64(identity.dump());
        };
        const std::string astridKey = "ref:skyrim.esm|0001BDE8";
        const nlohmann::json astrid = {{"id", astridKey}, {"refid", "0001BDE8"}};
        const nlohmann::json narrator = {{"id", "narrator"}, {"refid", nullptr}};
        const nlohmann::json player = {{"id", "player"}, {"refid", "00000014"}};
        const std::string dynKey = "dyn:4f1c2a3b-5d6e-4f70-8a9b-0c1d2e3f4a5b";
        const nlohmann::json dynamic = {{"id", dynKey}, {"refid", "FF000812"}};
        auto envelope = [](nlohmann::json actor, nlohmann::json listener, nlohmann::json targets) {
            return nlohmann::json{{"response_identity_version", 1}, {"actor", std::move(actor)},
                                  {"listener", std::move(listener)}, {"targets", std::move(targets)}};
        };

        // Base64 round trip and strictness.
        assert(DecodeBase64(Base64("a|b/c")) == std::optional<std::string>("a|b/c"));
        assert(!DecodeBase64("abc") && !DecodeBase64("ab=c") && !DecodeBase64("a-_b") && !DecodeBase64("QR=="));

        // Legacy three fields stay legacy; short lines keep empty fields; an empty fourth field is legacy.
        const auto legacy = ParseResponseLine("Astrid|ScriptQueue|Hello/neutral/Player");
        assert(legacy.Valid() && !legacy.identity.present && legacy.label == "Astrid" && legacy.queue == "ScriptQueue");
        assert(ParseResponseLine("Astrid|ScriptQueue").Valid());
        assert(!ParseResponseLine("Astrid|ScriptQueue|Hi|").identity.present);

        // Typed narrator vs player vs physical namesakes: the label decides nothing.
        const auto typedNarrator = ParseResponseLine(line("The Narrator", "ScriptQueue", "Hi", envelope(narrator, nullptr, nlohmann::json::array())));
        assert(typedNarrator.Valid() && typedNarrator.identity.actor.IsNarrator() && typedNarrator.identity.actor.refId == 0);
        const auto physicalNarrator = ParseResponseLine(line("The Narrator", "ScriptQueue", "Hi",
            envelope({{"id", "ref:mymod.esp|00000D62"}, {"refid", "05000D62"}}, nullptr, nlohmann::json::array())));
        assert(physicalNarrator.Valid() && physicalNarrator.identity.actor.IsPhysical() &&
               physicalNarrator.identity.actor.refId == 0x05000D62);
        const auto typedPlayer = ParseResponseLine(line("Dragonborn", "ScriptQueue", "Hi", envelope(player, astrid, nlohmann::json::array())));
        assert(typedPlayer.Valid() && typedPlayer.identity.actor.IsPlayer() && typedPlayer.identity.listener.refId == 0x0001BDE8);
        const auto physicalPlayerName = ParseResponseLine(line("Player", "ScriptQueue", "Hi", envelope(astrid, nullptr, nlohmann::json::array())));
        assert(physicalPlayerName.Valid() && physicalPlayerName.identity.actor.IsPhysical());
        // The narrator is never identified through RefID 14, and ref 14 is only the typed player.
        assert(!ParseResponseLine(line("N", "q", "p", envelope({{"id", "narrator"}, {"refid", "00000014"}}, nullptr, nlohmann::json::array()))).Valid());
        assert(!ParseResponseLine(line("N", "q", "p", envelope({{"id", "ref:skyrim.esm|00000014"}, {"refid", "00000014"}}, nullptr, nlohmann::json::array()))).Valid());
        assert(!ParseResponseLine(line("N", "q", "p", envelope({{"id", "player"}, {"refid", "0001BDE8"}}, nullptr, nlohmann::json::array()))).Valid());

        // Null listener stays none; a listener object is exact.
        assert(!typedNarrator.identity.listener.Present());
        // Dynamic identity: FF RefID required, and a ref: key must match the RefID's local id.
        assert(ParseResponseLine(line("Bandit", "ScriptQueue", "Hi", envelope(dynamic, nullptr, nlohmann::json::array()))).Valid());
        assert(!ParseResponseLine(line("Bandit", "q", "p", envelope({{"id", dynKey}, {"refid", "0001BDE8"}}, nullptr, nlohmann::json::array()))).Valid());
        assert(!ParseResponseLine(line("Astrid", "q", "p", envelope({{"id", astridKey}, {"refid", "0001BDE9"}}, nullptr, nlohmann::json::array()))).Valid());
        assert(ParseResponseLine(line("L", "q", "p", envelope({{"id", "ref:light.esl|00000801"}, {"refid", "FE012801"}}, nullptr, nlohmann::json::array()))).Valid());
        assert(!ParseResponseLine(line("A", "q", "p", envelope({{"id", astridKey}, {"refid", "0001bde8"}}, nullptr, nlohmann::json::array()))).Valid());

        // Unsupported or malformed metadata drops the line instead of downgrading to the label.
        auto bad = envelope(astrid, nullptr, nlohmann::json::array());
        bad["response_identity_version"] = 2;
        assert(ParseResponseLine(line("Astrid", "q", "p", bad)).invalidReason == "identity_version_unsupported");
        bad = envelope(astrid, nullptr, nlohmann::json::array());
        bad["extra"] = true;
        assert(!ParseResponseLine(line("Astrid", "q", "p", bad)).Valid());
        bad = envelope(astrid, nullptr, nlohmann::json::array());
        bad.erase("listener");
        assert(!ParseResponseLine(line("Astrid", "q", "p", bad)).Valid());
        assert(!ParseResponseLine("Astrid|q|p|not base64!").Valid());
        assert(!ParseResponseLine("Astrid|q|p|" + Base64("[1]")).Valid());
        assert(!ParseResponseLine(line("Astrid", "q", "p", envelope(astrid, nullptr, nlohmann::json::array())) + "|x").Valid());
        // A decorated label that disagrees with the envelope is a mismatch, not a second selector.
        assert(ParseResponseLine(line("Astrid [RefID: 0004D6D1]", "q", "p", envelope(astrid, nullptr, nlohmann::json::array()))).invalidReason == "identity_label_mismatch");
        assert(ParseResponseLine(line("Astrid [RefID: 0001BDE8]", "q", "p", envelope(astrid, nullptr, nlohmann::json::array()))).Valid());

        // Targets: zero-based argument after the command; conflicting duplicates and narrator targets rejected.
        const auto attack = ParseResponseLine(line("Astrid", "command", "Attack@Bandit",
            envelope(astrid, nullptr, nlohmann::json::array({{{"arg", 0}, {"id", dynKey}, {"refid", "FF000812"}}}))));
        assert(attack.Valid() && attack.identity.targets.size() == 1 && attack.identity.TargetFor(0)->refId == 0xFF000812);
        assert(!attack.identity.TargetFor(1));
        assert(ParseResponseLine(line("Astrid", "command", "Attack@Bandit", envelope(astrid, nullptr, nlohmann::json::array({
            {{"arg", 0}, {"id", dynKey}, {"refid", "FF000812"}}, {{"arg", 0}, {"id", astridKey}, {"refid", "0001BDE8"}}}))))
                   .invalidReason == "identity_target_conflict");
        assert(!ParseResponseLine(line("Astrid", "command", "Attack@X", envelope(astrid, nullptr, nlohmann::json::array({
            {{"arg", 0}, {"id", "narrator"}, {"refid", nullptr}}})))).Valid());
        assert(!ParseResponseLine(line("Astrid", "command", "Attack@X", envelope(astrid, nullptr, nlohmann::json::array({
            {{"arg", -1}, {"id", astridKey}, {"refid", "0001BDE8"}}})))).Valid());

        // Multi-line packets: each line carries its own identity; a bad line does not poison its neighbours.
        const std::string packet = line("The Narrator", "ScriptQueue", "A", envelope(narrator, nullptr, nlohmann::json::array())) +
            "\r\n" + "Astrid|ScriptQueue|B|%%%" + "\r\n" + line("The Narrator", "ScriptQueue", "C",
            envelope({{"id", "ref:mymod.esp|00000D62"}, {"refid", "05000D62"}}, nullptr, nlohmann::json::array()));
        std::vector<ResponseLine> lines;
        for (std::size_t start = 0; start <= packet.size();) {
            const auto end = packet.find("\r\n", start);
            lines.push_back(ParseResponseLine(std::string_view(packet).substr(start, end == std::string::npos ? std::string::npos : end - start)));
            if (end == std::string::npos) break;
            start = end + 2;
        }
        assert(lines.size() == 3 && lines[0].identity.actor.IsNarrator() && !lines[1].Valid() &&
               lines[2].identity.actor.IsPhysical() && lines[0].label == lines[2].label);

        // Queue carry seam: a bound endpoint over a recycled FF slot whose registry key changed is refused.
        using ActorIdentityUtils::BoundActor;
        using ActorIdentityUtils::BoundActorObservation;
        const BoundActor queued{0xFF000812, dynKey};
        BoundActorObservation recycled{true, 0xFF000812, true, false, "dyn:0f8b2c1e-4a5d-4e6f-9a7b-1c2d3e4f5a6b"};
        assert(!ActorIdentityUtils::StillSameBoundActor(queued, recycled));
        recycled.currentActorKey = dynKey;
        assert(ActorIdentityUtils::StillSameBoundActor(queued, recycled));
    }

    {
        // C8: the generated decorator is terminal, so a "RefID:" inside a display name does not win.
        using ActorTargetIdentifierUtils::Parse;
        const auto named = Parse("RefID: Fan [RefID: 0001BDE8]");
        assert(named.hasRefId && named.refId == 0x0001BDE8 && named.fallbackName == "RefID: Fan");
        assert(Parse("[RefID: 0xFF001234] Bandit").refId == 0xFF001234);
    }

    {
        // C8b: strict decorator grammar. Only a terminal or leading decorator (or a bare legacy reference) binds;
        // trailing or embedded text makes the reference malformed, and a malformed one never selects by label.
        using ActorTargetIdentifierUtils::Parse;
        using ActorTargetIdentifierUtils::IsUnresolvableExplicit;
        using ActorTargetIdentifierUtils::IdentifiesActor;
        assert(Parse("  Alvor [ refid: 0x00013475 ]  ").refId == 0x00013475);
        assert(Parse("Alvor [RefID: 00013475]").fallbackName == "Alvor");
        for (const char* bad : {"Alvor [RefID: 00013475] and Bandit", "Alvor [RefID: 00013475]x",
                                "Alvor RefID: 00013475", "Alvor [RefID: 00013475", "Alvor [RefID: 0001347G]",
                                "Alvor [RefID: 123456789]", "[RefID: 00013475] RefID: 0001BDE8", "RefID: Fan",
                                "Alvor [RefID: 00013475] [Bandit]", "[RefID: 00013475]] Alvor"}) {
            const auto parsed = Parse(bad);
            assert(parsed.hasRefIdMarker && !parsed.hasRefId && IsUnresolvableExplicit(parsed));
            assert(!IdentifiesActor(bad, bad, 0x00013475) && !IdentifiesActor(bad, parsed.fallbackName, 0x00013475));
        }
        assert(Parse("RefID: 0x0001BDE8").refId == 0x0001BDE8 && Parse("RefID: 0001BDE8").fallbackName.empty());
        assert(Parse("[RefID: 0001BDE8]").refId == 0x0001BDE8);
        // Hex-looking display names are names: the label is never read as a reference.
        for (const char* name : {"Babe", "Dead", "123", "Face"}) {
            const auto parsed = Parse(name);
            assert(!parsed.hasRefIdMarker && !parsed.hasRefId && parsed.fallbackName == name);
            assert(IdentifiesActor(name, name, 0x0001BDE8));
        }
        assert(Parse("Babe [RefID: 0001BDE8]").refId == 0x0001BDE8 && Parse("Babe [RefID: 0001BDE8]").fallbackName == "Babe");
        // Role-command argument types decide numeric semantics, never the text's shape.
        using ActorTargetIdentifierUtils::RoleCommandActorArgs;
        using K = ActorTargetIdentifierUtils::RoleActorArgKind;
        assert(RoleCommandActorArgs("Instruction").at(0).kind == K::AgentName);
        assert(RoleCommandActorArgs("RenameNPC").at(0).kind == K::HexRef);
        assert(RoleCommandActorArgs("spawnCharacter").at(0).index == 4);
        assert(RoleCommandActorArgs("ScriptProxy").empty());
    }

    {
        // C8b: response lines never downgrade an envelope shifted behind an empty fourth field.
        using EventIdentityUtils::ParseResponseLine;
        assert(!ParseResponseLine("Astrid|ScriptQueue|Hi||eyJ4IjoxfQ==").Valid());
        assert(ParseResponseLine("Astrid|ScriptQueue|Hi||").Valid());
        assert(ParseResponseLine("Astrid|ScriptQueue|Hi|| \r\n").Valid());
        assert(!ParseResponseLine("Astrid|ScriptQueue|Hi|||x").Valid());
    }

    {
        // C8b: ScriptProxy actor identity v1 inside the command JSON.
        using EventIdentityUtils::ParseScriptProxyIdentity;
        using EventIdentityUtils::ScriptProxyRefValue;
        const nlohmann::json alvor = {{"id", "ref:skyrim.esm|00013475"}, {"refid", "00013475"}};
        const nlohmann::json bandit = {{"id", "dyn:4f1c2a3b-5d6e-4f70-8a9b-0c1d2e3f4a5b"}, {"refid", "FF000812"}};
        // The getter's own reading: strings only, base 0 then base 16.
        assert(ScriptProxyRefValue("0x00013475") == 0x00013475 && ScriptProxyRefValue("FF000812") == 0xFF000812);
        assert(ScriptProxyRefValue("77") == 77 && ScriptProxyRefValue(77) == 0 && ScriptProxyRefValue("zz") == 0);

        const nlohmann::json legacy = {{"cmdID", 6}, {"targetObjectFormId", "0x00013475"}, {"akTarget", "0xFF000812"}};
        assert(!ParseScriptProxyIdentity(legacy).declared && ParseScriptProxyIdentity(legacy).Valid());

        auto declared = legacy;
        declared["actor_identity_version"] = 1;
        declared["actor_targets"] = {{"targetObjectFormId", alvor}, {"akTarget", bandit}};
        const auto ok = ParseScriptProxyIdentity(declared);
        assert(ok.declared && ok.Valid() && ok.targets.size() == 2);
        assert(ok.TargetFor("akTarget")->refId == 0xFF000812 && ok.TargetFor("akTarget")->id.starts_with("dyn:"));
        assert(!ok.TargetFor("akSpell"));

        auto broken = [&](auto mutate) {
            auto command = declared;
            mutate(command);
            const auto parsed = ParseScriptProxyIdentity(command);
            return parsed.declared && !parsed.Valid() && parsed.targets.empty();
        };
        assert(broken([](auto& c) { c.erase("actor_identity_version"); }));
        assert(broken([](auto& c) { c.erase("actor_targets"); }));
        assert(broken([](auto& c) { c["actor_identity_version"] = 2; }));
        assert(broken([](auto& c) { c["actor_identity_version"] = "1"; }));
        assert(broken([](auto& c) { c["actor_targets"] = nlohmann::json::array(); }));
        assert(broken([](auto& c) { c["akTarget"] = "0xFF000813"; }));                     // Mismatched value.
        assert(broken([](auto& c) { c["akTarget"] = 0xFF000812; }));                       // Getters ignore numbers.
        assert(broken([&](auto& c) { c["actor_targets"]["akOther"] = alvor; }));            // Absent parameter.
        assert(broken([&](auto& c) { c["actor_targets"]["cmdID"] = alvor; }));              // Reserved key.
        assert(broken([&](auto& c) { c["chim_binding"] = 1; c["actor_targets"]["chim_binding"] = alvor; }));
        assert(broken([](auto& c) { c["actor_targets"]["akTarget"] = {{"id", "narrator"}, {"refid", nullptr}}; }));
        assert(broken([](auto& c) { c["actor_targets"]["akTarget"]["refid"] = "FF000813"; }));
        assert(broken([](auto& c) { c["actor_targets"]["akTarget"]["extra"] = 1; }));
        assert(!ParseScriptProxyIdentity(nlohmann::json::array()).Valid());
    }

    // Physical diary identity: sixth spawnBook argument, AIBK instance record and unique-id allocation.
    {
        using namespace DiaryBookIdentityUtils;
        const std::string author = "ref:skyrim.esm|0001A694";
        const std::string recipient = "dyn:0f8d5a2c-3b1e-4c7d-9a6f-2e4b8c1d7e90";
        nlohmann::json good{{"identity_version", 1}, {"book_key", "diary:" + author}, {"author_key", author},
                            {"recipient_key", recipient}};
        const auto arg = "b64:" + EventIdentityUtils::Base64(good.dump());
        const auto parsed = ParseIdentityArgument(arg);
        assert(parsed && parsed->bookKey == "diary:" + author && parsed->recipientKey == recipient);
        assert(ParseIdentityArgument(EventIdentityUtils::Base64(good.dump())) == parsed);
        auto bad = [&](auto edit) {
            auto copy = good;
            edit(copy);
            return !ParseIdentityArgument(EventIdentityUtils::Base64(copy.dump()));
        };
        assert(bad([](auto& c) { c["identity_version"] = 2; }));
        assert(bad([](auto& c) { c["book_key"] = "diary:Lydia"; }));                       // Never a title/name.
        assert(bad([](auto& c) { c["author_key"] = "player"; c["book_key"] = "diary:player"; }));
        assert(bad([](auto& c) { c["recipient_key"] = "narrator"; }));                    // Keeper/narrator never physical.
        assert(bad([](auto& c) { c["extra"] = 1; }));
        assert(bad([](auto& c) { c.erase("recipient_key"); }));
        assert(!ParseIdentityArgument("b64:not base64"));
        assert(!ParseIdentityArgument(""));
        assert(RecipientMatches(*parsed, recipient) && RecipientMatches(*parsed, recipient, recipient));
        assert(!RecipientMatches(*parsed, author) && !RecipientMatches(*parsed, recipient, author));
        assert(!RecipientMatches(*parsed, ""));

        // Two same-title books from different authors stay distinct instances with their own text.
        const nlohmann::json otherJson{{"identity_version", 1}, {"book_key", "diary:ref:skyrim.esm|000A2C94"},
                                       {"author_key", "ref:skyrim.esm|000A2C94"}, {"recipient_key", recipient}};
        const auto other = *ParseIdentity(otherJson);
        std::vector<Instance> instances{
            {1, 0x14, 1, *parsed, ContentHash("first"), "first"},
            {2, 0x14, 2, other, ContentHash("second"), "second"},
        };
        const auto bytes = SerializeInstances(instances);
        const auto restored = ParseInstances(bytes);
        assert(restored && *restored == instances);
        assert(FindByUniqueId(*restored, 0x14, 2)->content == "second");
        assert(!FindByUniqueId(*restored, 0x15, 2) && !FindByUniqueId(*restored, 0x14, 0));
        assert(ParseInstances(SerializeInstances({})) && ParseInstances(SerializeInstances({}))->empty());
        auto tampered = bytes;
        tampered[tampered.size() - 5] ^= 1;                                               // Content changed, hash stale.
        assert(!ParseInstances(tampered));
        assert(!ParseInstances(bytes.substr(0, bytes.size() - 1)));
        assert(!ParseInstances(bytes + "x"));
        auto duplicate = instances;
        duplicate[1].uniqueId = 1;
        assert(!ParseInstances(SerializeInstances(duplicate)));
        duplicate = instances;
        duplicate[1].instanceId = 1;
        assert(!ParseInstances(SerializeInstances(duplicate)));
        // v2 keeps a dropped copy's world reference; two instances never share one, v1 records carry none.
        auto dropped = instances;
        dropped[0].referenceFormId = 0xFF000801;
        assert(*ParseInstances(SerializeInstances(dropped)) == dropped);
        dropped[1].referenceFormId = 0xFF000801;
        assert(!ParseInstances(SerializeInstances(dropped)));
        std::string legacyRecord;
        for (int i = 0; i < 4; ++i) legacyRecord.push_back(i == 0 ? 1 : 0);
        legacyRecord += SerializeInstances({instances[0]}).substr(4);
        legacyRecord.resize(legacyRecord.size() - 4);
        assert(ParseInstances(legacyRecord, 1) && ParseInstances(legacyRecord, 1)->front() == instances[0]);
        assert(!ParseInstances(bytes, 1) && !ParseInstances(bytes, 0) && !ParseInstances(bytes, RecordVersion + 1));

        assert(AllocateUniqueId({}) == 1);
        assert(AllocateUniqueId({1, 2, 3}) == 4);
        assert(AllocateUniqueId({0xFFFF}, 0xFFFF) == 1);
        std::set<std::uint16_t> full;
        for (std::uint32_t id = 1; id <= 0xFFFF; ++id) full.insert(static_cast<std::uint16_t>(id));
        assert(!AllocateUniqueId(full));
    }

    {
        // Rechat active_agent_keys: exact verified physical keys; typed narrator, unverified or keyless agents
        // dropped (never replaced); an empty list is still sent as an array.
        using EventIdentityUtils::ActiveAgentCapture;
        const std::string dyn = "dyn:0f8a2c3e-1b4d-4e5f-8a6b-7c8d9e0f1a2b";
        const std::vector<ActiveAgentCapture> captures{
            {false, "Astrid", 0x0001BDE8, "ref:skyrim.esm|0001BDE8", 0x0001BDE8, true},
            {false, "Astrid", 0x02012345, "ref:dawnguard.esm|00012345", 0x02012345, true},
            {false, "Lydia", 0xFF000801, dyn, 0xFF000801, true},
            {true, "The Narrator", 0, "narrator", 0, false},
            {false, "Recycled", 0xFF000802, "dyn:1f8a2c3e-1b4d-4e5f-8a6b-7c8d9e0f1a2b", 0xFF000802, false},
            {false, "Rebound", 0x00013BBD, "ref:skyrim.esm|00013BBD", 0x00013BBE, true},
            {false, "Runtime", 0xFF000803, "runtime:FF000803", 0xFF000803, true},
            {false, "Upper", 0x00013BBF, "ref:Skyrim.esm|00013BBF", 0x00013BBF, true},
            {false, "Astrid", 0x0001BDE8, "ref:skyrim.esm|0001BDE8", 0x0001BDE8, true}};
        const auto lists = EventIdentityUtils::BuildActiveAgentLists(captures);
        assert((lists.keys == std::vector<std::string>{"ref:skyrim.esm|0001BDE8", "ref:dawnguard.esm|00012345", dyn}));
        assert(lists.labels.size() == 8 && lists.labels[0] == "Astrid" && lists.labels[1] == "Astrid");
        EventIdentityUtils::RechatPayloadFields fields;
        fields.speakerKey = "ref:skyrim.esm|0001BDE8";
        fields.activeAgents = lists;
        const auto payload = EventIdentityUtils::BuildRechatPayload(fields);
        assert(payload["active_agent_keys"].size() == 3 && payload["active_agents"].size() == 8);
        assert(!payload.contains("listener_key") && payload["rechat_identity_version"] == 1);
        const auto none = EventIdentityUtils::BuildRechatPayload({});
        assert(none["active_agent_keys"].is_array() && none["active_agent_keys"].empty());
        assert(none["active_agents"].is_array() && none["active_agents"].empty());
    }

    {
        // gamedata rows: typed player only, key kind must match the reference, unkeyed FF deferred,
        // and the change hash separates a recycled FF slot or a new load.
        using namespace ActorIdentityUtils;
        const auto player = CaptureGameDataActor(true, 0, "ref:skyrim.esm|0001BDE8", 3);
        assert(GameDataActorFields(player) == (std::pair<std::string, std::string>{"player", "00000014"}));
        assert(!CaptureGameDataActor(false, 0x0001BDE8, "player", 3).Keyed());
        assert(!CaptureGameDataActor(false, 0x0001BDE8, "narrator", 3).Keyed());
        const auto dyn = "dyn:0f8a2c3e-1b4d-4e5f-8a6b-7c8d9e0f1a2b";
        assert(!CaptureGameDataActor(false, 0x0001BDE8, dyn, 3).Keyed());
        assert(!CaptureGameDataActor(false, 0xFF000801, "ref:skyrim.esm|0001BDE8", 3).Keyed());
        const auto first = CaptureGameDataActor(false, 0xFF000801, dyn, 3);
        assert(GameDataActorFields(first)->second == "FF000801");
        const auto unkeyed = CaptureGameDataActor(false, 0xFF000801, {}, 3);
        assert(unkeyed.Deferred() && !GameDataActorFields(unkeyed));
        // A non-FF reference without a canonical key is held back too, never downgraded to a name-only row.
        const auto unkeyable = CaptureGameDataActor(false, 0x05000ABC, {}, 3);
        assert(unkeyable.Deferred() && unkeyable.Unkeyable() && !unkeyed.Unkeyable() && !player.Deferred());
        nlohmann::json hit, none, unresolved;
        ApplyGameDataAttackTarget(hit, player, "Astrid");
        assert(hit["attack_target_key"] == "player" && hit["attack_target_refid"] == "00000014" && hit["attack_target"] == "Astrid");
        ApplyGameDataAttackTarget(none, std::nullopt, "");
        ApplyGameDataAttackTarget(unresolved, unkeyed, "Bandit");
        for (const auto* row : {&none, &unresolved}) {
            assert((*row)["attack_target_key"].is_null() && (*row)["attack_target_refid"].is_null() && !row->contains("attack_target"));
        }
        const auto recycled = CaptureGameDataActor(false, 0xFF000801, "dyn:7e1d0c4b-2a39-4f6e-9b8a-5c4d3e2f1a0b", 3);
        const auto reloaded = CaptureGameDataActor(false, 0xFF000801, dyn, 4);
        assert(GameDataChangeHash(first, "h") != GameDataChangeHash(recycled, "h"));
        assert(GameDataChangeHash(first, "h") != GameDataChangeHash(reloaded, "h"));
        assert(GameDataChangeHash(first, "h") == GameDataChangeHash(CaptureGameDataActor(false, 0xFF000801, dyn, 3), "h"));
    }

    {
        // Gamedata delivery lanes are scoped by the real GameDataChangeHash prefix, including a static ref: key
        // (which itself contains '|'), a dyn: key and the player; payloads may contain '|' and '#'.
        using namespace ActorIdentityUtils;
        using Cache = GameDataDeliveryCache;
        using namespace std::chrono_literals;
        const std::string staticKey = "ref:skyrim.esm|0001BDE8";
        const std::string dyn = "dyn:6f1c2a4e-8b3d-4c5a-9e7f-0a1b2c3d4e5f";
        const auto staticL1 = CaptureGameDataActor(false, 0x0001BDE8, staticKey, 1);
        const auto staticL2 = CaptureGameDataActor(false, 0x0001BDE8, staticKey, 2);
        const auto dynL1 = CaptureGameDataActor(false, 0xFF000801, dyn, 1);
        const auto dynL2 = CaptureGameDataActor(false, 0xFF000801, dyn, 2);
        const auto playerL1 = CaptureGameDataActor(true, PlayerRefId, {}, 1);
        const auto playerL2 = CaptureGameDataActor(true, PlayerRefId, {}, 2);
        assert(staticL1.Keyed() && dynL1.Keyed() && playerL1.Keyed());
        const std::string payload = "Iron Sword|0x00012EB7|kw#1|x";
        assert(Cache::ScopeOf(GameDataChangeHash(staticL1, payload)) == "ref:skyrim.esm|0001BDE8|0001BDE8|1#");
        assert(Cache::ScopeOf(GameDataChangeHash(dynL1, payload)) == dyn + "|FF000801|1#");
        assert(Cache::ScopeOf(GameDataChangeHash(playerL1, payload)) == "player|00000014|1#");
        assert(Cache::ScopeOf(GameDataChangeHash(staticL1, "")) == "ref:skyrim.esm|0001BDE8|0001BDE8|1#");
        assert(Cache::ScopeOf(GameDataChangeHash(staticL1, payload)) !=
               Cache::ScopeOf(GameDataChangeHash(staticL2, payload)));
        // Unprefixed or non-canonical prefixes share the unscoped lane.
        assert(Cache::ScopeOf(payload).empty() && Cache::ScopeOf("Potion #2|x|y").empty());
        assert(Cache::ScopeOf("k1|FF000801|1#A").empty() && Cache::ScopeOf("|0001BDE8|1#A").empty());
        assert(Cache::ScopeOf("ref:Skyrim.esm|0001BDE8|0001BDE8|1#A").empty());
        assert(Cache::ScopeOf("player|0000014|1#A").empty() && Cache::ScopeOf("player|00000014|#A").empty());
        assert(Cache::ScopeOf("player|0000001G|1#A").empty() && Cache::ScopeOf("player|00000014|1x#A").empty());

        const auto t0 = Cache::Clock::now();
        // A new load starts a fresh lane for every key kind: the earlier load's in-flight ack neither records nor
        // blocks, and a delivered earlier-load hash never suppresses the new load's send.
        const std::vector<std::tuple<GameDataActorIdentity, GameDataActorIdentity, std::uint32_t>> loads{
            {staticL1, staticL2, 0x0001BDE8u}, {dynL1, dynL2, 0xFF000801u}, {playerL1, playerL2, PlayerRefId}};
        for (const auto& [l1, l2, formId] : loads) {
            Cache c;
            const auto h1 = GameDataChangeHash(l1, payload);
            const auto h2 = GameDataChangeHash(l2, payload);
            const auto older = GameDataChangeHash(l1, "B");
            const auto a = c.Begin(formId, h1, false, t0);
            assert(a != 0 && c.Complete(formId, a, h1, true, t0) && c.Delivered(formId, h1));
            const auto stale = c.Begin(formId, older, false, t0);
            assert(stale != 0);
            const auto b = c.Begin(formId, h2, false, t0 + 1s);
            assert(b != 0 && b != stale);  // not blocked by the earlier load's request in flight
            assert(!c.Complete(formId, stale, older, true, t0 + 2s));
            assert(!c.Complete(formId, stale, older, false, t0 + 2s));
            assert(c.Complete(formId, b, h2, true, t0 + 3s) && c.Delivered(formId, h2) && !c.Delivered(formId, h1));
            assert(!c.NeedsRetry(formId, t0 + 999s));
        }
        // Same load, newer payload: one serialized lane (identical prefix, so no new lane).
        {
            Cache c;
            const auto a = c.Begin(1, GameDataChangeHash(staticL1, "A"), false, t0);
            assert(c.Begin(1, GameDataChangeHash(staticL1, "B"), false, t0) == 0);
            assert(!c.Complete(1, a, GameDataChangeHash(staticL1, "A"), true, t0) && c.NeedsRetry(1, t0));
        }

        // Lost callback, recovered by polling NeedsRetry only (the cadence calls it before any Begin): nothing is
        // retried inside the window; at the window NeedsRetry expires the request and the resend gets a new ticket.
        {
            Cache c;
            const auto h = GameDataChangeHash(staticL1, "A");
            const auto lost = c.Begin(2, h, false, t0);
            assert(lost != 0);
            assert(!c.NeedsRetry(2, t0 + 15s) && !c.NeedsRetry(2, t0 + Cache::kPendingWindow - 1s));
            assert(c.NeedsRetry(2, t0 + Cache::kPendingWindow));
            const auto resend = c.Begin(2, h, false, t0 + Cache::kPendingWindow);
            assert(resend != 0 && resend != lost && c.Latest(2) == resend);
            // The lost ticket's late callback can neither record nor clear the newer lane.
            assert(!c.Complete(2, lost, h, true, t0 + 70s) && !c.Complete(2, lost, h, false, t0 + 70s));
            assert(!c.NeedsRetry(2, t0 + 71s) && c.Latest(2) == resend);
            assert(c.Complete(2, resend, h, true, t0 + 72s) && c.Delivered(2, h));
            assert(!c.Complete(2, lost, h, false, t0 + 73s) && c.Delivered(2, h) && !c.NeedsRetry(2, t0 + 999s));
        }
        // Repeated losses count as failures measured from each send: 60 s window, then the 120 s cap.
        {
            Cache c;
            const auto h = GameDataChangeHash(dynL1, "A");
            auto sent = t0;
            assert(c.Begin(3, h, false, sent) != 0);
            const std::vector<std::chrono::seconds> gaps{60s, 60s, 60s, 120s, 120s};
            for (const auto gap : gaps) {
                assert(!c.NeedsRetry(3, sent + gap - 1s));
                assert(c.NeedsRetry(3, sent + gap));
                sent += gap;
                assert(c.Begin(3, h, false, sent) != 0);
            }
            assert(c.Complete(3, c.Latest(3), h, true, sent) && !c.NeedsRetry(3, sent + 999s));
        }
        // Overlapped (Issue) lane lost, then a new load scope on the same FormID.
        {
            Cache c;
            const auto h = GameDataChangeHash(playerL1, "A");
            const auto n2 = GameDataChangeHash(playerL2, "A");
            const auto a = c.Issue(4, h, t0);
            const auto b = c.Issue(4, h, t0 + 1s);
            assert(!c.NeedsRetry(4, t0 + 60s) && c.NeedsRetry(4, t0 + 61s));
            assert(!c.Complete(4, a, h, true, t0 + 62s) && !c.Complete(4, b, h, true, t0 + 62s));
            const auto n = c.Begin(4, n2, false, t0 + 63s);
            assert(n != 0 && c.Complete(4, n, n2, true, t0 + 64s) && !c.NeedsRetry(4, t0 + 999s));
        }
    }

    {
        // Identified diary dispatch: delivery is bound to the dispatch load and, when bound, the same reference.
        using DiaryBookIdentityUtils::DispatchBinding;
        using DiaryBookIdentityUtils::DispatchStillCurrent;
        const DispatchBinding legacy{7, 0x0001BDE8, false, 0};
        assert(DispatchStillCurrent(legacy, 7, false));
        assert(!DispatchStillCurrent(legacy, 8, false));  // same static NPC after a reload: dropped
        const DispatchBinding bound{7, 0x0001BDE8, true, 0x0001BDE8};
        assert(DispatchStillCurrent(bound, 7, true));
        assert(!DispatchStillCurrent(bound, 7, false));  // handle released or key changed
        assert(!DispatchStillCurrent(bound, 8, true));
        assert(!DispatchStillCurrent(DispatchBinding{7, 0x0001BDE8, true, 0xFF000801}, 7, true));
        assert(!DispatchStillCurrent(DispatchBinding{7, 0, false, 0}, 7, false));
    }

    {
        // Menu TTS, log and stream listeners (any rechat depth) share one selector. The typed narrator's engine
        // object is the player, so its decorated identifier would name RefID 00000014.
        using ActorTargetIdentifierUtils::AgentSelector;
        using ActorTargetIdentifierUtils::ProfileAgentView;
        using ActorTargetIdentifierUtils::ProfileHashSource;
        const std::string narratorName = "The Narrator";
        const auto narratorDecorated = ActorIdentityUtils::BuildPromptIdentifier(narratorName, 0x14);
        const auto narratorListener = AgentSelector(true, narratorName, narratorDecorated);
        assert(narratorListener == narratorName);  // QueueInterruptNPC's NARRATOR_NAME guard still skips it
        // Plain name: no RefID lookup, so md5("The Narrator") as on unstable, never the player's ref key.
        assert(!Parse(narratorListener).hasRefIdMarker);
        assert(ProfileHashSource(narratorListener, {}) == narratorName);
        // Even a decorated 00000014 never takes the typed narrator agent's key; the selector is kept as is.
        assert(ProfileHashSource(narratorDecorated, ProfileAgentView{true, true, "ref:skyrim.esm|00000014"}) ==
               narratorDecorated);

        // A physical NPC named "The Narrator" stays decorated and physical: it is interrupted and keeps its key.
        const auto namesake = AgentSelector(false, narratorName, ActorIdentityUtils::BuildPromptIdentifier(narratorName, 0x0001A2B3));
        assert(namesake != narratorName);
        assert(Parse(namesake).hasRefId && Parse(namesake).refId == 0x0001A2B3);
        assert(ProfileHashSource(namesake, ProfileAgentView{true, false, "ref:skyrim.esm|0001A2B3"}) ==
               "ref:skyrim.esm|0001A2B3");
        // Physical agent without a key, or nobody at the RefID: legacy selector string.
        assert(ProfileHashSource(namesake, ProfileAgentView{true, false, ""}) == namesake);
        assert(ProfileHashSource(namesake, {}) == namesake);
        // Bare names never look up a profile key.
        assert(ProfileHashSource("Alvor", ProfileAgentView{true, false, "ref:skyrim.esm|00013475"}) == "Alvor");
    }

    return 0;
}
