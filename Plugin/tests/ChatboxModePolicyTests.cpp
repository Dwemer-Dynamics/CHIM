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

    Check(ChatboxModePolicy::ModeAfterSubmission("DIRECTOR"sv) == "STANDARD"sv,
          "Director mode did not reset to Standard");
    Check(ChatboxModePolicy::ModeAfterSubmission("SHOUT"sv) == "SHOUT"sv,
          "Persistent mode changed after submission");

    const auto whisper = ChatboxModePolicy::ParseSubmission("~ Keep this quiet", "STANDARD");
    Check(whisper.symbolOverride && whisper.mode == "WHISPER" && whisper.message == "Keep this quiet",
          "Whisper symbol was not parsed");

    const auto close = ChatboxModePolicy::ParseSubmission("~~ Only you should hear this", "WHISPER");
    Check(close.symbolOverride && close.mode == "CLOSE" && close.message == "Only you should hear this",
          "Long Close symbol did not take precedence over Whisper");

    Check(ChatboxModePolicy::ParseSubmission("!! Everyone, run!", "STANDARD").mode == "SHOUT",
          "Shout symbol was not parsed");
    Check(ChatboxModePolicy::ParseSubmission("@ Describe the room", "STANDARD").mode == "NARRATOR",
          "Narrator symbol was not parsed");
    Check(ChatboxModePolicy::ParseSubmission("> Have Lydia inspect the doorway", "STANDARD").mode == "DIRECTOR",
          "Director symbol was not parsed");
    Check(ChatboxModePolicy::ParseSubmission("# Give me 1000 gold", "STANDARD").mode == "CHEATMODE",
          "Cheat symbol was not parsed");
    Check(ChatboxModePolicy::ParseSubmission("** Warn them about the dragon", "STANDARD").mode == "AUTOCHAT",
          "Auto Chat symbol was not parsed");

    const auto injectionLog = ChatboxModePolicy::ParseSubmission("((A dragon lands nearby.))", "STANDARD");
    Check(injectionLog.symbolOverride && injectionLog.mode == "INJECTION_LOG" &&
              injectionLog.message == "A dragon lands nearby.",
          "Event Inject wrapper was not parsed");

    const auto injectionChat = ChatboxModePolicy::ParseSubmission("(A dragon lands nearby.)", "STANDARD");
    Check(injectionChat.symbolOverride && injectionChat.mode == "INJECTION_CHAT" &&
              injectionChat.message == "A dragon lands nearby.",
          "Inject and Chat wrapper was not parsed");

    const auto standard = ChatboxModePolicy::ParseSubmission("Hello there", "SHOUT");
    Check(!standard.symbolOverride && standard.mode == "SHOUT" && standard.message == "Hello there",
          "Unprefixed input did not preserve the selected mode");

    const auto emptyShortcut = ChatboxModePolicy::ParseSubmission("~~   ", "STANDARD");
    Check(emptyShortcut.symbolOverride && emptyShortcut.message.empty(),
          "Symbol-only input must remain empty so submission can be rejected");

    std::cout << "Chatbox mode policy tests passed" << std::endl;
    return 0;
}
