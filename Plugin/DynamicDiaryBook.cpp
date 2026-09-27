#include "DynamicDiaryBook.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

#include "Globals.h"
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
    };

    std::optional<PendingDiaryBook> g_pendingDiaryBook;
    std::unique_ptr<RE::BSString> g_activeDiaryDescription;
    bool g_suppressNextDiaryRead{ false };

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

    bool QueueReadableBook(RE::TESObjectREFR* a_reference, RE::TESObjectBOOK* a_book, std::string_view a_title)
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
        const auto textPath = GetDiaryTextPath(title);
        std::ifstream input(textPath, std::ios::binary);
        if (!input) {
            logger::warn("[PHYSICAL_DIARY] Missing cached text for '{}': {}", title, textPath.string());
            return true;
        }
        std::ostringstream contentBuffer;
        contentBuffer << input.rdbuf();
        const std::string content = contentBuffer.str();
        if (content.empty()) {
            logger::warn("[PHYSICAL_DIARY] Cached text is empty for '{}'", title);
            return true;
        }

        g_pendingDiaryBook = PendingDiaryBook{
            a_reference ? a_reference->GetFormID() : 0,
            a_book->GetFormID(),
            BuildBookDescription(title, content),
            title
        };
        RE::UIMessageQueue::GetSingleton()->AddMessage(
            RE::BookMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
        logger::info("[PHYSICAL_DIARY] Waiting for the original Book Menu to close before displaying '{}'", title);

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
}
