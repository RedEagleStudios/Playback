#pragma once

#include <string_view>

namespace playback::replay {

// A translation key of a chat line a command sends: its output (commands.*), the copy operators get ("[Name: output]",
// chat.type.admin) and /gamemode's notice to its target (gameMode.changed). Leading colour codes and the % that marks
// a key inside a message are skipped.
[[nodiscard]] constexpr bool isCommandFeedbackKey(std::string_view key) {
    constexpr std::string_view Section = "\xC2\xA7"; // §, then one code character
    while (key.size() > Section.size() && key.starts_with(Section)) key.remove_prefix(Section.size() + 1);
    if (key.starts_with('%')) key.remove_prefix(1);
    return key.starts_with("commands.") || key == "chat.type.admin" || key == "gameMode.changed";
}

// A recorded server message (not a player's chat): a translation key, or the JSON rawtext of a text-object message
// ({"rawtext":[{"translate":"gameMode.changed","with":...}]}), which counts when any of its "translate" keys does.
[[nodiscard]] constexpr bool isCommandFeedbackMessage(std::string_view message) {
    if (isCommandFeedbackKey(message)) return true;
    constexpr std::string_view Field      = "\"translate\"";
    constexpr std::string_view Whitespace = " \t\r\n";
    for (auto at = message.find(Field); at != std::string_view::npos; at = message.find(Field, at + Field.size())) {
        auto       rest  = message.substr(at + Field.size());
        auto const colon = rest.find_first_not_of(Whitespace);
        if (colon == std::string_view::npos || rest[colon] != ':') continue;
        rest.remove_prefix(colon + 1);
        auto const open = rest.find_first_not_of(Whitespace);
        if (open == std::string_view::npos || rest[open] != '"') continue;
        rest.remove_prefix(open + 1);
        auto const close = rest.find('"');
        if (close == std::string_view::npos) return false;
        if (isCommandFeedbackKey(rest.substr(0, close))) return true;
    }
    return false;
}

} // namespace playback::replay
