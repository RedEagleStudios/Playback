#pragma once

#include "playback/editor/graphics/BlurEffects.h"
#include "playback/editor/graphics/D3D12Compat.h"

#include <memory>
#include <span>

namespace playback::editor::graphics {

// Blurs or lights a finished frame in place on the GPU, one BlurPass after another. Each pass copies the frame to a
// scratch texture and writes the result back through a UAV, so the frame must allow unordered access. Scratches are
// reused from frame to frame: work on one queue runs in order. Each dispatch uses the next of a ring of descriptor
// sets, so a handful of frames can be in flight at once.
class D3D12BlurPass {
public:
    D3D12BlurPass();
    ~D3D12BlurPass();

    D3D12BlurPass(D3D12BlurPass const&)            = delete;
    D3D12BlurPass& operator=(D3D12BlurPass const&) = delete;

    [[nodiscard]] bool ready(ID3D12Device* device);

    // Whether apply() works on this texture: it writes the texture through a typed UAV.
    [[nodiscard]] bool supports(ID3D12Resource* texture);

    // Applies every pass in order to the texture, which is in `state` and is left that way. `frameAspect` (width /
    // height) places the frame the passes are laid out in. False when it could not run; the texture is then untouched
    // and nothing was recorded.
    bool apply(
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource*            texture,
        D3D12_RESOURCE_STATES      state,
        std::span<BlurPass const>  passes,
        float                      frameAspect
    );

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace playback::editor::graphics
