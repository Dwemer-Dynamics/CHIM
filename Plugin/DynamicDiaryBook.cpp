#include "DynamicDiaryBook.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "Globals.h"
#include "PlaythroughSession.h"
#include "HTTPUploader.h"
#include "ThreadPool.h"
#include "RE/Skyrim.h"
#include "md5.h"

namespace RE
{
    // Supply the relocation implementation omitted from the pinned CommonLib 7.2 source archive.
    void BookMenu::OpenBookMenu(const BSString& description, const ExtraDataList* extraList, TESObjectREFR* reference,
                                TESObjectBOOK* book, const NiPoint3& position, const NiMatrix3& rotation, float scale,
                                bool useDefaultPosition)
    {
        using func_t = decltype(&BookMenu::OpenBookMenu);
        static REL::Relocation<func_t> func{ RELOCATION_ID(50122, 51053) };
        func(description, extraList, reference, book, position, rotation, scale, useDefaultPosition);
    }
}

namespace
{
    constexpr RE::FormID kDiaryBookLocalFormID = 0x045CEF;

    struct PendingDiaryBook
    {
        RE::FormID referenceFormID{ 0 };
        RE::FormID bookFormID{ 0 };
        std::string description;
        std::string title;
        std::uint32_t loadGeneration{ 0 };
    };

    std::optional<PendingDiaryBook> g_pendingDiaryBook;
    std::mutex g_instanceLock;
    std::vector<DiaryBookIdentityUtils::Instance> g_instances;
    std::uint32_t g_nextInstanceId{ 1 };
    std::uint32_t g_loadGeneration{ 0 };
    std::unique_ptr<RE::BSString> g_activeDiaryDescription;
    bool g_suppressNextDiaryRead{ false };

    // The single in-flight native delivery; the container event for its pickup reports the engine's id.
    struct PendingSpawn
    {
        RE::FormID referenceFormID{ 0 };
        RE::FormID recipientFormID{ 0 };
        std::uint16_t uniqueID{ 0 };
    };
    std::mutex g_spawnLock;
    std::optional<PendingSpawn> g_pendingSpawn;

    std::string NormalizeName(std::string_view a_name)
    {
        std::string normalized(a_name);
        normalized.erase(normalized.begin(), std::find_if(normalized.begin(), normalized.end(), [](unsigned char ch) {
            return !std::isspace(ch);
        }));
        normalized.erase(std::find_if(normalized.rbegin(), normalized.rend(), [](unsigned char ch) {
            return !std::isspace(ch);
        }).base(), normalized.end());
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return normalized;
    }

    RE::FormID GetDiaryBookRuntimeFormID()
    {
        return RE::TESDataHandler::GetSingleton()->LookupFormID(kDiaryBookLocalFormID, "AIAgent.esp");
    }

    RE::TESObjectBOOK* GetDiaryBook()
    {
        const auto runtimeFormID = GetDiaryBookRuntimeFormID();
        if (runtimeFormID == 0) {
            return nullptr;
        }

        auto* book = RE::TESForm::LookupByID<RE::TESObjectBOOK>(runtimeFormID);
        if (book) {
            book->value = 5;
        }
        return book;
    }

    std::filesystem::path GetDiaryTextPath(std::string_view a_title)
    {
        return std::filesystem::path("Data/SKSE/Plugins/CHIM/diaries") /
            std::format("{}.txt", md5low(std::string(a_title), true));
    }

    std::string MakeBookTextSafe(std::string_view a_text)
    {
        std::string safe;
        safe.reserve(a_text.size());

        for (std::size_t index = 0; index < a_text.size();) {
            const auto first = static_cast<unsigned char>(a_text[index]);
            if (first < 0x80) {
                safe += static_cast<char>(first);
                ++index;
                continue;
            }

            std::uint32_t codePoint = 0;
            std::size_t sequenceLength = 1;
            if ((first & 0xE0) == 0xC0 && index + 1 < a_text.size()) {
                codePoint = first & 0x1F;
                sequenceLength = 2;
            } else if ((first & 0xF0) == 0xE0 && index + 2 < a_text.size()) {
                codePoint = first & 0x0F;
                sequenceLength = 3;
            } else if ((first & 0xF8) == 0xF0 && index + 3 < a_text.size()) {
                codePoint = first & 0x07;
                sequenceLength = 4;
            } else {
                safe += '?';
                ++index;
                continue;
            }

            bool validSequence = true;
            for (std::size_t offset = 1; offset < sequenceLength; ++offset) {
                const auto continuation = static_cast<unsigned char>(a_text[index + offset]);
                if ((continuation & 0xC0) != 0x80) {
                    validSequence = false;
                    break;
                }
                codePoint = (codePoint << 6) | (continuation & 0x3F);
            }
            if (!validSequence) {
                safe += '?';
                ++index;
                continue;
            }
            index += sequenceLength;

            switch (codePoint) {
                case 0x00A0:
                    safe += ' ';
                    break;
                case 0x2018:
                case 0x2019:
                case 0x2032:
                    safe += '\'';
                    break;
                case 0x201C:
                case 0x201D:
                case 0x2033:
                    safe += '"';
                    break;
                case 0x2013:
                case 0x2014:
                case 0x2212:
                    safe += '-';
                    break;
                case 0x2026:
                    safe += "...";
                    break;
                case 0x00C0:
                case 0x00C1:
                case 0x00C2:
                case 0x00C3:
                case 0x00C4:
                case 0x00C5:
                    safe += 'A';
                    break;
                case 0x00C6:
                    safe += "AE";
                    break;
                case 0x00C7:
                    safe += 'C';
                    break;
                case 0x00C8:
                case 0x00C9:
                case 0x00CA:
                case 0x00CB:
                    safe += 'E';
                    break;
                case 0x00CC:
                case 0x00CD:
                case 0x00CE:
                case 0x00CF:
                    safe += 'I';
                    break;
                case 0x00D1:
                    safe += 'N';
                    break;
                case 0x00D2:
                case 0x00D3:
                case 0x00D4:
                case 0x00D5:
                case 0x00D6:
                case 0x00D8:
                    safe += 'O';
                    break;
                case 0x00D9:
                case 0x00DA:
                case 0x00DB:
                case 0x00DC:
                    safe += 'U';
                    break;
                case 0x00DD:
                    safe += 'Y';
                    break;
                case 0x00DF:
                    safe += "ss";
                    break;
                case 0x00E0:
                case 0x00E1:
                case 0x00E2:
                case 0x00E3:
                case 0x00E4:
                case 0x00E5:
                    safe += 'a';
                    break;
                case 0x00E6:
                    safe += "ae";
                    break;
                case 0x00E7:
                    safe += 'c';
                    break;
                case 0x00E8:
                case 0x00E9:
                case 0x00EA:
                case 0x00EB:
                    safe += 'e';
                    break;
                case 0x00EC:
                case 0x00ED:
                case 0x00EE:
                case 0x00EF:
                    safe += 'i';
                    break;
                case 0x00F1:
                    safe += 'n';
                    break;
                case 0x00F2:
                case 0x00F3:
                case 0x00F4:
                case 0x00F5:
                case 0x00F6:
                case 0x00F8:
                    safe += 'o';
                    break;
                case 0x00F9:
                case 0x00FA:
                case 0x00FB:
                case 0x00FC:
                    safe += 'u';
                    break;
                case 0x00FD:
                case 0x00FF:
                    safe += 'y';
                    break;
                default:
                    safe += '?';
                    break;
            }
        }

        return safe;
    }

    std::string EscapeBookText(std::string_view a_text)
    {
        const auto bookSafeText = MakeBookTextSafe(a_text);
        std::string escaped;
        escaped.reserve(bookSafeText.size() + 64);
        for (const char ch : bookSafeText) {
            switch (ch) {
                case '&':
                    escaped += "&amp;";
                    break;
                case '<':
                    escaped += "&lt;";
                    break;
                case '>':
                    escaped += "&gt;";
                    break;
                case '\r':
                    break;
                case '\n':
                    escaped += "<br>";
                    break;
                default:
                    escaped += ch;
                    break;
            }
        }
        return escaped;
    }

    std::string BuildBookDescription(std::string_view a_title, std::string_view a_content)
    {
        return std::format(
            "<font face='$HandwrittenFont' size='16' color='#1A1008'><p align='left'>"
            "{}<br><br>{}</p></font>",
            EscapeBookText(a_title), EscapeBookText(a_content));
    }

    bool IsDedicatedDiaryBook(const RE::TESObjectBOOK* a_book)
    {
        if (!a_book) {
            return false;
        }

        const auto* diaryBook = GetDiaryBook();
        return diaryBook && a_book->GetFormID() == diaryBook->GetFormID();
    }

    struct UniqueKey
    {
        RE::FormID baseID{ 0 };
        std::uint16_t uniqueID{ 0 };
    };

    // The engine ExtraUniqueID actually present on this copy; 0 ids are never associated.
    std::optional<UniqueKey> ReadUniqueKey(const RE::ExtraDataList* a_extraList)
    {
        const auto* unique = a_extraList ? a_extraList->GetByType<RE::ExtraUniqueID>() : nullptr;
        if (!unique || unique->uniqueID == 0) {
            return std::nullopt;
        }
        return UniqueKey{ unique->baseID, unique->uniqueID };
    }

    // Caller holds g_instanceLock. Exact (baseID, uniqueID) association only; never a title or name match.
    DiaryBookIdentityUtils::Instance* FindInstanceLocked(const RE::ExtraDataList* a_extraList)
    {
        const auto key = ReadUniqueKey(a_extraList);
        if (!key) {
            return nullptr;
        }
        const auto it = std::find_if(g_instances.begin(), g_instances.end(), [&](const auto& instance) {
            return instance.ownerFormId == key->baseID && instance.uniqueId == key->uniqueID;
        });
        return it == g_instances.end() ? nullptr : &*it;
    }

    // Caller holds g_instanceLock. A dropped copy is found by its world reference first, then its unique id.
    DiaryBookIdentityUtils::Instance* FindReferenceInstanceLocked(RE::TESObjectREFR* a_reference)
    {
        if (!a_reference) {
            return nullptr;
        }
        const auto formID = a_reference->GetFormID();
        const auto it = std::find_if(g_instances.begin(), g_instances.end(), [&](const auto& instance) {
            return instance.referenceFormId != 0 && instance.referenceFormId == formID;
        });
        return it != g_instances.end() ? &*it : FindInstanceLocked(&a_reference->extraList);
    }

    DynamicDiaryBook::IdentifiedDiary ToIdentifiedDiary(const DiaryBookIdentityUtils::Instance& a_instance,
                                                        std::string a_title)
    {
        return { a_instance.identity, a_instance.instanceId, a_instance.contentHash, a_instance.content,
                 std::move(a_title) };
    }

    template <class Visitor>
    void ForEachDiaryStack(RE::TESObjectREFR* a_container, Visitor&& a_visit)
    {
        const auto* diaryBook = GetDiaryBook();
        if (!a_container || !diaryBook) {
            return;
        }
        for (const auto& [boundObject, inventoryData] : a_container->GetInventory()) {
            if (!boundObject || boundObject->GetFormID() != diaryBook->GetFormID() || inventoryData.first <= 0 ||
                !inventoryData.second || !inventoryData.second->extraLists) {
                continue;
            }
            for (auto* extraList : *inventoryData.second->extraLists) {
                if (extraList) {
                    a_visit(extraList);
                }
            }
        }
    }

    void QueueDescription(RE::TESObjectREFR* a_reference, RE::TESObjectBOOK* a_book, const std::string& a_title,
                          std::string_view a_content)
    {
        g_pendingDiaryBook = PendingDiaryBook{
            a_reference ? a_reference->GetFormID() : 0,
            a_book->GetFormID(),
            BuildBookDescription(a_title, a_content),
            a_title,
            g_loadGeneration
        };
        RE::UIMessageQueue::GetSingleton()->AddMessage(
            RE::BookMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
    }
}

namespace DynamicDiaryBook
{
    bool IsPhysicalDiaryBook(const RE::TESObjectBOOK* a_book)
    {
        return IsDedicatedDiaryBook(a_book);
    }

    bool ActorCarriesDiary(RE::Actor* a_actor, std::string_view a_title)
    {
        if (!a_actor || a_title.empty()) {
            return false;
        }

        const auto* diaryBook = GetDiaryBook();
        if (!diaryBook) {
            logger::warn("[PHYSICAL_DIARY] Dedicated diary form is unavailable");
            return false;
        }
        const auto diaryFormID = diaryBook->GetFormID();

        const auto expectedTitle = NormalizeName(a_title);
        bool carriesDedicatedDiary = false;
        for (const auto& [boundObject, inventoryData] : a_actor->GetInventory()) {
            if (!boundObject || boundObject->GetFormID() != diaryFormID || inventoryData.first <= 0) {
                continue;
            }

            carriesDedicatedDiary = true;
            const auto& entryData = inventoryData.second;
            if (!entryData) {
                continue;
            }

            const char* displayName = entryData->GetDisplayName();
            if (displayName && NormalizeName(displayName) == expectedTitle) {
                return true;
            }

            if (!entryData->extraLists) {
                continue;
            }
            for (auto* extraList : *entryData->extraLists) {
                if (!extraList) {
                    continue;
                }
                const char* stackName = extraList->GetDisplayName(boundObject);
                if (stackName && NormalizeName(stackName) == expectedTitle) {
                    return true;
                }
            }
        }

        if (carriesDedicatedDiary) {
            logger::warn("[PHYSICAL_DIARY] {} carries a diary book with an unexpected title; treating it as present",
                         a_actor->GetDisplayFullName());
        }
        return carriesDedicatedDiary;
    }

    bool StoreDiaryText(std::string_view a_title, std::string_view a_content)
    {
        if (a_title.empty() || a_content.empty()) {
            return false;
        }

        const auto path = GetDiaryTextPath(a_title);
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            logger::warn("[PHYSICAL_DIARY] Could not create text cache directory: {}", error.message());
            return false;
        }

        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            logger::warn("[PHYSICAL_DIARY] Could not cache text for '{}' at {}", a_title, path.string());
            return false;
        }
        output.write(a_content.data(), static_cast<std::streamsize>(a_content.size()));
        output.close();
        logger::info("[PHYSICAL_DIARY] Cached {} bytes of text for '{}'", a_content.size(), a_title);
        return true;
    }

    bool QueueReadableBook(RE::TESObjectREFR* a_reference, RE::TESObjectBOOK* a_book, std::string_view a_title,
                           IdentifiedDiary* a_read)
    {
        if (!IsDedicatedDiaryBook(a_book) || a_title.empty()) {
            return false;
        }

        if (g_suppressNextDiaryRead) {
            g_suppressNextDiaryRead = false;
            logger::info("[PHYSICAL_DIARY] Accepted rendered reopen for '{}' without another replacement", a_title);
            return true;
        }

        const std::string title(a_title);
        if (a_reference) {
            std::optional<IdentifiedDiary> identified;
            {
                std::scoped_lock lock(g_instanceLock);
                if (const auto* instance = FindReferenceInstanceLocked(a_reference)) {
                    identified = ToIdentifiedDiary(*instance, title);
                }
            }
            if (identified) {
                if (a_read) {
                    *a_read = *identified;
                }
                QueueDescription(a_reference, a_book, title, identified->content);
                logger::info("[PHYSICAL_DIARY] Displaying identified diary instance from a world reference");
                return true;
            }
        }

        // The dedicated diary base renders identified text only through an exact instance association. A copy
        // without one (lost co-save record, legacy title-spawned book) keeps the ordinary base text: a title or
        // display name is not proof of which diary it is, even when unique. Legacy cache files stay on disk.
        logger::warn("[PHYSICAL_DIARY] Diary copy '{}' has no exact instance association; showing the base book text",
                     title);
        return true;
    }

    void OnBookMenuClosed()
    {
        if (!g_pendingDiaryBook) {
            g_activeDiaryDescription.reset();
            return;
        }

        PendingDiaryBook pending = std::move(*g_pendingDiaryBook);
        g_pendingDiaryBook.reset();
        if (pending.loadGeneration != g_loadGeneration) {
            g_activeDiaryDescription.reset();
            logger::info("[PHYSICAL_DIARY] Dropped a pending diary reopen from before the last load");
            return;
        }

        auto* book = RE::TESForm::LookupByID<RE::TESObjectBOOK>(pending.bookFormID);
        auto* reference = pending.referenceFormID != 0
            ? RE::TESForm::LookupByID<RE::TESObjectREFR>(pending.referenceFormID)
            : nullptr;
        if (!book) {
            logger::warn("[PHYSICAL_DIARY] Book form disappeared before '{}' could be displayed", pending.title);
            return;
        }

        const RE::NiPoint3 position = reference ? reference->GetPosition() : RE::NiPoint3{};
        const RE::NiMatrix3 rotation = reference ? RE::NiMatrix3(reference->GetAngle()) : RE::NiMatrix3{};
        const float scale = reference ? reference->GetScale() : 1.0f;
        g_activeDiaryDescription = std::make_unique<RE::BSString>(pending.description.c_str());
        g_suppressNextDiaryRead = true;
        RE::BookMenu::OpenBookMenu(
            *g_activeDiaryDescription, nullptr, reference, book,
            position, rotation, scale, true);
        logger::info("[PHYSICAL_DIARY] Displayed {} bytes of native book text for '{}' after Book Menu closed",
                     pending.description.size(), pending.title);
    }

    bool ActorCarriesIdentifiedDiary(RE::Actor* a_actor, const DiaryBookIdentityUtils::DiaryIdentity& a_identity)
    {
        bool found = false;
        std::scoped_lock lock(g_instanceLock);
        ForEachDiaryStack(a_actor, [&](RE::ExtraDataList* a_extraList) {
            const auto* instance = FindInstanceLocked(a_extraList);
            found = found || (instance && instance->identity.bookKey == a_identity.bookKey);
        });
        return found;
    }

    std::vector<IdentifiedDiary> CarriedIdentifiedDiaries(RE::Actor* a_actor)
    {
        std::vector<IdentifiedDiary> carried;
        auto* book = GetDiaryBook();
        std::scoped_lock lock(g_instanceLock);
        ForEachDiaryStack(a_actor, [&](RE::ExtraDataList* a_extraList) {
            if (const auto* instance = FindInstanceLocked(a_extraList)) {
                const char* name = a_extraList->GetDisplayName(book);
                carried.push_back(ToIdentifiedDiary(*instance, name ? name : ""));
            }
        });
        return carried;
    }

    bool DeliverIdentifiedDiary(RE::Actor* a_recipient, std::string_view a_title,
                                const DiaryBookIdentityUtils::DiaryIdentity& a_identity, std::string a_content)
    {
        auto* book = GetDiaryBook();
        if (!a_recipient || !book || a_title.empty() || a_content.empty() ||
            a_content.size() > DiaryBookIdentityUtils::MaxContentLength) {
            return false;
        }
        const auto hash = DiaryBookIdentityUtils::ContentHash(a_content);
        const auto recipientFormID = a_recipient->GetFormID();

        std::set<std::uint16_t> usedIds;
        {
            std::scoped_lock lock(g_instanceLock);
            bool refreshed = false;
            ForEachDiaryStack(a_recipient, [&](RE::ExtraDataList* a_extraList) {
                auto* instance = FindInstanceLocked(a_extraList);
                if (instance && instance->identity.bookKey == a_identity.bookKey) {
                    instance->content = a_content;
                    instance->contentHash = hash;
                    refreshed = true;
                }
            });
            if (refreshed) {
                logger::info("[PHYSICAL_DIARY] Refreshed identified diary text; physical copy already present");
                return true;
            }
            for (const auto& instance : g_instances) {
                if (instance.ownerFormId == recipientFormID) {
                    usedIds.insert(instance.uniqueId);
                }
            }
        }
        // Unique ids are per owning container across every item it holds, not only diaries.
        for (const auto& [boundObject, inventoryData] : a_recipient->GetInventory()) {
            if (!inventoryData.second || !inventoryData.second->extraLists) {
                continue;
            }
            for (auto* extraList : *inventoryData.second->extraLists) {
                if (const auto key = ReadUniqueKey(extraList); key && key->baseID == recipientFormID) {
                    usedIds.insert(key->uniqueID);
                }
            }
        }

        // The engine creates the reference and owns its initialized extra list; only our own extras are added.
        RE::NiPointer<RE::TESObjectREFR> reference = a_recipient->PlaceObjectAtMe(book, false);
        if (!reference) {
            logger::warn("[PHYSICAL_DIARY] Engine did not create a reference for the identified diary");
            return false;
        }
        auto discardReference = [&](const char* a_reason) {
            // Only the reference created above is cleaned up; it never reached an inventory.
            if (!reference->IsDeleted()) {
                reference->Disable();
                reference->SetDelete(true);
            }
            logger::warn("[PHYSICAL_DIARY] Identified diary not delivered: {}", a_reason);
            return false;
        };
        if (reference->extraList.GetByType<RE::ExtraUniqueID>()) {
            return discardReference("new reference already carries a unique id");
        }
        auto* changes = a_recipient->GetInventoryChanges();
        const auto preferred = changes ? changes->GetNextUniqueID() : std::uint16_t{ 1 };
        const auto uniqueID = DiaryBookIdentityUtils::AllocateUniqueId(usedIds, preferred);
        if (!uniqueID) {
            return discardReference("recipient has no free unique id");
        }
        reference->SetDisplayName(RE::BSFixedString(std::string(a_title)), true);
        reference->extraList.Add(new RE::ExtraUniqueID(recipientFormID, *uniqueID));

        {
            std::scoped_lock spawnLock(g_spawnLock);
            g_pendingSpawn = PendingSpawn{ reference->GetFormID(), recipientFormID, 0 };
        }
        a_recipient->PickUpObject(reference.get(), 1, false, false);
        PendingSpawn captured;
        {
            std::scoped_lock spawnLock(g_spawnLock);
            captured = g_pendingSpawn.value_or(PendingSpawn{});
            g_pendingSpawn.reset();
        }

        // Associate only the copy the engine actually placed in the recipient's inventory.
        std::scoped_lock lock(g_instanceLock);
        std::vector<UniqueKey> exact;
        std::vector<UniqueKey> reported;
        ForEachDiaryStack(a_recipient, [&](RE::ExtraDataList* a_extraList) {
            const auto key = ReadUniqueKey(a_extraList);
            if (!key || FindInstanceLocked(a_extraList)) {
                return;
            }
            if (key->baseID == recipientFormID && key->uniqueID == *uniqueID) {
                exact.push_back(*key);
            } else if (captured.uniqueID != 0 && key->uniqueID == captured.uniqueID) {
                reported.push_back(*key);
            }
        });
        const UniqueKey* chosen = exact.size() == 1 ? &exact.front()
                                : (exact.empty() && reported.size() == 1 ? &reported.front() : nullptr);
        if (!chosen) {
            if (reference->GetParentCell() && !reference->IsDeleted() && !reference->IsDisabled()) {
                return discardReference("the recipient did not pick up the new copy");
            }
            logger::error("[PHYSICAL_DIARY] New diary copy entered inventory without a resolvable unique id; "
                          "it stays unassociated and shows no identified text");
            return false;
        }
        g_instances.push_back(DiaryBookIdentityUtils::Instance{
            g_nextInstanceId++, chosen->baseID, chosen->uniqueID, a_identity, hash, std::move(a_content), 0 });
        logger::info("[PHYSICAL_DIARY] Delivered identified diary instance {} (unique id {})",
                     g_instances.back().instanceId, chosen->uniqueID);
        return true;
    }

    bool QueueInventoryDiaryRead(std::uint32_t a_baseFormID, std::uint16_t a_uniqueID, IdentifiedDiary* a_read)
    {
        auto* book = GetDiaryBook();
        auto* player = RE::PlayerCharacter::GetSingleton();
        // The event names the read book's base form; any other book is not an identified diary.
        if (!book || !player || a_uniqueID == 0 || a_baseFormID != book->GetFormID()) {
            return false;
        }

        // Resolve against the copies the player actually holds; an ambiguous id fails closed.
        std::optional<IdentifiedDiary> match;
        int matches = 0;
        {
            std::scoped_lock lock(g_instanceLock);
            ForEachDiaryStack(player, [&](RE::ExtraDataList* a_extraList) {
                const auto key = ReadUniqueKey(a_extraList);
                if (!key || key->uniqueID != a_uniqueID) {
                    return;
                }
                if (const auto* instance = FindInstanceLocked(a_extraList)) {
                    ++matches;
                    const char* name = a_extraList->GetDisplayName(book);
                    match = ToIdentifiedDiary(*instance, name ? name : "");
                }
            });
        }
        if (matches != 1 || !match) {
            if (matches > 1) {
                logger::warn("[PHYSICAL_DIARY] Inventory read unique id {} (base {:08X}) is ambiguous", a_uniqueID,
                             a_baseFormID);
            }
            return false;
        }
        if (g_suppressNextDiaryRead) {
            g_suppressNextDiaryRead = false;
            return true;
        }
        if (a_read) {
            *a_read = *match;
        }
        QueueDescription(nullptr, book, match->title, match->content);
        logger::info("[PHYSICAL_DIARY] Displaying identified diary instance from inventory");
        return true;
    }

    void OnDiaryContainerChanged(std::uint32_t a_oldContainer, std::uint32_t a_newContainer,
                                 std::uint32_t a_baseObject, std::uint16_t a_uniqueID, std::uint32_t a_reference)
    {
        const auto* book = GetDiaryBook();
        if (!book || a_baseObject != book->GetFormID() || a_oldContainer == a_newContainer) {
            return;
        }
        {
            // The pickup that completes a native delivery reports the id the engine actually kept.
            std::scoped_lock spawnLock(g_spawnLock);
            if (g_pendingSpawn && a_reference != 0 && a_reference == g_pendingSpawn->referenceFormID &&
                a_newContainer == g_pendingSpawn->recipientFormID) {
                g_pendingSpawn->uniqueID = a_uniqueID;
                return;
            }
        }
        if (a_oldContainer == 0 && a_reference == 0) {
            return;
        }
        // Follow the copy by re-reading the actual destination after the move completes.
        const auto generation = g_loadGeneration;
        SKSE::GetTaskInterface()->AddTask([a_oldContainer, a_newContainer, a_uniqueID, a_reference, generation]() {
            if (generation != g_loadGeneration) {
                return;
            }
            std::scoped_lock lock(g_instanceLock);
            const auto source = std::find_if(g_instances.begin(), g_instances.end(), [&](const auto& instance) {
                return a_oldContainer != 0
                    ? instance.referenceFormId == 0 && instance.ownerFormId == a_oldContainer &&
                          instance.uniqueId == a_uniqueID && a_uniqueID != 0
                    : instance.referenceFormId != 0 && instance.referenceFormId == a_reference;
            });
            if (source == g_instances.end()) {
                return;
            }
            if (a_newContainer == 0) {
                // Dropped into the world: the new reference holds the association until it is picked up.
                if (a_reference == 0) {
                    logger::warn("[PHYSICAL_DIARY] Identified diary {} was dropped without a reference; association lost",
                                 source->instanceId);
                    return;
                }
                source->referenceFormId = a_reference;
                logger::info("[PHYSICAL_DIARY] Identified diary {} is now a world reference", source->instanceId);
                return;
            }
            auto* destination = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_newContainer);
            std::vector<UniqueKey> exact;
            std::vector<UniqueKey> sameId;
            ForEachDiaryStack(destination, [&](RE::ExtraDataList* a_extraList) {
                const auto key = ReadUniqueKey(a_extraList);
                const auto* owner = key ? FindInstanceLocked(a_extraList) : nullptr;
                if (!key || (owner && owner != &*source) || key->uniqueID != a_uniqueID) {
                    return;
                }
                (key->baseID == a_newContainer ? exact : sameId).push_back(*key);
            });
            const UniqueKey* chosen = exact.size() == 1 ? &exact.front()
                                    : (exact.empty() && sameId.size() == 1 ? &sameId.front() : nullptr);
            if (!chosen) {
                logger::warn("[PHYSICAL_DIARY] Identified diary {} could not be matched in its destination; "
                             "association kept at its last known owner", source->instanceId);
                return;
            }
            source->ownerFormId = chosen->baseID;
            source->uniqueId = chosen->uniqueID;
            source->referenceFormId = 0;
            logger::info("[PHYSICAL_DIARY] Identified diary instance {} followed its transfer", source->instanceId);
        });
    }

    void QueueIdentifiedDiaryUpload(const IdentifiedDiary& a_diary, std::string a_readerKey,
                                    std::string a_requestToken)
    {
        std::vector<std::pair<std::string, std::string>> metadata{
            { "book_key", a_diary.identity.bookKey },
            { "author_key", a_diary.identity.authorKey },
            { "recipient_key", a_diary.identity.recipientKey },
            { "book_instance", std::to_string(a_diary.instanceId) },
            { "content_version", std::format("{:016x}", a_diary.contentHash) },
            { "reader_key", std::move(a_readerKey) },
        };
        std::string content = "Title: " + a_diary.title + "\n" + a_diary.content;
        // The read happened in this load; a worker running after a reload must not tag it with the new one.
        const auto loadEpoch = PlaythroughSession::Context();
        ThreadPool::getInstance().enqueue(
            "HTTPUploader",
            [content = std::move(content), title = a_diary.title, token = std::move(a_requestToken),
             metadata = std::move(metadata), loadEpoch]() {
                PlaythroughSession::Scope scope(loadEpoch);
                try {
                    HTTPUploader::getInstance().UploadBookContent(content, title, token, "", metadata);
                } catch (const std::exception& e) {
                    logger::error("[PHYSICAL_DIARY] Identified diary upload failed: {}", e.what());
                }
            },
            "UploadBookContent", std::chrono::seconds(45));
    }

    std::string SerializeInstances()
    {
        std::scoped_lock lock(g_instanceLock);
        return DiaryBookIdentityUtils::SerializeInstances(g_instances);
    }

    void RestoreInstances(std::string_view a_record, bool a_present, std::uint32_t a_version,
                          bool (*a_resolve)(std::uint32_t, std::uint32_t&))
    {
        std::vector<DiaryBookIdentityUtils::Instance> restored;
        if (a_present) {
            auto parsed = DiaryBookIdentityUtils::ParseInstances(a_record, a_version);
            if (!parsed) {
                logger::warn("[PHYSICAL_DIARY] Diary instance record is malformed; identified diaries reset");
            } else {
                for (auto& instance : *parsed) {
                    std::uint32_t owner = 0;
                    std::uint32_t reference = 0;
                    const bool referenceResolved = instance.referenceFormId == 0 ||
                        (a_resolve && a_resolve(instance.referenceFormId, reference));
                    if (instance.ownerFormId != 0 && a_resolve && a_resolve(instance.ownerFormId, owner) &&
                        referenceResolved) {
                        instance.ownerFormId = owner;
                        instance.referenceFormId = reference;
                        restored.push_back(std::move(instance));
                    }
                }
            }
        }
        std::scoped_lock lock(g_instanceLock);
        g_instances = std::move(restored);
        g_nextInstanceId = 1;
        for (const auto& instance : g_instances) {
            g_nextInstanceId = (std::max)(g_nextInstanceId, instance.instanceId + 1);
        }
    }

    void Revert()
    {
        {
            std::scoped_lock lock(g_instanceLock);
            g_instances.clear();
            g_nextInstanceId = 1;
        }
        ++g_loadGeneration;
        g_pendingDiaryBook.reset();
        g_suppressNextDiaryRead = false;
    }
}
