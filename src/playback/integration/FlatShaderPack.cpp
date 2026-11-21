#include "playback/integration/FlatShaderPack.h"

#include "playback/integration/ArkeBridge.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace playback::integration {

namespace {

struct PackFile {
    std::string_view path;
    std::string_view text;
};

// Iris renders without the vanilla face shading when oldLighting is off, and separateAo moves the ambient occlusion
// out of the vertex colour into its alpha, where the terrain programs drop it.
constexpr std::string_view Properties = R"(oldLighting=false
separateAo=true
vignette=false
underwaterOverlay=false
dynamicHandLight=false
shadowTerrain=false
shadowTranslucent=false
shadowEntities=false
shadowPlayer=false
shadowBlockEntities=false
)";
constexpr std::string_view Vertex = R"(out vec2 texcoord;
out vec4 glcolor;

void main() {
    gl_Position = ftransform();
    texcoord = (gl_TextureMatrix[0] * gl_MultiTexCoord0).xy;
    glcolor = gl_Color;
}
)";

// FLAT_TEXTURED samples the texture; FLAT_AO_ALPHA marks programs whose vertex alpha is ambient occlusion
// (separateAo), which is left out so only the texture's own alpha cuts out leaves and plants.
constexpr std::string_view Fragment = R"(uniform sampler2D gtexture;

in vec2 texcoord;
in vec4 glcolor;

/* RENDERTARGETS: 0 */
layout(location = 0) out vec4 outColor;

void main() {
    vec4 color = glcolor;
#ifdef FLAT_AO_ALPHA
    color.a = 1.0;
#endif
#ifdef FLAT_TEXTURED
    color *= texture(gtexture, texcoord);
#endif
#ifdef FLAT_AO_ALPHA
    if (color.a < 0.1) discard;
#else
    if (color.a < 0.004) discard;
#endif
    outColor = color;
}
)";

constexpr std::string_view FinalVertex = R"(#version 330 compatibility

out vec2 texcoord;

void main() {
    gl_Position = ftransform();
    texcoord = (gl_TextureMatrix[0] * gl_MultiTexCoord0).xy;
}
)";

constexpr std::string_view FinalFragment = R"(#version 330 compatibility

uniform sampler2D colortex0;

in vec2 texcoord;

layout(location = 0) out vec4 outColor;

void main() { outColor = vec4(texture(colortex0, texcoord).rgb, 1.0); }
)";

constexpr std::string_view Untextured = "#version 330 compatibility\n#include \"/program/flat_fragment.glsl\"\n";
constexpr std::string_view Textured =
    "#version 330 compatibility\n#define FLAT_TEXTURED\n#include \"/program/flat_fragment.glsl\"\n";
constexpr std::string_view Terrain = "#version 330 compatibility\n#define FLAT_TEXTURED\n#define FLAT_AO_ALPHA\n"
                                     "#include \"/program/flat_fragment.glsl\"\n";
constexpr std::string_view SharedVertex = "#version 330 compatibility\n#include \"/program/flat_vertex.glsl\"\n";

constexpr std::array Files{
    PackFile{"shaders/shaders.properties",           Properties          },
    PackFile{"shaders/program/flat_vertex.glsl",     Vertex              },
    PackFile{"shaders/program/flat_fragment.glsl",   Fragment            },
    PackFile{"shaders/final.vsh",                    FinalVertex         },
    PackFile{"shaders/final.fsh",                    FinalFragment       },
    PackFile{"shaders/gbuffers_basic.vsh",           SharedVertex        },
    PackFile{"shaders/gbuffers_basic.fsh",           Untextured          },
    PackFile{"shaders/gbuffers_skybasic.vsh",        SharedVertex        },
    PackFile{"shaders/gbuffers_skybasic.fsh",        Untextured          },
    PackFile{"shaders/gbuffers_skytextured.vsh",     SharedVertex        },
    PackFile{"shaders/gbuffers_skytextured.fsh",     Textured            },
    PackFile{"shaders/gbuffers_clouds.vsh",          SharedVertex        },
    PackFile{"shaders/gbuffers_clouds.fsh",          Textured            },
    PackFile{"shaders/gbuffers_terrain.vsh",         SharedVertex        },
    PackFile{"shaders/gbuffers_terrain.fsh",         Terrain             },
    PackFile{"shaders/gbuffers_water.vsh",           SharedVertex        },
    PackFile{"shaders/gbuffers_water.fsh",           Terrain             },
    PackFile{"shaders/gbuffers_block.vsh",           SharedVertex        },
    PackFile{"shaders/gbuffers_block.fsh",           Textured            },
    PackFile{"shaders/gbuffers_entities.vsh",        SharedVertex        },
    PackFile{"shaders/gbuffers_entities.fsh",        Textured            },
    PackFile{"shaders/gbuffers_hand.vsh",            SharedVertex        },
    PackFile{"shaders/gbuffers_hand.fsh",            Textured            },
    PackFile{"shaders/gbuffers_textured.vsh",        SharedVertex        },
    PackFile{"shaders/gbuffers_textured.fsh",        Textured            },
    PackFile{"shaders/gbuffers_textured_lit.vsh",    SharedVertex        },
    PackFile{"shaders/gbuffers_textured_lit.fsh",    Textured            },
    PackFile{"shaders/gbuffers_weather.vsh",         SharedVertex        },
    PackFile{"shaders/gbuffers_weather.fsh",         Textured            },
    PackFile{"shaders/gbuffers_beaconbeam.vsh",      SharedVertex        },
    PackFile{"shaders/gbuffers_beaconbeam.fsh",      Textured            },
    PackFile{"shaders/gbuffers_spidereyes.vsh",      SharedVertex        },
    PackFile{"shaders/gbuffers_spidereyes.fsh",      Textured            },
    PackFile{"shaders/gbuffers_armor_glint.vsh",     SharedVertex        },
    PackFile{"shaders/gbuffers_armor_glint.fsh",     Textured            },
    PackFile{"shaders/gbuffers_damagedblock.vsh",    SharedVertex        },
    PackFile{"shaders/gbuffers_damagedblock.fsh",    Textured            },
};

bool hasText(std::filesystem::path const& file, std::string_view text) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::string const current{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    return current == text;
}

} // namespace

std::string installFlatShaderPack() {
    std::string const name{FlatShaderPackName};
    auto const        root = arkeShaderPackPath(name);
    if (!root) return {};
    for (auto const& [path, text] : Files) {
        auto const file = *root / std::filesystem::path(path);
        if (hasText(file, text)) continue;
        std::error_code error;
        std::filesystem::create_directories(file.parent_path(), error);
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) return {};
    }
    return name;
}

} // namespace playback::integration
