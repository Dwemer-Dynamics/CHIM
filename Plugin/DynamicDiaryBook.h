#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "DiaryBookIdentityUtils.h"

namespace RE
{
    class Actor;
    class ExtraDataList;
    class TESObjectBOOK;
    class TESObjectREFR;
}

namespace DynamicDiaryBook
{
    // What a read or upload of one identified copy reports: its identity, logical instance and content version.
    struct IdentifiedDiary
    {
        DiaryBookIdentityUtils::DiaryIdentity identity;
        std::uint32_t instanceId{ 0 };
        std::uint64_t contentHash{ 0 };
        std::string content;
        std::string title;
    };

    bool ActorCarriesDiary(RE::Actor* a_actor, std::string_view a_title);
    bool IsPhysicalDiaryBook(const RE::TESObjectBOOK* a_book);
    bool StoreDiaryText(std::string_view a_title, std::string_view a_content);
    bool QueueReadableBook(RE::TESObjectREFR* a_reference, RE::TESObjectBOOK* a_book, std::string_view a_title,
                           IdentifiedDiary* a_read = nullptr);
    void OnBookMenuClosed();

    // Identified physical diaries: one immutable instance per spawned copy, bound to its engine ExtraUniqueID.
    bool ActorCarriesIdentifiedDiary(RE::Actor* a_actor, const DiaryBookIdentityUtils::DiaryIdentity& a_identity);
    std::vector<IdentifiedDiary> CarriedIdentifiedDiaries(RE::Actor* a_actor);
    // Uploads the copy's own text with book_key, author_key, recipient_key, book_instance, content_version and
    // reader_key so the server never resolves an identified diary by title.
    void QueueIdentifiedDiaryUpload(const IdentifiedDiary& a_diary, std::string a_readerKey,
                                    std::string a_requestToken = {});
    // Creates the copy through an engine reference picked up by the recipient; fails closed without association.
    bool DeliverIdentifiedDiary(RE::Actor* a_recipient, std::string_view a_title,
                                const DiaryBookIdentityUtils::DiaryIdentity& a_identity, std::string a_content);
    // Reads with no world reference (inventory reads) carry only the engine unique id.
    bool QueueInventoryDiaryRead(std::uint32_t a_baseFormID, std::uint16_t a_uniqueID,
                                 IdentifiedDiary* a_read = nullptr);
    void OnDiaryContainerChanged(std::uint32_t a_oldContainer, std::uint32_t a_newContainer,
                                 std::uint32_t a_baseObject, std::uint16_t a_uniqueID, std::uint32_t a_reference);
    std::string SerializeInstances();
    // Replaces all instance state; a missing or malformed record leaves this save with no identified diaries.
    void RestoreInstances(std::string_view a_record, bool a_present, std::uint32_t a_version,
                          bool (*a_resolve)(std::uint32_t, std::uint32_t&));
    void Revert();
}
