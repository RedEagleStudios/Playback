#include "playback/record/ControllerTransitions.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace {

struct Transition {
    std::string controller;
    std::string state;
    std::string nextState;
    std::string stopExpression;
    float       blendOutTime{};
};

std::vector<std::string> states(std::vector<Transition> const& transitions) {
    std::vector<std::string> result;
    for (auto const& transition : transitions) result.push_back(transition.state + ">" + transition.nextState);
    return result;
}

} // namespace

using playback::record::addControllerTransition;

TEST_CASE("controller transitions: distinct transitions to the same target are all kept") {
    std::vector<Transition> transitions;
    addControllerTransition(transitions, {"controller.attack", "attack", "hold", "!q.is_using_item"});
    addControllerTransition(transitions, {"controller.attack", "put_away", "hold", "1.0"});
    addControllerTransition(transitions, {"controller.attack", "attack", "hold", "q.anim_time > 0.5"});

    CHECK(states(transitions) == std::vector<std::string>{"attack>hold", "put_away>hold", "attack>hold"});
}

TEST_CASE("controller transitions: an exact repeat moves to the end and carries the new values") {
    std::vector<Transition> transitions;
    addControllerTransition(transitions, {"controller.ride", "rider_air", "rider_sit", "!q.is_riding", 0.1f});
    addControllerTransition(transitions, {"controller.ride", "rider_sit", "rider_air", "q.is_jumping", 0.1f});
    addControllerTransition(transitions, {"controller.ride", "rider_air", "rider_sit", "!q.is_riding", 0.3f});

    REQUIRE(transitions.size() == 2);
    CHECK(states(transitions) == std::vector<std::string>{"rider_sit>rider_air", "rider_air>rider_sit"});
    CHECK(transitions.back().blendOutTime == doctest::Approx(0.3f));
}

TEST_CASE("controller transitions: the same states on another controller are not a repeat") {
    std::vector<Transition> transitions;
    addControllerTransition(transitions, {"controller.a", "idle", "walk", "q.is_moving"});
    addControllerTransition(transitions, {"controller.b", "idle", "walk", "q.is_moving"});

    CHECK(transitions.size() == 2);
    CHECK(transitions.front().controller == "controller.a");
}
