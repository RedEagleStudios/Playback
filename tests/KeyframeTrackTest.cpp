#include "playback/keyframe/KeyframeTrack.h"

#include <doctest/doctest.h>

#include <cmath>

namespace model = playback::state::editing::model;
using playback::keyframe::KeyframeTrack;

namespace {

model::CameraKeyframe key(float x, float yaw, model::CameraInterpolationType type) {
    model::CameraKeyframe result;
    result.position          = {x, 64.0f, 0.0f};
    result.yaw               = yaw;
    result.interpolationType = type;
    return result;
}

} // namespace

TEST_CASE("keyframe track: no pose outside the keys or without keys") {
    CHECK_FALSE(KeyframeTrack({}).createChange(0.0L).has_value());

    KeyframeTrack track({
        {10, key(0.0f,  0.0f, model::CameraInterpolationType::Linear)},
        {20, key(10.0f, 0.0f, model::CameraInterpolationType::Linear)},
    });
    CHECK_FALSE(track.createChange(9.5L).has_value());
    CHECK_FALSE(track.createChange(20.5L).has_value());
}

TEST_CASE("keyframe track: keys give their own pose") {
    KeyframeTrack track({
        {0,  key(1.0f, 30.0f, model::CameraInterpolationType::Smooth)},
        {10, key(5.0f, 60.0f, model::CameraInterpolationType::Smooth)},
        {20, key(9.0f, 90.0f, model::CameraInterpolationType::Smooth)},
    });
    auto const    middle = track.createChange(10.0L);
    REQUIRE(middle.has_value());
    CHECK(middle->position.x == doctest::Approx(5.0f));
    CHECK(middle->yaw == doctest::Approx(60.0f));
}

TEST_CASE("keyframe track: linear keys interpolate evenly") {
    KeyframeTrack track({
        {0,  key(0.0f,  0.0f,  model::CameraInterpolationType::Linear)},
        {20, key(10.0f, 40.0f, model::CameraInterpolationType::Linear)},
    });
    auto const    quarter = track.createChange(5.0L);
    REQUIRE(quarter.has_value());
    CHECK(quarter->position.x == doctest::Approx(2.5f));
    CHECK(quarter->position.y == doctest::Approx(64.0f));
    CHECK(quarter->yaw == doctest::Approx(10.0f));
}

TEST_CASE("keyframe track: yaw turns the short way across 180 degrees") {
    KeyframeTrack track({
        {0,  key(0.0f, 170.0f,  model::CameraInterpolationType::Linear)},
        {10, key(0.0f, -170.0f, model::CameraInterpolationType::Linear)},
    });
    auto const    quarter = track.createChange(2.5L);
    REQUIRE(quarter.has_value());
    CHECK(quarter->yaw == doctest::Approx(175.0f));
    auto const middle = track.createChange(5.0L);
    REQUIRE(middle.has_value());
    CHECK(std::abs(middle->yaw) == doctest::Approx(180.0f));
}

TEST_CASE("keyframe track: a hold key keeps its pose until the next key") {
    KeyframeTrack track({
        {0,  key(0.0f,  0.0f,  model::CameraInterpolationType::Hold)  },
        {10, key(10.0f, 90.0f, model::CameraInterpolationType::Linear)},
    });
    auto const    before = track.createChange(9.9L);
    REQUIRE(before.has_value());
    CHECK(before->position.x == doctest::Approx(0.0f));
    CHECK(before->yaw == doctest::Approx(0.0f));
    auto const at = track.createChange(10.0L);
    REQUIRE(at.has_value());
    CHECK(at->position.x == doctest::Approx(10.0f));
}

TEST_CASE("keyframe track: ease in-out starts and ends slowly") {
    KeyframeTrack track({
        {0,  key(0.0f,   0.0f, model::CameraInterpolationType::EaseInOut)},
        {20, key(100.0f, 0.0f, model::CameraInterpolationType::EaseInOut)},
    });
    CHECK(track.createChange(5.0L)->position.x == doctest::Approx(6.25f));
    CHECK(track.createChange(10.0L)->position.x == doctest::Approx(50.0f));
    CHECK(track.createChange(15.0L)->position.x == doctest::Approx(93.75f));
}

TEST_CASE("keyframe track: smooth keys pass through every key") {
    KeyframeTrack track({
        {0,  key(0.0f,  0.0f, model::CameraInterpolationType::Smooth)},
        {10, key(10.0f, 0.0f, model::CameraInterpolationType::Smooth)},
        {20, key(0.0f,  0.0f, model::CameraInterpolationType::Smooth)},
    });
    CHECK(track.createChange(9.999L)->position.x == doctest::Approx(10.0f).epsilon(0.01));
    auto const between = track.createChange(5.0L)->position.x;
    CHECK(between > 0.0f);
    CHECK(between < 10.0f);
}
