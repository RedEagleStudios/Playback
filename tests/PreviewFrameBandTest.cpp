#include "playback/editor/graphics/PreviewFrameBand.h"

#include <doctest/doctest.h>

namespace graphics = playback::editor::graphics;

TEST_CASE("preview frame band: a 2.39:1 frame takes a centred band of a 16:9 window at full width") {
    auto const band = graphics::letterboxViewport({0.0f, 0.0f, 1920.0f, 1080.0f}, 2.39f);
    REQUIRE(band.has_value());
    CHECK(band->x == 0.0f);
    CHECK(band->width == 1920.0f);
    CHECK(band->height == 803.0f);
    CHECK(band->y == 138.0f);
    CHECK(band->width / band->height == doctest::Approx(2.39f).epsilon(0.002));
}

TEST_CASE("preview frame band: the band keeps the window's own offset") {
    auto const band = graphics::letterboxViewport({10.0f, 20.0f, 1600.0f, 848.0f}, 21.0f / 9.0f);
    REQUIRE(band.has_value());
    CHECK(band->x == 10.0f);
    CHECK(band->height == 686.0f);
    CHECK(band->y == 20.0f + 81.0f);
}

TEST_CASE("preview frame band: a frame no wider than the window needs no band") {
    CHECK_FALSE(graphics::letterboxViewport({0.0f, 0.0f, 1920.0f, 1080.0f}, 16.0f / 9.0f).has_value());
    CHECK_FALSE(graphics::letterboxViewport({0.0f, 0.0f, 1920.0f, 1080.0f}, 9.0f / 16.0f).has_value());
    CHECK_FALSE(graphics::letterboxViewport({0.0f, 0.0f, 0.0f, 0.0f}, 2.39f).has_value());
}
