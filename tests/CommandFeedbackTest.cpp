#include "playback/replay/CommandFeedback.h"

#include <doctest/doctest.h>

using playback::replay::isCommandFeedbackMessage;

// Messages as a 26.60 server sent them, taken from recordings.
TEST_CASE("command feedback: /gamemode's notice to its target is feedback") {
    CHECK(isCommandFeedbackMessage(
        R"({"rawtext":[{"translate":"gameMode.changed","with":{"rawtext":[{"text":"%createWorldScreen.gameMode.creative"}]}}]})"
    ));
    CHECK(isCommandFeedbackMessage(R"({ "rawtext" : [ { "translate" : "commands.time.set" } ] })"));
}

TEST_CASE("command feedback: command output keys, coloured or marked, are feedback") {
    CHECK(isCommandFeedbackMessage("commands.gamemode.success.self"));
    CHECK(isCommandFeedbackMessage("%commands.weather.clear"));
    CHECK(isCommandFeedbackMessage("\xC2\xA7"
                                   "7%chat.type.admin"));
}

TEST_CASE("command feedback: other server messages stay") {
    CHECK_FALSE(isCommandFeedbackMessage("death.attack.fall"));
    CHECK_FALSE(isCommandFeedbackMessage("\xC2\xA7"
                                         "e%multiplayer.player.joined"));
    CHECK_FALSE(isCommandFeedbackMessage("chat.type.announcement"));
    CHECK_FALSE(isCommandFeedbackMessage(R"({"rawtext":[{"text":"commands.gamemode.success.self"}]})"));
    CHECK_FALSE(isCommandFeedbackMessage(R"({"rawtext":[{"translate":"death.attack.generic","with":["Steve"]}]})"));
}
