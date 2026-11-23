#include "playback/state/editing/CameraKeys.h"

#include <doctest/doctest.h>

#include <optional>

namespace model = playback::state::editing::model;
using playback::state::editing::addCameraKey;

namespace {

model::CameraKeyframe pose(float x, float yaw) {
    model::CameraKeyframe key;
    key.position = {x, 64.0f, 0.0f};
    key.yaw      = yaw;
    return key;
}

auto viewFov(float fov) {
    return [fov]() -> std::optional<float> { return fov; };
}

} // namespace

TEST_CASE("camera keys: a new track takes the view's FOV with a captured pose") {
    model::CameraEntity camera;
    addCameraKey(camera, 20, pose(1.0f, 90.0f), viewFov(55.0f));

    CHECK(camera.fov == doctest::Approx(55.0f));
    REQUIRE(camera.keysByTick.contains(20));
    CHECK(camera.keysByTick.at(20).position.x == doctest::Approx(1.0f));
    CHECK(camera.keysByTick.at(20).yaw == doctest::Approx(90.0f));
}

TEST_CASE("camera keys: a new track takes the view's FOV without a captured pose") {
    model::CameraEntity camera;
    addCameraKey(camera, 0, std::nullopt, viewFov(40.0f));

    CHECK(camera.fov == doctest::Approx(40.0f));
    CHECK(camera.keysByTick.size() == 1);
}

TEST_CASE("camera keys: only the first key reads the view's FOV") {
    model::CameraEntity camera;
    addCameraKey(camera, 0, pose(0.0f, 0.0f), viewFov(55.0f));
    bool asked = false;
    addCameraKey(camera, 40, pose(10.0f, 0.0f), [&asked]() -> std::optional<float> {
        asked = true;
        return 90.0f;
    });

    CHECK_FALSE(asked);
    CHECK(camera.fov == doctest::Approx(55.0f));
    CHECK(camera.keysByTick.size() == 2);
}

TEST_CASE("camera keys: a keyed FOV is not replaced by the view's") {
    model::CameraEntity camera;
    camera.channels[model::CameraProperty::Fov][0] = {30.0f};
    addCameraKey(camera, 0, pose(0.0f, 0.0f), viewFov(55.0f));

    CHECK(camera.fov == doctest::Approx(70.0f));
}

TEST_CASE("camera keys: without a view the track keeps its FOV") {
    model::CameraEntity camera;
    addCameraKey(camera, 0, pose(0.0f, 0.0f), []() -> std::optional<float> { return std::nullopt; });

    CHECK(camera.fov == doctest::Approx(70.0f));
}

TEST_CASE("camera keys: an uncaptured key copies the track's last key") {
    model::CameraEntity camera;
    addCameraKey(camera, 0, pose(1.0f, 10.0f), viewFov(70.0f));
    addCameraKey(camera, 40, pose(2.0f, 20.0f), viewFov(70.0f));
    addCameraKey(camera, 20, std::nullopt, viewFov(70.0f));

    CHECK(camera.keysByTick.at(20).position.x == doctest::Approx(2.0f));
}

TEST_CASE("camera keys: a person track keeps the subject of the key before") {
    model::CameraEntity camera;
    camera.kind   = model::CameraTrackKind::FirstPerson;
    auto first    = pose(0.0f, 0.0f);
    first.subject = "Steve";
    addCameraKey(camera, 0, first, viewFov(70.0f));
    auto later    = pose(5.0f, 0.0f);
    later.subject = "Alex";
    addCameraKey(camera, 30, later, viewFov(70.0f));

    CHECK(camera.keysByTick.at(30).subject == "Steve");
}

TEST_CASE("camera keys: a keyframe track's keys have no subject") {
    model::CameraEntity camera;
    auto                captured = pose(0.0f, 0.0f);
    captured.subject             = "Steve";
    addCameraKey(camera, 0, captured, viewFov(70.0f));

    CHECK(camera.keysByTick.at(0).subject.empty());
}
