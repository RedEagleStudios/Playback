#include "playback/state/editing/models/PropertyChannel.h"

#include <doctest/doctest.h>

namespace model = playback::state::editing::model;

TEST_CASE("property channel: an empty channel has no value") {
    CHECK_FALSE(model::sampleChannel({}, 0.0).has_value());
    CHECK(model::channelValue({}, "fov", 3.0, 70.0f) == doctest::Approx(70.0f));
}

TEST_CASE("property channel: outside its keys a channel holds the nearest key") {
    model::ValueChannel channel{
        {10, {30.0f, model::KeyEasing::Linear}},
        {20, {50.0f, model::KeyEasing::Linear}},
    };
    CHECK(*model::sampleChannel(channel, 0.0) == doctest::Approx(30.0f));
    CHECK(*model::sampleChannel(channel, 25.0) == doctest::Approx(50.0f));
}

TEST_CASE("property channel: the easing of the key a stretch starts from applies") {
    model::ValueChannel channel{
        {0,  {0.0f, model::KeyEasing::Linear}   },
        {10, {100.0f, model::KeyEasing::Hold}   },
        {20, {0.0f, model::KeyEasing::EaseInOut}},
        {30, {100.0f, model::KeyEasing::Linear} },
    };
    CHECK(*model::sampleChannel(channel, 2.5) == doctest::Approx(25.0f));
    CHECK(*model::sampleChannel(channel, 15.0) == doctest::Approx(100.0f));
    CHECK(*model::sampleChannel(channel, 22.5) == doctest::Approx(15.625f));
    CHECK(*model::sampleChannel(channel, 25.0) == doctest::Approx(50.0f));
}

TEST_CASE("property channel: a keyed property overrides its static value") {
    model::ValueChannels channels;
    channels["fov"] = {
        {0,  {40.0f, model::KeyEasing::Linear}},
        {10, {60.0f, model::KeyEasing::Linear}},
    };
    CHECK(model::channelValue(channels, "fov", 5.0, 70.0f) == doctest::Approx(50.0f));
    CHECK(model::channelValue(channels, "opacity", 5.0, 1.0f) == doctest::Approx(1.0f));
}
