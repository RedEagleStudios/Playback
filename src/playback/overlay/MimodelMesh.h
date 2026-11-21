#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// A Mine-imator model (.mimodel) flattened into textured quads, for the overlay's software renderer.
namespace playback::overlay::mimodel {

// Premultiplied RGBA8 texels, one uint32_t each with R in the low byte (the byte order of OverlayImage::rgba).
struct Texture {
    uint32_t              width{};
    uint32_t              height{};
    std::vector<uint32_t> texels;
};

// One face of a shape, already placed in model space: Minecraft's axes (x east, y up, z south) in model units
// (16 to a block), relative to the model's origin.
struct Face {
    std::array<glm::vec3, 4> position;
    // Texel coordinates in `texture`, per corner.
    std::array<glm::vec2, 4> uv;
    // Outward, unit length. Two-sided faces are lit from whichever side the camera sees.
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    // Index into Model::textures; -1 draws flat grey (the texture is missing or failed to decode).
    int32_t texture{-1};
    // Inclusive texel rectangle the face samples from: nearest sampling right on a face's far edge would otherwise pick
    // the neighbouring face's texels.
    int32_t texelMinX{};
    int32_t texelMinY{};
    int32_t texelMaxX{};
    int32_t texelMaxY{};
    // Mine-imator's colour options, inherited down the part tree: multiply, alpha, mix towards a colour, glow.
    glm::vec3 blend{1.0f};
    float     alpha{1.0f};
    glm::vec3 mix{0.0f};
    float     mixAmount{};
    float     emissive{};
    bool      twoSided{};
};

struct Model {
    std::vector<Face>    faces;
    std::vector<Texture> textures;
    // Every file the model was built from (itself and its textures) with the write time it had, so a cache can tell
    // when an edit in Mine-imator or an image editor needs a reload.
    std::vector<std::pair<std::filesystem::path, std::filesystem::file_time_type>> sources;
};

// Parses `file` and decodes its textures. Returns null with `error` set when the file is missing, is not a model, or
// has nothing to draw. Missing textures are not errors: their faces draw grey.
[[nodiscard]] std::shared_ptr<Model const> loadModel(std::filesystem::path const& file, std::string& error);

// UTF-8 text (the editor's strings) to a path.
[[nodiscard]] std::filesystem::path pathFromUtf8(std::string const& text);

} // namespace playback::overlay::mimodel
