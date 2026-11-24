#include "playback/state/editing/models/CameraLens.h"

#include <doctest/doctest.h>

namespace model = playback::state::editing::model;

TEST_CASE("camera lens: a landscape frame is the full-frame sensor's 24 mm side high") {
    CHECK(model::lensFov(50.0f, 16.0f / 9.0f) == doctest::Approx(26.99f).epsilon(0.001));
    CHECK(model::lensFov(15.0f, 16.0f / 9.0f) == doctest::Approx(77.32f).epsilon(0.001));
    CHECK(model::lensFov(200.0f, 16.0f / 9.0f) == doctest::Approx(6.87f).epsilon(0.001));
}

TEST_CASE("camera lens: a tall frame turns the sensor, so the short side spans its width") {
    // 9:16: 24 mm wide, 42.7 mm high.
    CHECK(model::lensFov(50.0f, 9.0f / 16.0f) == doctest::Approx(46.21f).epsilon(0.001));
    CHECK(model::lensFov(50.0f, 9.0f / 16.0f) > model::lensFov(50.0f, 16.0f / 9.0f));
}

TEST_CASE("camera lens: the dropdown names the lens a FOV came from, at the frame shape it was set for") {
    for (int const focalLength : model::LensFocalLengths) {
        float const fov = model::lensFov(static_cast<float>(focalLength), 9.0f / 16.0f);
        CHECK(model::matchingLens(fov, 9.0f / 16.0f) == focalLength);
    }
    CHECK_FALSE(model::matchingLens(70.0f, 16.0f / 9.0f).has_value());
    // Set on a landscape frame, a 50 mm view is no lens of a 9:16 frame.
    CHECK_FALSE(model::matchingLens(model::lensFov(50.0f, 16.0f / 9.0f), 9.0f / 16.0f).has_value());
}
