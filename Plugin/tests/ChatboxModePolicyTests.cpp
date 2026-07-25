#include "ChatboxModePolicy.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

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
    using namespace std::literals;

    Check(ChatboxModePolicy::IsOneShot("SPAWN"sv),
          "Spawn mode must reset after submission");
    Check(ChatboxModePolicy::IsOneShot("DIRECTOR"sv),
          "Director mode must reset after submission");
    Check(!ChatboxModePolicy::IsOneShot("STANDARD"sv),
          "Standard mode must remain selected");
    Check(!ChatboxModePolicy::IsOneShot("WHISPER"sv),
          "Whisper mode must remain selected");
    Check(!ChatboxModePolicy::IsOneShot("CLOSE"sv),
          "Close mode must remain selected");
    Check(!ChatboxModePolicy::IsOneShot("NARRATOR"sv),
          "Narrator mode must remain selected");

    Check(ChatboxModePolicy::ModeAfterSubmission("SPAWN"sv) == "STANDARD"sv,
          "Spawn mode did not reset to Standard");
    Check(ChatboxModePolicy::ModeAfterSubmission("DIRECTOR"sv) == "STANDARD"sv,
          "Director mode did not reset to Standard");
    Check(ChatboxModePolicy::ModeAfterSubmission("SHOUT"sv) == "SHOUT"sv,
          "Persistent mode changed after submission");

    std::cout << "Chatbox mode policy tests passed" << std::endl;
    return 0;
}
