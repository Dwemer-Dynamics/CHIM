#pragma once

#include <string_view>

namespace RE
{
    class Actor;
    class TESObjectBOOK;
    class TESObjectREFR;
}

namespace DynamicDiaryBook
{
    bool ActorCarriesDiary(RE::Actor* a_actor, std::string_view a_title);
    bool IsPhysicalDiaryBook(const RE::TESObjectBOOK* a_book);
    bool StoreDiaryText(std::string_view a_title, std::string_view a_content);
    bool QueueReadableBook(RE::TESObjectREFR* a_reference, RE::TESObjectBOOK* a_book, std::string_view a_title);
    void OnBookMenuClosed();
}
